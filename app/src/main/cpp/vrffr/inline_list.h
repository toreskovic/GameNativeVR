#pragma once
#include <array>
#include <cstddef>
#include <cassert>
#include <initializer_list>
#include <utility>

namespace ffr {
// Bounded snapshots avoid heap allocations in draw interception. Capacity is
// checked by the caller; non-trivial elements retain normal RAII ownership.
template<class T, std::size_t Capacity> class InlineList {
  std::array<T, Capacity> values_{};
  std::size_t count_ = 0;
public:
  InlineList() = default;
  InlineList(std::initializer_list<T> values) {
    for (const auto &v : values) push_back(v);
  }
  std::size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }
  T *begin() { return values_.data(); }
  T *end() { return begin() + count_; }
  const T *begin() const { return values_.data(); }
  const T *end() const { return begin() + count_; }
  T &operator[](std::size_t i) { assert(i < count_); return values_[i]; }
  const T &operator[](std::size_t i) const { assert(i < count_); return values_[i]; }
  void push_back(T value) {
    assert(count_ < Capacity);
    if (count_ < Capacity) values_[count_++] = std::move(value);
  }
};
}
