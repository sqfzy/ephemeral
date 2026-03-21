#pragma once

#include <memory>
#include <utility>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace eph::utils {

/**
 * @brief 大页内存分配辅助工具
 *
 * 系统配置要求：
 * - Linux: 需要配置 /proc/sys/vm/nr_hugepages 或使用透明大页
 *          查看可用大页：cat /proc/meminfo | grep Huge
 * - Windows: 需要 SeLockMemoryPrivilege 权限
 *
 * 使用示例：
 * @code
 *   // 创建使用大页的对象
 *   auto data = HugePage::make<std::vector<int>>(1000);
 *
 *   // 就像普通 unique_ptr 一样使用
 *   data->push_back(42);
 * @endcode
 *
 * @note 大页分配失败不会导致程序失败，会自动使用普通内存
 * @note 适用于长期存活的大型数据结构（如缓存、索引、大数组）
 */
class HugePage {
public:
  /**
   * @brief 创建使用大页内存的对象（推荐接口）
   *
   * 尝试在大页内存上构造对象，失败时回退到普通内存。
   * 返回的 unique_ptr 包含自定义删除器，能正确释放大页或普通内存。
   *
   * @tparam T 要创建的对象类型
   * @tparam Args 构造函数参数类型
   * @param args 转发给 T 的构造函数的参数
   * @return std::unique_ptr<T> 智能指针，自动管理对象生命周期
   *
   * @throws 构造函数可能抛出的任何异常（内存分配失败会抛出 std::bad_alloc）
   *
   * @example
   * @code
   *   // 创建 10MB 的缓冲区
   *   auto buffer = HugePage::make<std::array<char, 10*1024*1024>>();
   *
   *   // 创建带参数的对象
   *   auto map = HugePage::make<std::unordered_map<int, std::string>>(1000);
   * @endcode
   */
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

  /**
   * @brief 分配大页内存（底层接口）
   *
   * 尝试分配大页内存，失败时回退到 std::aligned_alloc。
   * 通过输出参数 is_hugepage 告知调用者实际使用的内存类型。
   *
   * @param size 请求的字节数
   * @param alignment 内存对齐要求（如 alignof(T)）
   * @param is_hugepage [out] 输出参数，true 表示成功分配大页，false
   * 表示使用普通内存
   * @return void* 分配的内存指针，失败返回 nullptr
   *
   * @note Linux: 使用 mmap + MAP_HUGETLB 标志尝试 2MB 大页
   * @note Windows: 使用 VirtualAlloc + MEM_LARGE_PAGES 标志
   * @note 其他平台: 直接使用 std::aligned_alloc
   *
   * @warning 返回的指针必须通过 deallocate() 释放，不能使用 free()
   * @warning 此函数不抛异常，分配失败返回 nullptr
   */
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
      return ptr;
    }

    // 大页分配失败，回退到普通内存
    // 使用 aligned_alloc 保证对齐，避免性能问题
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

  /**
   * @brief 释放内存（底层接口）
   *
   * 根据内存类型选择正确的释放方式：
   * - 大页内存：使用 munmap (Linux) 或 VirtualFree (Windows)
   * - 普通内存：使用 std::free
   *
   * @param ptr 要释放的内存指针（由 allocate() 返回）
   * @param size 原始分配的字节数（必须与 allocate 时相同）
   * @param is_hugepage 标识内存类型（来自 allocate 的输出参数）
   *
   * @note ptr 为 nullptr 时安全（无操作）
   * @note 此函数不抛异常，保证 noexcept
   * @warning 参数必须与 allocate() 时一致，否则行为未定义
   */
  static void deallocate(void *ptr, size_t size, bool is_hugepage) noexcept {
    if (!ptr)
      return;

#if defined(__linux__)
    if (is_hugepage) {
      // 释放 mmap 分配的大页内存
      munmap(ptr, size);
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
  /**
   * @brief unique_ptr 自定义删除器
   *
   * 捕获分配时的元数据（大小和类型），确保在对象销毁时：
   * 1. 正确调用析构函数
   * 2. 使用正确的方式释放内存
   *
   * @tparam T 管理的对象类型
   *
   * @note 此结构体会作为 unique_ptr 的一部分存储
   * @note 大小为 sizeof(size_t) + sizeof(bool)，通常为 9 字节（可能对齐到 16
   * 字节）
   */
  template <typename T> struct Deleter {
    size_t size;      // 原始分配大小（字节）
    bool is_hugepage; // true = 大页内存，false = 普通内存

    /**
     * @brief 删除器调用操作符
     *
     * 当 unique_ptr 销毁时自动调用：
     * 1. 调用对象析构函数
     * 2. 根据内存类型释放内存
     *
     * @param ptr 要删除的对象指针
     *
     * @note 保证 noexcept，删除器不能抛异常
     */
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
