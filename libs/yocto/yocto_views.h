//
// # Yocto/Views: Views and ranges
//
// Yocto/Views provides several views and ranges over data typical of graphics,
// applications.
// Yocto/Views is implemented in `yocto_views.h`.
//

//
// LICENSE:
//
// Copyright (c) 2016 -- 2022 Fabio Pellacini
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
//

#ifndef _YOCTO_VIEWS_H_
#define _YOCTO_VIEWS_H_

// -----------------------------------------------------------------------------
// INCLUDES
// -----------------------------------------------------------------------------

#include <vector>

// -----------------------------------------------------------------------------
// USING DIRECTIVES
// -----------------------------------------------------------------------------
namespace yocto {

// using directives
using std::vector;

}  // namespace yocto

// -----------------------------------------------------------------------------
// ONE-DIMENSIONAL SPAN
// -----------------------------------------------------------------------------
namespace yocto {

// Span similar to std::span. We'll switch to the standard library version when
// present in all compilers.
template <typename T>
struct span {
 public:
  // Constructors
  constexpr span() noexcept : _data{nullptr}, _size{0} {}
  constexpr span(const span&) noexcept = default;
  constexpr span(span&&) noexcept      = default;
  constexpr span(T* data, size_t size) noexcept : _data{data}, _size{size} {}
  constexpr span(T* begin, T* end) noexcept
      : _data{begin}, _size{end - begin} {}
  constexpr span(std::vector<T>& arr) noexcept
      : _data{arr.data()}, _size{arr.size()} {}

  // Assignments
  constexpr span& operator=(const span&) noexcept  = default;
  constexpr span& operator=(span&& other) noexcept = default;

  // Size
  constexpr bool   empty() const noexcept { return _size == 0; }
  constexpr size_t size() const noexcept { return _size; }

  // Access
  constexpr T& operator[](size_t idx) const noexcept { return _data[idx]; }
  constexpr T& front() const noexcept { return _data[0]; }
  constexpr T& back() const noexcept { return _data[_size - 1]; }

  // Iteration
  constexpr T* begin() const noexcept { return _data; }
  constexpr T* end() const noexcept { return _data + _size; }

  // Data access
  constexpr T* data() const noexcept { return _data; }

 private:
  T*     _data = nullptr;
  size_t _size = 0;
};

// Constant span
template <typename T>
using cspan = span<const T>;

}  // namespace yocto

// -----------------------------------------------------------------------------
// N-DIMENSIONAL SPAN
// -----------------------------------------------------------------------------
namespace yocto {

// N-dimensional span similar to std::mdspan. We will switch to the standard
// version as it becomes available.
template <typename T, size_t N>
struct ndspan {
 public:
  // Constructors
  constexpr ndspan() noexcept : _extents{0}, _data{nullptr} {}
  constexpr ndspan(T* data, const vec<size_t, N>& extents) noexcept
      : _data{data}, _extents{extents} {}
  constexpr ndspan(const ndspan& other) noexcept = default;
  constexpr ndspan(ndspan&& other) noexcept      = default;

  // Assignments
  constexpr ndspan& operator=(const ndspan& other) noexcept = default;
  constexpr ndspan& operator=(ndspan&& other) noexcept      = default;

  // Size
  constexpr bool           empty() const noexcept { return size() == 0; }
  constexpr size_t         size() const noexcept { return _size(_extents); }
  constexpr vec<size_t, N> extents() const noexcept { return _extents; }
  constexpr size_t         extent(size_t dimension) const noexcept {
    return _extents[dimension];
  }

  // Access
  constexpr T& operator[](size_t idx) const noexcept { return _data[idx]; }
  constexpr T& operator[](const array<size_t, N>& idx) const noexcept {
    return _data[_index(idx, _extents)];
  }

  // Iteration
  constexpr T* begin() const noexcept { return _data; }
  constexpr T* end() const noexcept { return _data + _size; }

  // Data access
  constexpr T* data() const noexcept { return _data; }

 private:
  T*             _data    = nullptr;
  vec<size_t, N> _extents = {0};

  static size_t _size(const vec<size_t, 1>& extents) { return extents[0]; }
  static size_t _size(const vec<size_t, 2>& extents) {
    return extents[0] * extents[1];
  }
  static size_t _size(const vec<size_t, 3>& extents) {
    return extents[0] * extents[1] * extents[2];
  }
  static size_t _index(
      const vec<size_t, 1>& index, const vec<size_t, 1>& extents) {
    return index[0];
  }
  static size_t _index(
      const vec<size_t, 2>& index, const vec<size_t, 2>& extents) {
    return index[1] * extents[0] + index[0];
  }
  static size_t _index(
      const vec<size_t, 3>& index, const vec<size_t, 3>& extents) {
    return (index[2] * extents[1] + index[1]) * extents[0] + index[0];
  }
};

// Using directives
template <typename T>
using span1d = ndspan<T, 1>;
template <typename T>
using span2d = ndspan<T, 2>;
template <typename T>
using span3d = ndspan<T, 3>;

}  // namespace yocto

// -----------------------------------------------------------------------------
//
//
// IMPLEMENTATION
//
//
// -----------------------------------------------------------------------------

#endif
