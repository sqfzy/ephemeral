/// @file hugepage.hpp
/// @brief Huge-page memory allocation utilities for latency-sensitive data structures.
///
/// Provides transparent huge-page allocation with automatic fallback to
/// standard aligned memory when huge pages are unavailable. On Linux,
/// uses `mmap(MAP_HUGETLB)` to request 2 MB pages; on Windows, uses
/// `VirtualAlloc(MEM_LARGE_PAGES)`. Other platforms fall back to
/// `std::aligned_alloc` unconditionally.
///
/// Typical usage: allocate long-lived, large objects (order-book caches,
/// ring buffers, hash maps) on huge pages to reduce TLB misses and
/// improve memory access latency in HFT hot paths.
///
/// @note Huge-page allocation failure is non-fatal -- the library silently
///       falls back to regular aligned memory and logs a warning.

#pragma once

#include <memory>
#include <utility>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace eph::utils {

namespace detail {

/// @brief Lazily-initialized logger for the huge-page subsystem.
inline const std::shared_ptr<spdlog::logger>& hugepage_logger() {
    static auto l = [] {
        auto lg = spdlog::get("utils.hugepage");
        if (!lg) lg = spdlog::stdout_color_mt("utils.hugepage");
        return lg;
    }();
    return l;
}

} // namespace detail

/// @brief Huge-page memory allocation helper.
///
/// Provides static methods to allocate objects on huge-page-backed memory
/// with automatic fallback to standard aligned memory.
///
/// System requirements:
/// - Linux: configure `/proc/sys/vm/nr_hugepages` or enable transparent
///   huge pages. Check availability: `cat /proc/meminfo | grep Huge`.
/// - Windows: the process needs `SeLockMemoryPrivilege`.
///
/// @code
///   // Create an object backed by huge pages
///   auto data = HugePage::make<std::vector<int>>(1000);
///
///   // Use it exactly like a regular unique_ptr
///   data->push_back(42);
/// @endcode
///
/// @note Allocation failure is non-fatal -- falls back to regular memory.
/// @note Best suited for long-lived, large data structures (caches, indexes,
///       ring buffers).
class HugePage {
public:
  /// @brief Construct an object on huge-page memory (recommended API).
  ///
  /// Attempts placement-new on a huge-page allocation. On failure, falls
  /// back to standard aligned memory. The returned `unique_ptr` carries a
  /// custom deleter that correctly releases either type of memory.
  ///
  /// @tparam T     The object type to construct.
  /// @tparam Args  Constructor argument types.
  /// @param args   Arguments forwarded to `T`'s constructor.
  /// @return A `unique_ptr<T>` with a custom deleter managing the lifetime.
  ///
  /// @throws std::bad_alloc if memory allocation fails entirely.
  /// @throws Any exception thrown by `T`'s constructor (memory is freed
  ///         on construction failure).
  ///
  /// @code
  ///   auto buffer = HugePage::make<std::array<char, 10*1024*1024>>();
  ///   auto map    = HugePage::make<std::unordered_map<int, std::string>>(1000);
  /// @endcode
  template <typename T, typename... Args> static auto make(Args &&...args) {
    size_t size = sizeof(T);
    size_t alignment = alignof(T);
    bool is_hugepage = false;
    size_t allocated_size = 0;

    // 分配内存（可能是大页或普通内存）
    void *mem = allocate(size, alignment, is_hugepage, allocated_size);
    if (!mem) {
      throw std::bad_alloc();
    }

    try {
      // 在分配的内存上构造对象（placement new）
      T *obj = new (mem) T(std::forward<Args>(args)...);

      // 返回带有自定义删除器的 unique_ptr
      // 删除器使用实际分配大小（对齐后），确保正确释放
      return std::unique_ptr<T, Deleter<T>>(obj, Deleter<T>{allocated_size, is_hugepage});
    } catch (...) {
      // 确保如果构造函数抛异常，能正确释放刚分配的内存避免泄露
      deallocate(mem, allocated_size, is_hugepage);
      throw;
    }
  }

  /// @brief Allocate raw huge-page memory (low-level API).
  ///
  /// Tries to allocate huge-page memory; falls back to `std::aligned_alloc`
  /// on failure. The output parameter `is_hugepage` indicates which type
  /// of allocation succeeded.
  ///
  /// @param size               Requested allocation size in bytes.
  /// @param alignment          Required alignment (e.g., `alignof(T)`).
  /// @param is_hugepage        [out] Set to `true` if huge pages were used,
  ///                           `false` if standard memory was used.
  /// @param out_allocated_size [out] Actual allocated size (rounded up for
  ///                           alignment or huge-page boundaries).
  /// @return Pointer to the allocated memory, or `nullptr` on failure.
  ///
  /// @note Linux: uses `mmap` with `MAP_HUGETLB` for 2 MB pages.
  /// @note Windows: uses `VirtualAlloc` with `MEM_LARGE_PAGES`.
  /// @note Other platforms: directly calls `std::aligned_alloc`.
  ///
  /// @warning The returned pointer must be freed via deallocate(), not `free()`.
  /// @warning This function is `noexcept`; on failure it returns `nullptr`.
  static void *allocate(size_t size, size_t alignment,
                        bool &is_hugepage,
                        size_t &out_allocated_size) noexcept {

    is_hugepage = false;

    // 确保对齐值至少为系统的基本最大对齐
    size_t actual_alignment = alignment > alignof(std::max_align_t)
                                  ? alignment
                                  : alignof(std::max_align_t);
    // std::aligned_alloc 强制要求申请的内存大小必须是对齐值的整数倍
    size_t actual_size =
        (size + actual_alignment - 1) & ~(actual_alignment - 1);

    // 输出实际分配大小，供 deallocate 使用
    out_allocated_size = actual_size;

#if defined(__linux__)
    // Linux: 尝试使用 mmap 分配 2MB 大页
    // MAP_HUGETLB: 请求大页内存
    // MAP_ANONYMOUS: 不关联文件，初始化为零
    // MAP_PRIVATE: 进程私有映射
    void *ptr = mmap(nullptr, actual_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

    if (ptr != MAP_FAILED) {
      is_hugepage = true;
      // Round up allocated size to hugepage boundary (2MB) so that
      // munmap receives the correct length matching the kernel's mapping.
      static constexpr size_t kHugePageSize = 2UL * 1024 * 1024;
      if (actual_size > SIZE_MAX - kHugePageSize + 1) {
          // Overflow would occur in rounding, use actual_size as-is
          out_allocated_size = actual_size;
      } else {
          out_allocated_size = (actual_size + kHugePageSize - 1) & ~(kHugePageSize - 1);
      }
      return ptr;
    }

    // 大页分配失败，回退到普通内存
    SPDLOG_LOGGER_WARN(detail::hugepage_logger(),
        "Hugepage allocation failed for {} bytes, falling back to aligned_alloc",
        actual_size);
    return std::aligned_alloc(actual_alignment, actual_size);

#elif defined(_WIN32)
    // Windows: 尝试使用 VirtualAlloc 分配大页
    SIZE_T min_large = GetLargePageMinimum();

    // 只有请求大小 >= 最小大页尺寸时才尝试
    if (min_large > 0 && actual_size >= min_large) {
      void *ptr = VirtualAlloc(nullptr, actual_size,
                               MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES,
                               PAGE_READWRITE);
      if (ptr) {
        is_hugepage = true;
        return ptr;
      }
    }

    // 大页不可用或分配失败，回退到普通内存
    return std::aligned_alloc(actual_alignment, actual_size);

#else
    // 其他平台不支持大页，直接使用标准分配
    return std::aligned_alloc(actual_alignment, actual_size);
#endif
  }

  /// @brief Free memory previously obtained from allocate() (low-level API).
  ///
  /// Selects the correct deallocation strategy based on memory type:
  /// - Huge-page memory: `munmap` (Linux) or `VirtualFree` (Windows).
  /// - Standard memory: `std::free`.
  ///
  /// @param ptr         Pointer returned by allocate().
  /// @param size        The `out_allocated_size` value from the matching allocate() call.
  /// @param is_hugepage The `is_hugepage` flag from the matching allocate() call.
  ///
  /// @note Safe to call with `nullptr` (no-op).
  /// @note Guaranteed `noexcept`.
  /// @warning Parameters must exactly match the original allocate() call;
  ///          mismatched values cause undefined behavior.
  static void deallocate(void *ptr, size_t size, bool is_hugepage) noexcept {
    if (!ptr)
      return;

#if defined(__linux__)
    if (is_hugepage) {
      // 释放 mmap 分配的大页内存
      if (munmap(ptr, size) != 0) [[unlikely]] {
        SPDLOG_LOGGER_ERROR(detail::hugepage_logger(),
            "munmap failed for hugepage at {}, size={}, errno={}",
            ptr, size, errno);
      }
    } else {
      // 释放 aligned_alloc 分配的普通内存
      std::free(ptr);
    }

#elif defined(_WIN32)
    if (is_hugepage) {
      // 释放 VirtualAlloc 分配的大页内���
      // MEM_RELEASE: 释放整个区域，size 参数必须为 0
      VirtualFree(ptr, 0, MEM_RELEASE);
    } else {
      std::free(ptr);
    }

#else
    // 其他平台只使用了 aligned_alloc
    std::free(ptr);
#endif
  }

private:
  /// @brief Custom deleter for huge-page-backed `unique_ptr`.
  ///
  /// Captures the allocation metadata (size and type) so that the correct
  /// deallocation path is taken when the `unique_ptr` is destroyed:
  /// 1. Calls the object's destructor (placement-new symmetry).
  /// 2. Frees the underlying memory via deallocate().
  ///
  /// @tparam T The managed object type.
  ///
  /// @note Stored inline inside the `unique_ptr`. Size is
  ///       `sizeof(size_t) + sizeof(bool)` (typically 9 bytes, padded to 16).
  template <typename T> struct Deleter {
    size_t size;      ///< Allocated size in bytes (from allocate's out_allocated_size).
    bool is_hugepage; ///< `true` = huge-page memory, `false` = standard memory.

    /// @brief Invoked when the owning `unique_ptr` is destroyed.
    ///
    /// Explicitly destructs the object (placement-new symmetry) and then
    /// releases the underlying memory via deallocate().
    ///
    /// @param ptr Pointer to the managed object.
    void operator()(T *ptr) const noexcept {
      if (ptr) {
        // 显式调用析构函数（因为对象是通过 placement new 创建的）
        ptr->~T();

        // 释放底层内存
        deallocate(ptr, size, is_hugepage);
      }
    }
  };
};

} // namespace eph::utils
