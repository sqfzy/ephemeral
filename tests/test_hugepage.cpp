#include <array>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "eph/utils/hugepage.hpp"

using namespace eph::utils;

TEST(HugePageTest, AllocateDeallocateBasic) {
  bool is_hugepage = false;
  size_t size = 4096;

  void *ptr = HugePage::allocate(size, alignof(std::max_align_t), is_hugepage);
  ASSERT_NE(ptr, nullptr);

  // 写入数据验证内存可用
  memset(ptr, 0x42, size);

  // 验证写入
  auto *bytes = static_cast<unsigned char *>(ptr);
  EXPECT_EQ(bytes[0], 0x42);
  EXPECT_EQ(bytes[size - 1], 0x42);

  HugePage::deallocate(ptr, size, is_hugepage);
}

TEST(HugePageTest, AllocateLargeMemory) {
  bool is_hugepage = false;
  size_t size = 4 * 1024 * 1024; // 4 MB

  void *ptr = HugePage::allocate(size, alignof(std::max_align_t), is_hugepage);
  ASSERT_NE(ptr, nullptr);

  // 在 Linux 上，4MB 有可能成功分配大页（2MB 页的倍数）
  // 但不保证，所以不强制检查 is_hugepage 的值

  HugePage::deallocate(ptr, size, is_hugepage);
}

TEST(HugePageTest, DeallocateNullptr) {
  // 应该安全处理 nullptr
  EXPECT_NO_THROW({
    HugePage::deallocate(nullptr, 1024, false);
    HugePage::deallocate(nullptr, 1024, true);
  });
}

TEST(HugePageTest, MakeSimpleType) {
  auto ptr = HugePage::make<int>(42);

  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(*ptr, 42);

  *ptr = 100;
  EXPECT_EQ(*ptr, 100);
}

TEST(HugePageTest, MakeArray) {
  constexpr size_t SIZE = 1024;
  auto ptr = HugePage::make<std::array<int, SIZE>>();

  ASSERT_NE(ptr, nullptr);

  // 写入数据
  for (size_t i = 0; i < SIZE; ++i) {
    (*ptr)[i] = static_cast<int>(i);
  }

  // 验证数据
  for (size_t i = 0; i < SIZE; ++i) {
    EXPECT_EQ((*ptr)[i], static_cast<int>(i));
  }
}

TEST(HugePageTest, MakeVector) {
  auto ptr = HugePage::make<std::vector<int>>(100, 42);

  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(ptr->size(), 100);
  EXPECT_EQ((*ptr)[0], 42);
  EXPECT_EQ((*ptr)[99], 42);

  // 修改和追加
  ptr->push_back(999);
  EXPECT_EQ(ptr->size(), 101);
  EXPECT_EQ(ptr->back(), 999);
}

TEST(HugePageTest, MakeString) {
  auto ptr = HugePage::make<std::string>("Hello, HugePage!");

  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(*ptr, "Hello, HugePage!");

  *ptr += " Modified";
  EXPECT_EQ(*ptr, "Hello, HugePage! Modified");
}

TEST(HugePageTest, MakeComplexType) {
  auto ptr = HugePage::make<std::unordered_map<int, std::string>>();

  ASSERT_NE(ptr, nullptr);
  EXPECT_TRUE(ptr->empty());

  // 插入数据
  (*ptr)[1] = "one";
  (*ptr)[2] = "two";
  (*ptr)[3] = "three";

  EXPECT_EQ(ptr->size(), 3);
  EXPECT_EQ((*ptr)[1], "one");
  EXPECT_EQ((*ptr)[2], "two");
  EXPECT_EQ((*ptr)[3], "three");
}

TEST(HugePageTest, MakeLargeBuffer) {
  // 分配 8 MB 缓冲区
  constexpr size_t SIZE = 8 * 1024 * 1024;
  auto ptr = HugePage::make<std::array<char, SIZE>>();

  ASSERT_NE(ptr, nullptr);

  // 写入测试模式
  for (size_t i = 0; i < SIZE; i += 4096) {
    (*ptr)[i] = static_cast<char>(i % 256);
  }

  // 验证
  for (size_t i = 0; i < SIZE; i += 4096) {
    EXPECT_EQ((*ptr)[i], static_cast<char>(i % 256));
  }
}

TEST(HugePageTest, MultipleAllocations) {
  std::vector<decltype(HugePage::make<int>(0))> pointers;

  // 分配多个对象
  for (int i = 0; i < 10; ++i) {
    auto ptr = HugePage::make<int>(i * 100);
    pointers.push_back(std::move(ptr));
  }

  // 验证所有对象
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(*pointers[i], i * 100);
  }

  // unique_ptr 会自动释放所有内存
}

TEST(HugePageTest, MoveSemantics) {
  auto ptr1 = HugePage::make<std::vector<int>>(100, 42);
  ASSERT_NE(ptr1, nullptr);

  // 移动构造
  auto ptr2 = std::move(ptr1);
  EXPECT_EQ(ptr1, nullptr);
  ASSERT_NE(ptr2, nullptr);
  EXPECT_EQ(ptr2->size(), 100);

  // 移动赋值
  decltype(ptr2) ptr3;
  ptr3 = std::move(ptr2);
  EXPECT_EQ(ptr2, nullptr);
  ASSERT_NE(ptr3, nullptr);
  EXPECT_EQ(ptr3->size(), 100);
}

TEST(HugePageTest, CustomStructure) {
  struct TestData {
    int id;
    std::string name;
    std::vector<double> values;

    TestData(int i, std::string n, std::vector<double> v)
        : id(i), name(std::move(n)), values(std::move(v)) {}
  };

  auto ptr =
      HugePage::make<TestData>(42, "test", std::vector<double>{1.1, 2.2, 3.3});

  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(ptr->id, 42);
  EXPECT_EQ(ptr->name, "test");
  EXPECT_EQ(ptr->values.size(), 3);
  EXPECT_DOUBLE_EQ(ptr->values[0], 1.1);
  EXPECT_DOUBLE_EQ(ptr->values[2], 3.3);
}

TEST(HugePageTest, AlignmentCheck) {
  auto ptr = HugePage::make<int>(0);

  // 检查对齐
  uintptr_t addr = reinterpret_cast<uintptr_t>(ptr.get());
  EXPECT_EQ(addr % alignof(int), 0);
}

TEST(HugePageTest, LargeAlignment) {
  struct alignas(64) CacheAligned {
    char data[64];
  };

  auto ptr = HugePage::make<CacheAligned>();

  uintptr_t addr = reinterpret_cast<uintptr_t>(ptr.get());
  EXPECT_EQ(addr % 64, 0);
}

TEST(HugePageTest, DestructorCalled) {
  static int destructor_count = 0;

  struct Counter {
    Counter() = default;
    ~Counter() { ++destructor_count; }
  };

  destructor_count = 0;

  {
    auto ptr = HugePage::make<Counter>();
    EXPECT_EQ(destructor_count, 0);
  }

  // unique_ptr 销毁时应该调用析构函数
  EXPECT_EQ(destructor_count, 1);
}

TEST(HugePageTest, ExceptionSafety) {
  struct ThrowInConstructor {
    ThrowInConstructor() { throw std::runtime_error("Construction failed"); }
  };

  // 构造函数抛异常时应该正确清理内存
  EXPECT_THROW(
      { auto ptr = HugePage::make<ThrowInConstructor>(); }, std::runtime_error);

  // 程序不应崩溃或泄漏内存
}

TEST(HugePageTest, ZeroSizeAllocation) {
  bool is_hugepage = false;

  // 零大小分配的行为取决于平台
  // 不应该崩溃
  void *ptr = HugePage::allocate(0, alignof(std::max_align_t), is_hugepage);

  if (ptr) {
    HugePage::deallocate(ptr, 0, is_hugepage);
  }

  SUCCEED();
}

TEST(HugePageTest, MemoryInitialization) {
  constexpr size_t SIZE = 1024;
  auto ptr = HugePage::make<std::array<char, SIZE>>();

  // 注意：mmap 的 MAP_ANONYMOUS 保证初始化为零
  // 但 aligned_alloc 不保证，所以这个测试可能因平台而异

  // 至少应该能访问所有字节而不崩溃
  for (size_t i = 0; i < SIZE; ++i) {
    volatile char c = (*ptr)[i];
    (void)c;
  }

  SUCCEED();
}

#if defined(__linux__)
TEST(HugePageTest, CheckHugePageAvailability) {
  // 尝试读取系统大页配置
  std::ifstream meminfo("/proc/meminfo");
  std::string line;
  bool found_hugepages = false;

  while (std::getline(meminfo, line)) {
    if (line.find("HugePages_") != std::string::npos) {
      found_hugepages = true;
      // 仅输出信息，不做断言（因为系统可能未配置大页）
      std::cout << "  [INFO] " << line << std::endl;
    }
  }

  if (!found_hugepages) {
    std::cout << "  [INFO] HugePages not configured on this system"
              << std::endl;
  }

  SUCCEED();
}
#endif

TEST(HugePageTest, StressTest) {
  // 分配和释放多个不同大小的对象
  std::vector<decltype(HugePage::make<std::vector<int>>())> pointers;

  for (int i = 0; i < 50; ++i) {
    size_t size = 100 * (i + 1);
    auto ptr = HugePage::make<std::vector<int>>(size, i);
    pointers.push_back(std::move(ptr));
  }

  // 验证所有对象
  for (size_t i = 0; i < pointers.size(); ++i) {
    EXPECT_EQ(pointers[i]->size(), 100 * (i + 1));
    if (!pointers[i]->empty()) {
      EXPECT_EQ((*pointers[i])[0], static_cast<int>(i));
    }
  }

  // 清理一半
  pointers.erase(pointers.begin(), pointers.begin() + pointers.size() / 2);

  // 再分配一些
  for (int i = 0; i < 25; ++i) {
    auto ptr = HugePage::make<std::vector<int>>(200, 999);
    pointers.push_back(std::move(ptr));
  }

  SUCCEED();
}
