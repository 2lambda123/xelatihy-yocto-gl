//
// # Yocto/Image : Image utilities
//
// Yocto/Image is a collection of image utilities useful when writing rendering
// algorithms. These include a simple image data structure, color conversion
// utilities and tone mapping, loading and saving functionality, and image
// resizing.
// Yocto/Image is implemented in `yocto_image.h` and `yocto_image.cpp`, and
// depends on `stb_image_resize.h` for image resizing.
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

#ifndef _YOCTO_IMAGE_H_
#define _YOCTO_IMAGE_H_

// -----------------------------------------------------------------------------
// INCLUDES
// -----------------------------------------------------------------------------

#include <utility>
#include <vector>

#include "yocto_color.h"
#include "yocto_math.h"
#include "yocto_ndarray.h"
#include "yocto_noise.h"

// -----------------------------------------------------------------------------
// USING DIRECTIVES
// -----------------------------------------------------------------------------
namespace yocto {

// using directives
using std::vector;

}  // namespace yocto

// -----------------------------------------------------------------------------
// IMAGE UTILITIES
// -----------------------------------------------------------------------------
namespace yocto {

// Aspect ratio
template <typename T = float>
constexpr T image_aspect(const vec2s& extents) {
  return (T)extents[0] / (T)extents[1];
}

// Conversion from/to floats.
template <size_t N, typename T = float>
constexpr array2d<vec<T, N>> byte_to_float(const array2d<vec<byte, N>>& bt) {
  auto fl = array2d<vec<T, N>>{bt.extents()};
  for (auto idx : range(fl.size())) fl[idx] = byte_to_float(bt[idx]);
  return fl;
}
template <typename T, size_t N>
constexpr array2d<vec<byte, N>> float_to_byte(const array2d<vec<T, N>>& fl) {
  auto bt = array2d<vec<byte, N>>{fl.extents()};
  for (auto idx : range(fl.size())) bt[idx] = float_to_byte(fl[idx]);
  return bt;
}
template <typename T, typename Tb>
constexpr void byte_to_float(array2d<T>& fl, const array2d<Tb>& bt) {
  for (auto idx : range(fl.size())) fl[idx] = byte_to_float(bt[idx]);
}
template <typename T, typename Tb>
constexpr void float_to_byte(array2d<Tb>& bt, const array2d<T>& fl) {
  for (auto idx : range(fl.size())) bt[idx] = float_to_byte(fl[idx]);
}

// Conversion between linear and gamma-encoded images.
template <typename T>
constexpr array2d<T> srgb_to_rgb(const array2d<T>& srgb) {
  auto rgb = array2d<T>{srgb.extents()};
  for (auto idx : range(rgb.size())) rgb[idx] = srgb_to_rgb(srgb[idx]);
  return rgb;
}
template <typename T>
constexpr array2d<T> rgb_to_srgb(const array2d<T>& rgb) {
  auto srgb = array2d<T>{rgb.extents()};
  for (auto idx : range(srgb.size())) srgb[idx] = rgb_to_srgb(rgb[idx]);
  return srgb;
}
template <typename T>
constexpr void srgb_to_rgb(array2d<T>& rgb, const array2d<T>& srgb) {
  for (auto idx : range(rgb.size())) rgb[idx] = srgb_to_rgb(srgb[idx]);
}
template <typename T>
constexpr void rgb_to_srgb(array2d<T>& srgb, const array2d<T>& rgb) {
  for (auto idx : range(srgb.size())) srgb[idx] = rgb_to_srgb(rgb[idx]);
}

// Conversion between linear and gamma-encoded images.
template <size_t N, typename T = float>
constexpr array2d<vec<T, N>> srgbb_to_rgb(const array2d<vec<byte, N>>& srgb) {
  auto rgb = array2d<vec<T, N>>{srgb.extents()};
  for (auto idx : range(rgb.size())) rgb[idx] = srgbb_to_rgb<T>(srgb[idx]);
  return rgb;
}
template <typename T, size_t N>
constexpr array2d<vec<byte, N>> rgb_to_srgbb(const array2d<vec<T, N>>& rgb) {
  auto srgb = array2d<vec<byte, N>>{rgb.extents()};
  for (auto idx : range(rgb.size())) srgb[idx] = rgb_to_srgbb(rgb[idx]);
  return srgb;
}
template <typename T, typename Tb>
constexpr void srgbb_to_rgb(array2d<T>& rgb, const array2d<Tb>& srgb) {
  for (auto idx : range(rgb.size())) rgb[idx] = srgbb_to_rgb<float>(srgb[idx]);
}
template <typename T, typename Tb>
constexpr void rgb_to_srgbb(array2d<Tb>& srgb, const array2d<T>& rgb) {
  for (auto idx : range(rgb.size())) srgb[idx] = rgb_to_srgbb(rgb[idx]);
}

// Lookup pixel for evaluation
template <typename T, typename T1, size_t N>
constexpr vec<T, N> lookup_image(
    const array2d<vec<T1, N>>& image, const vec2s& ij, bool as_linear = false) {
  if constexpr (!std::is_same_v<T1, byte>) {
    return as_linear ? srgb_to_rgb(image[ij]) : image[ij];
  } else {
    return as_linear ? srgbb_to_rgb(image[ij]) : byte_to_float(image[ij]);
  }
}

// Evaluates an image at a point `uv`.
template <typename T, typename T1, size_t N>
constexpr vec<T, N> eval_image(const array2d<vec<T1, N>>& image,
    const vec<T, 2>& uv, bool as_linear = false, bool no_interpolation = false,
    bool clamp_to_edge = false) {
  if (image.empty()) return vec<T, N>{0};

  // get image width/height
  auto size = image.extents();

  // get coordinates normalized for tiling
  auto st = (clamp_to_edge ? clamp(uv, 0, 1) : mod(uv, 1)) * size;

  // handle interpolation
  if (no_interpolation) {
    auto ij = clamp((vec2s)st, 0, size - 1);
    return lookup_image(image, ij, as_linear);
  } else {
    auto ij     = clamp((vec2s)st, 0, size - 1);
    auto i1j    = (ij + vec2s{1, 0}) % size;
    auto ij1    = (ij + vec2s{0, 1}) % size;
    auto i1j1   = (ij + vec2s{1, 1}) % size;
    auto [u, v] = st - ij;
    return lookup_image(image, ij, as_linear) * (1 - u) * (1 - v) +
           lookup_image(image, ij1, as_linear) * (1 - u) * v +
           lookup_image(image, i1j, as_linear) * u * (1 - v) +
           lookup_image(image, i1j1, as_linear) * u * v;
  }
}

// Apply tone mapping returning a float or byte image.
template <typename T, size_t N, typename T1>
constexpr array2d<vec<T, N>> tonemap_image(const array2d<vec<T, N>>& image,
    T1 exposure, bool filmic = false, bool srgb = true) {
  auto result = array2d<vec<T, N>>(image.extents());
  for (auto idx : range(image.size())) {
    result[idx] = tonemap(image[idx], (T)exposure, filmic, srgb);
  }
  return result;
}

// Apply tone mapping. If the input image is an ldr, does nothing.
template <typename T, size_t N, typename T1>
constexpr void tonemap_image(array2d<vec<T, N>>& result,
    const array2d<vec<T, N>>& image, T1 exposure, bool filmic = false,
    bool srgb = true) {
  if (image.extents() != result.extents())
    throw std::invalid_argument{"image should be the same size"};
  for (auto idx : range(image.size())) {
    result[idx] = tonemap(image[idx], (T)exposure, filmic, srgb);
  }
}

// Apply tone mapping. If the input image is an ldr, does nothing.
// void tonemap_image(array2d<vec4f>& ldr, const array2d<vec4f>& image,
//     float exposure, bool filmic = false, bool srgb= true);
// Apply tone mapping using multithreading for speed.
void tonemap_image_mt(array2d<vec4f>& ldr, const array2d<vec4f>& image,
    float exposure, bool filmic = false, bool srgb = true);

// Get/Set region
template <typename T>
constexpr void get_region(array2d<T>& region, const array2d<T>& image,
    const vec2s& offset, const vec2s& extents) {
  if (region.extents() != extents) region = array2d<vec4f>(extents);
  for (auto ij : range(region.extents())) region[ij] = image[ij + offset];
}
template <typename T>
constexpr void set_region(
    array2d<T>& image, const array2d<T>& region, const vec2s& offset) {
  for (auto ij : range(region.extents())) image[ij + offset] = region[ij];
}

// Compute the difference between two images.
inline array2d<vec4f> image_difference(
    const array2d<vec4f>& image1, const array2d<vec4f>& image2, bool display) {
  // check sizes
  if (image1.extents() != image2.extents())
    throw std::invalid_argument{"image sizes are different"};

  // compute diff
  auto difference = array2d<vec4f>(image1.extents());
  for (auto idx : range(difference.size())) {
    auto diff       = abs(image1[idx] - image2[idx]);
    difference[idx] = display ? vec4f{max(diff), max(diff), max(diff), 1}
                              : diff;
  }
  return difference;
}

// Composite two images together.
template <typename T>
inline array2d<vec<T, 4>> composite_image(
    const array2d<vec<T, 4>>& image_a, const array2d<vec<T, 4>>& image_b) {
  if (image_a.extents() != image_b.extents())
    throw std::invalid_argument{"image should be the same size"};
  auto result = array2d<vec<T, 4>>(image_a.extents());
  for (auto idx : range(result.size())) {
    result[idx] = composite(image_a[idx], image_b[idx]);
  }
  return result;
}

// Composite two images together.
template <typename T>
inline void composite_image(array2d<vec<T, 4>>& result,
    const array2d<vec<T, 4>>& image_a, const array2d<vec<T, 4>>& image_b) {
  if (image_a.extents() != image_b.extents())
    throw std::invalid_argument{"image should be the same size"};
  if (image_a.extents() != result.extents())
    throw std::invalid_argument{"image should be the same size"};
  for (auto idx : range(result.size())) {
    result[idx] = composite(image_a[idx], image_b[idx]);
  }
}

// Color grade an hsr or ldr image to an ldr image.
template <typename T>
inline array2d<vec<T, 4>> colorgrade_image(const array2d<vec<T, 4>>& image,
    bool linear, const colorgrade_gparams<T>& params) {
  auto result = array2d<vec<T, 4>>(image.extents());
  for (auto idx : range(image.size())) {
    result[idx] = colorgrade(image[idx], linear, params);
  }
  return result;
}

// Color grade an hsr or ldr image to an ldr image.
template <typename T>
inline void colorgrade_image(array2d<vec<T, 4>>& result,
    const array2d<vec<T, 4>>& image, bool linear,
    const colorgrade_gparams<T>& params) {
  if (image.extents() != result.extents())
    throw std::invalid_argument{"image should be the same size"};
  for (auto idx : range(image.size())) {
    result[idx] = colorgrade(image[idx], linear, params);
  }
}

// Color grade an hsr or ldr image to an ldr image.
// Uses multithreading for speed.
void colorgrade_image_mt(array2d<vec4f>& result, const array2d<vec4f>& image,
    bool linear, const colorgrade_params& params);

// determine white balance colors
template <typename T>
inline vec<T, 3> compute_white_balance(const array2d<vec<T, 4>>& image) {
  auto rgb = vec<T, 3>{0, 0, 0};
  for (auto idx : range(image.size())) rgb += xyz(image[idx]);
  if (rgb == vec<T, 3>{0, 0, 0}) return vec<T, 3>{0, 0, 0};
  rgb /= max(rgb);
  return rgb;
}

// Resize an image.
array2d<vec4f> resize_image(const array2d<vec4f>& image, const vec2s& extents);

}  // namespace yocto

// -----------------------------------------------------------------------------
// IMAGE EXAMPLES
// -----------------------------------------------------------------------------
namespace yocto {

template <typename Func, typename T = std::invoke_result_t<Func, vec2s>>
inline array2d<T> _make_proc_image(const vec2s& extents, Func&& func) {
  auto image = array2d<T>(extents);
  for (auto ij : range(extents)) image[ij] = func(ij);
  return image;
}

// Make an image
template <typename T = float>
inline array2d<vec<T, 4>> make_grid(const vec2s& extents, T scale = 1,
    const vec<T, 4>& color0 = {0.5, 0.5, 0.5, 1.0},
    const vec<T, 4>& color1 = {0.5, 0.5, 0.7, 1.0}) {
  return _make_proc_image(extents, [=](vec2s ij) -> vec<T, 4> {
    auto [u, v] = fmod((4 * scale * ij) / extents, 1);
    auto thick  = (T)0.01 / 2;
    auto c = u <= thick || u >= 1 - thick || v <= thick || v >= 1 - thick ||
             (u >= (T)0.5 - thick && u <= 0.5f + thick) ||
             (v >= (T)0.5 - thick && v <= 0.5f + thick);
    return c ? color0 : color1;
  });
}

template <typename T = float>
inline array2d<vec<T, 4>> make_checker(const vec2s& extents, T scale = 1,
    const vec<T, 4>& color0 = {0.5, 0.5, 0.6, 1.0},
    const vec<T, 4>& color1 = {0.7, 0.7, 0.7, 1.0}) {
  return _make_proc_image(extents, [=](vec2s ij) -> vec<T, 4> {
    auto [u, v] = fmod((4 * scale * ij) / extents, 1);
    auto c      = u <= (T)0.5 != v <= (T)0.5;
    return c ? color0 : color1;
  });
}

template <typename T = float>
inline array2d<vec<T, 4>> make_bumps(const vec2s& extents, T scale = 1,
    const vec<T, 4>& color0 = {0, 0, 0, 1},
    const vec<T, 4>& color1 = {1, 1, 1, 1}) {
  return _make_proc_image(extents, [=](vec2s ij) -> vec<T, 4> {
    auto uv     = fmod((4 * scale * ij) / extents, 1);
    auto [u, v] = uv;
    auto thick  = (T)0.125;
    auto center = vec<T, 2>{
        u <= (T)0.5 ? (T)0.25 : (T)0.75,
        v <= (T)0.5 ? (T)0.25 : (T)0.75,
    };
    auto dist = clamp(length(uv - center), 0, thick) / thick;
    auto val  = u <= (T)0.5 != v <= (T)0.5 ? (1 + sqrt(1 - dist)) / 2
                                           : (dist * dist) / 2;
    return lerp(color0, color1, val);
  });
}

template <typename T = float>
inline array2d<vec<T, 4>> make_ramp(const vec2s& extents, T scale = 1,
    const vec<T, 4>& color0 = {0, 0, 0, 1},
    const vec<T, 4>& color1 = {1, 1, 1, 1}) {
  return _make_proc_image(extents, [=](vec2s ij) -> vec<T, 4> {
    auto [u, _] = fmod((scale * ij) / extents, 1);
    return lerp(color0, color1, u);
  });
}

template <typename T = float>
inline array2d<vec<T, 4>> make_gammaramp(const vec2s& extents, T scale = 1,
    const vec<T, 4>& color0 = {0, 0, 0, 1},
    const vec<T, 4>& color1 = {1, 1, 1, 1}) {
  return _make_proc_image(extents, [=](vec2s ij) -> vec<T, 4> {
    auto [u, v] = fmod((scale * ij) / extents, 1);
    auto gamma  = (T)2.2;
    if (v < (T)1 / 3) {
      return lerp(color0, color1, pow(u, gamma));
    } else if (v < (T)2 / 3) {
      return lerp(color0, color1, u);
    } else {
      return lerp(color0, color1, pow(u, 1 / gamma));
    }
  });
}

template <typename T = float>
inline array2d<vec<T, 4>> make_uvramp(
    const vec2s& extents = {1024, 1024}, T scale = 1) {
  return _make_proc_image(extents, [=](vec2s ij) -> vec<T, 4> {
    auto [u, v] = fmod((scale * ij) / extents, 1);
    return vec<T, 4>{u, v, 0, 1};
  });
}

template <typename T = float>
inline array2d<vec<T, 4>> make_orgrid(
    const vec2s& extents = {1024, 1024}, T scale = 1) {
  return _make_proc_image(extents, [=](vec2s ij) -> vec<T, 4> {
    auto [u, v] = fmod((scale * ij) / extents, 1);
    return u < (T)0.5
               ? (v < (T)0.5 ? vec<T, 4>{0, 0, 0, 1} : vec<T, 4>{0, 1, 0, 1})
               : (v < (T)0.5 ? vec<T, 4>{1, 0, 0, 1} : vec<T, 4>{1, 1, 0, 1});
  });
}

template <typename T = float>
inline array2d<vec<T, 4>> make_uvgrid(
    const vec2s& extents = {1024, 1024}, T scale = 1, bool colored = true) {
  return _make_proc_image(extents, [=](vec2s ij) -> vec<T, 4> {
    auto uv     = fmod((scale * ij) / extents, 1);
    uv[1]       = 1 - uv[1];
    auto [u, v] = uv;
    auto hue =
        (clamp((int)(u * 8), 0, 7) + (clamp((int)(v * 8), 0, 7) + 5) % 8 * 8) /
        (T)64.0;
    auto [vu, vv]   = fmod(uv * 4, 1);
    auto vc         = vu <= 0.5f != vv <= 0.5f;
    auto value      = vc ? 0.5f - 0.05f : 0.5f + 0.05f;
    auto [su, sv]   = fmod(uv * 16, 1);
    auto st         = (T)0.01 / 2;
    auto sc         = su <= st || su >= 1 - st || sv <= st || sv >= 1 - st;
    auto saturation = (T)0;
    if (sc) {
      saturation = (T)0.2;
      value      = (T)0.8;
    } else {
      saturation = (T)0.8;
    }
    auto hsv = vec<T, 3>{hue, saturation, value};
    auto rgb = (colored) ? hsv_to_rgb(hsv) : vec<T, 3>{value, value, value};
    return vec<T, 4>{rgb_to_srgb(rgb), 1};
  });
}

template <typename T = float>
inline array2d<vec<T, 4>> make_colormapramp(const vec2s& extents, T scale = 1) {
  return _make_proc_image(extents, [=](vec2s ij) -> vec<T, 4> {
    auto [u, v] = fmod((scale * ij) / extents, 1);
    auto rgb    = vec<T, 3>{0, 0, 0};
    if (v < (T)0.25) {
      rgb = colormap(u, colormap_type::viridis);
    } else if (v < (T)0.50) {
      rgb = colormap(u, colormap_type::plasma);
    } else if (v < (T)0.75) {
      rgb = colormap(u, colormap_type::magma);
    } else {
      rgb = colormap(u, colormap_type::inferno);
    }
    return vec<T, 4>{rgb, 1};
  });
}

template <typename T = float>
inline array2d<vec<T, 4>> make_noisemap(const vec2s& extents, T scale = 1,
    const vec<T, 4>& color0 = {0, 0, 0, 1},
    const vec<T, 4>& color1 = {1, 1, 1, 1}) {
  return _make_proc_image(extents, [=](vec2s ij) -> vec<T, 4> {
    auto [u, v] = (8 * scale * ij) / extents;
    auto value  = perlin_noise(vec<T, 3>{u, v, 0});
    return lerp(color0, color1, clamp(value, 0, 1));
  });
}

template <typename T = float>
inline array2d<vec<T, 4>> make_fbmmap(const vec2s& extents, T scale = 1,
    const vec<T, 4>& noise  = vec<T, 4>{2, 0.5, 8, 1},
    const vec<T, 4>& color0 = {0, 0, 0, 1},
    const vec<T, 4>& color1 = {1, 1, 1, 1}) {
  return _make_proc_image(extents, [=](vec2s ij) -> vec<T, 4> {
    auto [u, v]                         = (8 * scale * ij) / extents;
    auto [lacunarity, gain, octaves, _] = noise;
    auto value = perlin_fbm({u, v, 0}, lacunarity, gain, (int)octaves);
    return lerp(color0, color1, clamp(value, 0, 1));
  });
}

template <typename T = float>
inline array2d<vec<T, 4>> make_turbulencemap(const vec2s& extents, T scale = 1,
    const vec<T, 4>& noise  = vec<T, 4>{2, 0.5, 8, 1},
    const vec<T, 4>& color0 = {0, 0, 0, 1},
    const vec<T, 4>& color1 = {1, 1, 1, 1}) {
  return _make_proc_image(extents, [=](vec2s ij) -> vec<T, 4> {
    auto [u, v]                         = (8 * scale * ij) / extents;
    auto [lacunarity, gain, octaves, _] = noise;
    auto value = perlin_turbulence({u, v, 0}, lacunarity, gain, (int)octaves);
    v          = clamp(v, 0.0f, 1.0f);
    return lerp(color0, color1, clamp(value, 0, 1));
  });
}

template <typename T = float>
inline array2d<vec<T, 4>> make_ridgemap(const vec2s& extents, T scale = 1,
    const vec<T, 4>& noise  = {2, 0.5, 8, 1},
    const vec<T, 4>& color0 = {0, 0, 0, 1},
    const vec<T, 4>& color1 = {1, 1, 1, 1}) {
  return _make_proc_image(extents, [=](vec2s ij) -> vec<T, 4> {
    auto [u, v]                              = (8 * scale * ij) / extents;
    auto [lacunarity, gain, octaves, offset] = noise;
    auto value                               = perlin_ridge(
        {u, v, 0}, lacunarity, gain, (int)octaves, offset);
    return lerp(color0, color1, clamp(value, 0, 1));
  });
}

template <typename T = float>
inline array2d<vec<T, 4>> make_gnoisemap(const vec2s& extents, T scale = 1,
    const vec<T, 4>& color0 = {0, 0, 0, 1},
    const vec<T, 4>& color1 = {1, 1, 1, 1}) {
  return _make_proc_image(extents, [=](vec2s ij) -> vec<T, 4> {
    auto uv    = (8 * scale * ij) / extents;
    auto value = gradient_noise(uv) * (T)0.5 + (T)0.5;
    return lerp(color0, color1, clamp(value, 0, 1));
  });
}

// Add image border
template <typename T = float>
inline array2d<vec<T, 4>> add_border(const array2d<vec<T, 4>>& image, T width,
    const vec<T, 4>& color = {0, 0, 0, 1}) {
  auto result = image;
  auto [w, h] = image.extents();
  auto scale  = (T)1 / max(image.extents());
  for (auto ij : range(image.extents())) {
    auto [u, v] = (vec2f)ij * scale;
    if (u < width || v < width || v > w * scale - width ||
        v > h * scale - width) {
      result[ij] = color;
    }
  }
  return result;
}

// Comvert a bump map to a normal map.
template <typename T = float>
inline void bump_to_normal(array2d<vec<T, 4>>& normalmap,
    const array2d<vec<T, 4>>& bumpmap, float scale) {
  if (normalmap.extents() != bumpmap.extents())
    throw std::out_of_range{"different image sizes"};
  auto dxy = 1 / (vec2f)bumpmap.extents();
  for (auto ij : range(bumpmap.extents())) {
    auto i1j = (ij + vec2s{1, 0}) % bumpmap.extents();
    auto ij1 = (ij + vec2s{0, 1}) % bumpmap.extents();
    auto p00 = bumpmap[ij], p10 = bumpmap[i1j], p01 = bumpmap[ij1];
    auto g00 = mean(p00), g10 = mean(p10), g01 = mean(p01);
    auto normal = vec<T, 3>{
        scale * (g00 - g10) / dxy[0], scale * (g00 - g01) / dxy[1], 1};
    normal[1] = -normal[1];  // make green pointing up, even if y axis
                             // points down
    normal        = normalize(normal) * (T)0.5 + (T)0.5;
    normalmap[ij] = {normal, 1};
  }
}
template <typename T = float>
inline array2d<vec<T, 4>> bump_to_normal(
    const array2d<vec<T, 4>>& bumpmap, float scale) {
  auto normalmap = array2d<vec<T, 4>>{bumpmap.extents()};
  bump_to_normal(normalmap, bumpmap, scale);
  return normalmap;
}

// Implementation of sunsky modified heavily from pbrt
template <typename T = float>
inline array2d<vec<T, 4>> make_sunsky(const vec2s& extents, float theta_sun,
    T turbidity = 3, bool has_sun = false, T sun_intensity = 1,
    T                sun_radius    = 1,
    const vec<T, 3>& ground_albedo = vec<T, 3>{0.2, 0.2, 0.2}) {
  auto zenith_xyY = vec<T, 3>{
      ((T)0.00165 * pow(theta_sun, 3) - (T)0.00374 * pow(theta_sun, 2) +
          (T)0.00208 * theta_sun + (T)0.00000) *
              pow(turbidity, 2) +
          ((T)-0.02902 * pow(theta_sun, 3) + (T)0.06377 * pow(theta_sun, 2) -
              (T)0.03202 * theta_sun + (T)0.00394) *
              turbidity +
          ((T) + 0.11693 * pow(theta_sun, 3) - (T)0.21196 * pow(theta_sun, 2) +
              (T)0.06052 * theta_sun + (T)0.25885),
      ((T) + 0.00275 * pow(theta_sun, 3) - (T)0.00610 * pow(theta_sun, 2) +
          (T)0.00316 * theta_sun + (T)0.00000) *
              pow(turbidity, 2) +
          ((T)-0.04214 * pow(theta_sun, 3) + (T)0.08970 * pow(theta_sun, 2) -
              (T)0.04153 * theta_sun + (T)0.00515) *
              turbidity +
          ((T) + 0.15346 * pow(theta_sun, 3) - (T)0.26756 * pow(theta_sun, 2) +
              (T)0.06669 * theta_sun + (T)0.26688),
      1000 * ((T)4.0453 * turbidity - (T)4.9710) *
              tan(((T)4.0 / (T)9.0 - turbidity / (T)120.0) *
                  ((T)pi - 2 * theta_sun)) -
          (T)0.2155 * turbidity + 24192,
  };

  auto perez_A_xyY = vec<T, 3>{(T)-0.01925 * turbidity - (T)0.25922,
      (T)-0.01669 * turbidity - (T)0.26078,
      (T) + 0.17872 * turbidity - (T)1.46303};
  auto perez_B_xyY = vec<T, 3>{(T)-0.06651 * turbidity + (T)0.00081,
      (T)-0.09495 * turbidity + (T)0.00921,
      (T)-0.35540 * turbidity + (T)0.42749};
  auto perez_C_xyY = vec<T, 3>{(T)-0.00041 * turbidity + (T)0.21247,
      (T)-0.00792 * turbidity + (T)0.21023,
      (T)-0.02266 * turbidity + (T)5.32505};
  auto perez_D_xyY = vec<T, 3>{(T)-0.06409 * turbidity - (T)0.89887,
      (T)-0.04405 * turbidity - (T)1.65369,
      (T) + 0.12064 * turbidity - (T)2.57705};
  auto perez_E_xyY = vec<T, 3>{(T)-0.00325 * turbidity + (T)0.04517,
      (T)-0.01092 * turbidity + (T)0.05291,
      (T)-0.06696 * turbidity + (T)0.37027};

  auto perez_f = [](vec<T, 3> A, vec<T, 3> B, vec<T, 3> C, vec<T, 3> D,
                     vec<T, 3> E, T theta, T gamma, T theta_sun,
                     vec<T, 3> zenith) -> vec<T, 3> {
    auto num = ((1 + A * exp(B / cos(theta))) *
                (1 + C * exp(D * gamma) + E * cos(gamma) * cos(gamma)));
    auto den = ((1 + A * exp(B)) * (1 + C * exp(D * theta_sun) +
                                       E * cos(theta_sun) * cos(theta_sun)));
    return zenith * num / den;
  };

  auto sky = [&perez_f, perez_A_xyY, perez_B_xyY, perez_C_xyY, perez_D_xyY,
                 perez_E_xyY,
                 zenith_xyY](T theta, T gamma, T theta_sun) -> vec<T, 3> {
    return xyz_to_rgb(xyY_to_xyz(
               perez_f(perez_A_xyY, perez_B_xyY, perez_C_xyY, perez_D_xyY,
                   perez_E_xyY, theta, gamma, theta_sun, zenith_xyY))) /
           10000;
  };

  // compute sun luminance
  auto sun_ko     = vec<T, 3>{0.48f, 0.75f, 0.14f};
  auto sun_kg     = vec<T, 3>{0.1f, 0.0f, 0.0f};
  auto sun_kwa    = vec<T, 3>{0.02f, 0.0f, 0.0f};
  auto sun_sol    = vec<T, 3>{20000.0f, 27000.0f, 30000.0f};
  auto sun_lambda = vec<T, 3>{680, 530, 480};
  auto sun_beta   = (T)0.04608365822050 * turbidity - (T)0.04586025928522;
  auto sun_m =
      (T)1.0 /
      (cos(theta_sun) + (T)0.000940 * pow((T)1.6386 - theta_sun, (T)-1.253));

  auto tauR = exp(-sun_m * (T)0.008735 * pow(sun_lambda / 1000, (T)-4.08));
  auto tauA = exp(-sun_m * sun_beta * pow(sun_lambda / 1000, (T)-1.3));
  auto tauO = exp(-sun_m * sun_ko * (T)0.35);
  auto tauG = exp(
      (T)-1.41 * sun_kg * sun_m / pow(1 + (T)118.93 * sun_kg * sun_m, (T)0.45));
  auto tauWA  = exp((T)-0.2385 * sun_kwa * 2 * sun_m /
                    pow(1 + (T)20.07 * sun_kwa * 2 * sun_m, (T)0.45));
  auto sun_le = sun_sol * tauR * tauA * tauO * tauG * tauWA * 10000;

  // rescale by user
  sun_le *= sun_intensity;

  // size
  auto [width, height] = extents;

  // sun scale from Wikipedia scaled by user quantity and rescaled to at
  // the minimum 5 pixel diameter
  auto sun_angular_radius = 9.35e-03f / 2;  // Wikipedia
  sun_angular_radius *= sun_radius;
  sun_angular_radius = max(sun_angular_radius, 2 * pif / height);

  // sun direction
  auto sun_direction = vec3f{0, cos(theta_sun), sin(theta_sun)};

  auto sun = [has_sun, sun_angular_radius, sun_le](auto theta, auto gamma) {
    return (has_sun && gamma < sun_angular_radius) ? sun_le / 10000
                                                   : vec<T, 3>{0, 0, 0};
  };

  // Make the sun sky image
  auto img = array2d<vec<T, 4>>(extents);
  for (auto j : range(height / 2)) {
    auto theta = pif * ((j + 0.5f) / height);
    theta      = clamp(theta, 0.0f, (T)pi / 2 - flt_eps);
    for (auto i : range(width)) {
      auto phi = 2 * (T)pi * (T(i + 0.5) / width);
      auto w = vec3f{cos(phi) * sin(theta), cos(theta), sin(phi) * sin(theta)};
      auto gamma   = acos(clamp(dot(w, sun_direction), -1, 1));
      auto sky_col = sky(theta, gamma, theta_sun);
      auto sun_col = sun(theta, gamma);
      auto col     = sky_col + sun_col;
      img[{i, j}]  = {col, 1};
    }
  }

  if (ground_albedo != vec<T, 3>{0, 0, 0}) {
    auto ground = vec<T, 3>{0, 0, 0};
    for (auto j : range(height / 2)) {
      auto theta = (T)pi * (T(j + 0.5) / height);
      for (auto i : range(width)) {
        auto le    = xyz(img[{i, j}]);
        auto angle = sin(theta) * 4 * (T)pi / (width * height);
        ground += le * (ground_albedo / (T)pi) * cos(theta) * angle;
      }
    }
    for (auto j : range(height / 2, height)) {
      for (auto i : range(width)) {
        img[{i, j}] = {ground, 1};
      }
    }
  } else {
    for (auto j : range(height / 2, height)) {
      for (auto i : range(width)) {
        img[{i, j}] = {0, 0, 0, 1};
      }
    }
  }

  // done
  return img;
}

// Make an image of multiple lights.
template <typename T = float>
inline array2d<vec<T, 4>> make_lights(const vec2s& extents,
    const vec<T, 3>& le = {1, 1, 1}, int nlights = 4, T langle = pif / 4,
    T lwidth = (T)pi / 16, T lheight = (T)pi / 16) {
  auto [width, height] = extents;
  auto img             = array2d<vec<T, 4>>(extents);
  for (auto j : range(height)) {
    auto theta = pif * ((j + 0.5f) / height);
    theta      = clamp(theta, 0, (T)pi / 2 - (T)0.00001);
    if (fabs(theta - langle) > lheight / 2) continue;
    for (auto i : range(width)) {
      auto phi     = 2 * (T)pi * ((i + (T)0.5) / width);
      auto inlight = false;
      for (auto l : range(nlights)) {
        auto lphi = 2 * (T)pi * (l + (T)0.5) / nlights;
        inlight   = inlight || fabs(phi - lphi) < lwidth / 2;
      }
      img[{i, j}] = {le, 1};
    }
  }
  return img;
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// IMAGE DATA AND UTILITIES
// -----------------------------------------------------------------------------
namespace yocto {

// Image data as array of float or byte pixels. Images can be stored in linear
// or non linear color space.
struct image_data {
  // image data
  int           width  = 0;
  int           height = 0;
  bool          linear = false;
  vector<vec4f> pixels = {};

  // pixel access
  inline vec4f& operator[](vec2i ij) {
    auto [i, j] = ij;
    return pixels[j * width + i];
  }
  inline const vec4f& operator[](vec2i ij) const {
    auto [i, j] = ij;
    return pixels[j * width + i];
  }
};

// image creation
image_data make_image(int width, int height, bool linear);

// equality
bool operator==(const image_data& a, const image_data& b);
bool operator!=(const image_data& a, const image_data& b);

// swap
void swap(image_data& a, image_data& b);

// pixel access
inline vec4f get_pixel(const image_data& image, int i, int j) {
  return image.pixels[j * image.width + i];
}
inline void set_pixel(image_data& image, int i, int j, const vec4f& pixel) {
  image.pixels[j * image.width + i] = pixel;
}

// conversions
image_data convert_image(const image_data& image, bool linear);
void       convert_image(image_data& result, const image_data& image);

// Evaluates an image at a point `uv`.
vec4f eval_image(const image_data& image, const vec2f& uv,
    bool as_linear = false, bool no_interpolation = false,
    bool clamp_to_edge = false);

// Apply tone mapping returning a float or byte image.
image_data tonemap_image(
    const image_data& image, float exposure, bool filmic = false);

// Apply tone mapping. If the input image is an ldr, does nothing.
void tonemap_image(image_data& ldr, const image_data& image, float exposure,
    bool filmic = false);
// Apply tone mapping using multithreading for speed.
void tonemap_image_mt(image_data& ldr, const image_data& image, float exposure,
    bool filmic = false);

// Resize an image.
image_data resize_image(const image_data& image, size_t width, size_t height);

// set/get region
void set_region(image_data& image, const image_data& region, int x, int y);
void get_region(image_data& region, const image_data& image, int x, int y,
    int width, int height);

// Compute the difference between two images.
image_data image_difference(
    const image_data& image_a, const image_data& image_b, bool display_diff);

// Composite two images together.
image_data composite_image(
    const image_data& image_a, const image_data& image_b);

// Composite two images together.
void composite_image(
    image_data& result, const image_data& image_a, const image_data& image_b);

// Composite two images together.
void composite_image(image_data& result, const vector<image_data>& images);

// Color grade an hsr or ldr image to an ldr image.
image_data colorgrade_image(
    const image_data& image, const colorgrade_params& params);

// Color grade an hsr or ldr image to an ldr image.
// Uses multithreading for speed.
void colorgrade_image(image_data& result, const image_data& image,
    const colorgrade_params& params);

// Color grade an hsr or ldr image to an ldr image.
// Uses multithreading for speed.
void colorgrade_image_mt(image_data& result, const image_data& image,
    const colorgrade_params& params);

// determine white balance colors
vec3f compute_white_balance(const image_data& image);

}  // namespace yocto

// -----------------------------------------------------------------------------
// IMAGE UTILITIES
// -----------------------------------------------------------------------------
namespace yocto {

// Conversion from/to floats.
void byte_to_float(vector<vec4f>& fl, const vector<vec4b>& bt);
void float_to_byte(vector<vec4b>& bt, const vector<vec4f>& fl);

// Conversion between linear and gamma-encoded images.
void srgb_to_rgb(vector<vec4f>& rgb, const vector<vec4f>& srgb);
void rgb_to_srgb(vector<vec4f>& srgb, const vector<vec4f>& rgb);
void srgb_to_rgb(vector<vec4f>& rgb, const vector<vec4b>& srgb);
void rgb_to_srgb(vector<vec4b>& srgb, const vector<vec4f>& rgb);

// Apply tone mapping
void tonemap_image(vector<vec4f>& ldr, const vector<vec4f>& hdr, float exposure,
    bool filmic = false, bool srgb = true);
void tonemap_image(vector<vec4b>& ldr, const vector<vec4f>& hdr, float exposure,
    bool filmic = false, bool srgb = true);

// Apply tone mapping using multithreading for speed
void tonemap_image_mt(vector<vec4f>& ldr, const vector<vec4f>& hdr,
    float exposure, bool filmic = false, bool srgb = true);
void tonemap_image_mt(vector<vec4b>& ldr, const vector<vec4f>& hdr,
    float exposure, bool filmic = false, bool srgb = true);

// Color grade a linear or srgb image to an srgb image.
// Uses multithreading for speed.
void colorgrade_image_mt(vector<vec4f>& corrected, const vector<vec4f>& img,
    bool linear, const colorgrade_params& params);
void colorgrade_image_mt(vector<vec4b>& corrected, const vector<vec4f>& img,
    bool linear, const colorgrade_params& params);

// determine white balance colors
vec3f compute_white_balance(const vector<vec4f>& img);

// Resize an image.
void resize_image(vector<vec4f>& res, const vector<vec4f>& img, int width,
    int height, int res_width, int res_height);
void resize_image(vector<vec4b>& res, const vector<vec4b>& img, int width,
    int height, int res_width, int res_height);

// Compute the difference between two images
void image_difference(vector<vec4f>& diff, const vector<vec4f>& a,
    const vector<vec4f>& b, bool disply_diff);

}  // namespace yocto

#endif
