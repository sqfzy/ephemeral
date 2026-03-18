#pragma once

#include <concepts>
#include <type_traits>

namespace eph::base {

/**
 * @brief 基础数据约束
 * * 只有满足该约束的类型才能进入无锁队列。
 * 要求：可平凡拷贝（以便 memcpy）且具有默认构造函数。
 */
template <typename T>
concept TrivialData = 
    std::is_trivially_copyable_v<T> && 
    std::default_initializable<T>;

} // namespace common::base
