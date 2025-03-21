//
// Implementation for Yocto/Scene Input and Output functions.
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

#include "yocto_sceneio.h"

#include <cgltf/cgltf.h>
#include <cgltf/cgltf_write.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_resize2.h>
#include <stb_image/stb_image_write.h>
#include <tinyexr/tinyexr.h>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include "yocto_color.h"
#include "yocto_geometry.h"
#include "yocto_image.h"
#include "yocto_modelio.h"
#include "yocto_pbrtio.h"
#include "yocto_shading.h"
#include "yocto_shape.h"

// -----------------------------------------------------------------------------
// USING DIRECTIVES
// -----------------------------------------------------------------------------
namespace yocto {

// using directives
using std::unique_ptr;
using namespace std::string_literals;

}  // namespace yocto

// -----------------------------------------------------------------------------
// PARALLEL HELPERS
// -----------------------------------------------------------------------------
namespace yocto {

// Simple parallel for used since our target platforms do not yet support
// parallel algorithms. `Func` takes the integer index.
template <typename T, typename Func>
inline void parallel_for(T num, bool noparallel, Func&& func) {
  if (noparallel) {
    for (auto idx : range(num)) {
      func(idx);
    }
  } else {
    auto              futures  = vector<std::future<void>>{};
    auto              nthreads = std::thread::hardware_concurrency();
    std::atomic<T>    next_idx(0);
    std::atomic<bool> has_error(false);
    for (auto thread_id = 0; thread_id < (int)nthreads; thread_id++) {
      futures.emplace_back(
          std::async(std::launch::async, [&func, &next_idx, &has_error, num]() {
            while (true) {
              if (has_error) break;
              auto idx = next_idx.fetch_add(1);
              if (idx >= num) break;
              try {
                func(idx);
              } catch (std::exception& error) {
                has_error = true;
                throw;
              }
            }
          }));
    }
    for (auto& f : futures) f.get();
  }
}

// Simple parallel for used since our target platforms do not yet support
// parallel algorithms. `Func` takes the integer index.
template <typename Sequence1, typename Sequence2, typename Func>
inline void parallel_zip(Sequence1&& sequence1, Sequence2&& sequence2,
    bool noparallel, Func&& func) {
  if (std::size(sequence1) != std::size(sequence2))
    throw std::out_of_range{"invalid sequence lengths"};
  if (noparallel) {
    for (auto idx : range(sequence1.size())) {
      func(std::forward<Sequence1>(sequence1)[idx],
          std::forward<Sequence2>(sequence2)[idx]);
    }
  } else {
    auto                num      = std::size(sequence1);
    auto                futures  = vector<std::future<void>>{};
    auto                nthreads = std::thread::hardware_concurrency();
    std::atomic<size_t> next_idx(0);
    std::atomic<bool>   has_error(false);
    for (auto thread_id = 0; thread_id < (int)nthreads; thread_id++) {
      futures.emplace_back(std::async(std::launch::async,
          [&func, &next_idx, &has_error, num, &sequence1, &sequence2]() {
            try {
              while (true) {
                auto idx = next_idx.fetch_add(1);
                if (idx >= num) break;
                if (has_error) break;
                func(std::forward<Sequence1>(sequence1)[idx],
                    std::forward<Sequence2>(sequence2)[idx]);
              }
            } catch (...) {
              has_error = true;
              throw;
            }
          }));
    }
    for (auto& f : futures) f.get();
  }
}

// Simple parallel for used since our target platforms do not yet support
// parallel algorithms. `Func` takes a reference to a `T`.
template <typename T, typename Func>
inline void parallel_foreach(vector<T>& values, bool noparallel, Func&& func) {
  return parallel_for(values.size(), noparallel,
      [&func, &values](size_t idx) { return func(values[idx]); });
}
template <typename T, typename Func>
inline void parallel_foreach(
    const vector<T>& values, bool noparallel, Func&& func) {
  return parallel_for(values.size(), noparallel,
      [&func, &values](size_t idx) { return func(values[idx]); });
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// PATH HELPERS
// -----------------------------------------------------------------------------
namespace yocto {

// Make a path from a utf8 string
static std::filesystem::path to_path(const string& filename) {
  auto filename8 = std::u8string((char8_t*)filename.data(), filename.size());
  return std::filesystem::path(filename8);
}

// Make a utf8 string from a path
static string to_string(const std::filesystem::path& path) {
  auto string8 = path.u8string();
  return string((char*)string8.data(), string8.size());
}

// Normalize a path
string path_normalized(const string& path) { return to_string(to_path(path)); }

// Get directory name (not including /)
string path_dirname(const string& path) {
  return to_string(to_path(path).parent_path());
}

// Get filename without directory and extension.
string path_basename(const string& path) {
  return to_string(to_path(path).stem());
}

// Get extension
string path_extension(const string& path) {
  return to_string(to_path(path).extension());
}

// Check if a file can be opened for reading.
bool path_exists(const string& path) { return exists(to_path(path)); }

// Replace the extension of a file
string replace_extension(const string& path, const string& extension) {
  auto ext = to_path(extension).extension();
  return to_string(to_path(path).replace_extension(ext));
}

// Create a directory and all missing parent directories if needed
void make_directory(const string& path) {
  if (path_exists(path)) return;
  try {
    create_directories(to_path(path));
  } catch (...) {
    throw io_error{"cannot create directory " + path};
  }
}

bool make_directory(const string& path, string& error) {
  try {
    make_directory(path);
    return true;
  } catch (...) {
    error = "cannot create directory " + path;
    return false;
  }
}

// Joins paths
static string path_join(const string& patha, const string& pathb) {
  return to_string(to_path(patha) / to_path(pathb));
}
static string path_join(
    const string& patha, const string& pathb, const string& pathc) {
  return to_string(to_path(patha) / to_path(pathb) / to_path(pathc));
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// FILE IO
// -----------------------------------------------------------------------------
namespace yocto {

// Opens a file with a utf8 file name
static FILE* fopen_utf8(const char* filename, const char* mode) {
#ifdef _WIN32
  auto path8    = std::filesystem::u8path(filename);
  auto str_mode = string{mode};
  auto wmode    = std::wstring(str_mode.begin(), str_mode.end());
  return _wfopen(path8.c_str(), wmode.c_str());
#else
  return fopen(filename, mode);
#endif
}

// Opens a file with utf8 filename
FILE* fopen_utf8(const string& filename, const string& mode) {
#ifdef _WIN32
  auto path8 = std::filesystem::u8path(filename);
  auto wmode = std::wstring(mode.begin(), mode.end());
  return _wfopen(path8.c_str(), wmode.c_str());
#else
  return fopen(filename.c_str(), mode.c_str());
#endif
}

// Load a text file
string load_text(const string& filename) {
  // https://stackoverflow.com/questions/174531/how-to-read-the-content-of-a-file-to-a-string-in-c
  auto fs = fopen_utf8(filename.c_str(), "rb");
  if (fs == nullptr) throw io_error("cannot open " + filename);
  fseek(fs, 0, SEEK_END);
  auto length = ftell(fs);
  fseek(fs, 0, SEEK_SET);
  auto str = string(length, '\0');
  if (fread(str.data(), 1, length, fs) != length) {
    fclose(fs);
    throw io_error("cannot read " + filename);
  }
  fclose(fs);
  return str;
}

// Save a text file
void save_text(const string& filename, const string& str) {
  auto fs = fopen_utf8(filename.c_str(), "wt");
  if (fs == nullptr) throw io_error("cannot create " + filename);
  if (fprintf(fs, "%s", str.c_str()) < 0) {
    fclose(fs);
    throw io_error("cannot write " + filename);
  }
  fclose(fs);
}

// Load a binary file
vector<byte> load_binary(const string& filename) {
  // https://stackoverflow.com/questions/174531/how-to-read-the-content-of-a-file-to-a-string-in-c
  auto fs = fopen_utf8(filename.c_str(), "rb");
  if (fs == nullptr) throw io_error("cannot open " + filename);
  fseek(fs, 0, SEEK_END);
  auto length = ftell(fs);
  fseek(fs, 0, SEEK_SET);
  auto data = vector<byte>(length);
  if (fread(data.data(), 1, length, fs) != length) {
    fclose(fs);
    throw io_error("cannot read " + filename);
  }
  fclose(fs);
  return data;
}

// Save a binary file
void save_binary(const string& filename, const vector<byte>& data) {
  auto fs = fopen_utf8(filename.c_str(), "wb");
  if (fs == nullptr) throw io_error("cannot create " + filename);
  if (fwrite(data.data(), 1, data.size(), fs) != data.size()) {
    fclose(fs);
    throw io_error("cannot write " + filename);
  }
  fclose(fs);
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// JSON SUPPORT
// -----------------------------------------------------------------------------
namespace yocto {

// Json values
using json_value = nlohmann::ordered_json;

// Load/save json
[[maybe_unused]] static json_value load_json(const string& filename) {
  auto text = load_text(filename);
  try {
    return json_value::parse(text);
  } catch (...) {
    throw io_error("cannot parse " + filename);
  }
}
[[maybe_unused]] static void save_json(
    const string& filename, const json_value& json) {
  return save_text(filename, json.dump(2));
}

// Json conversions
inline void to_json(json_value& json, vec2f value) {
  nlohmann::to_json(json, (const array<float, 2>&)value);
}
inline void to_json(json_value& json, vec3f value) {
  nlohmann::to_json(json, (const array<float, 3>&)value);
}
inline void to_json(json_value& json, vec4f value) {
  nlohmann::to_json(json, (const array<float, 4>&)value);
}
inline void to_json(json_value& json, const frame2f& value) {
  nlohmann::to_json(json, (const array<float, 6>&)value);
}
inline void to_json(json_value& json, const frame3f& value) {
  nlohmann::to_json(json, (const array<float, 12>&)value);
}
inline void to_json(json_value& json, const mat2f& value) {
  nlohmann::to_json(json, (const array<float, 4>&)value);
}
inline void to_json(json_value& json, const mat3f& value) {
  nlohmann::to_json(json, (const array<float, 9>&)value);
}
inline void to_json(json_value& json, const mat4f& value) {
  nlohmann::to_json(json, (const array<float, 16>&)value);
}
inline void from_json(const json_value& json, vec2f& value) {
  nlohmann::from_json(json, (array<float, 2>&)value);
}
inline void from_json(const json_value& json, vec3f& value) {
  nlohmann::from_json(json, (array<float, 3>&)value);
}
inline void from_json(const json_value& json, vec4f& value) {
  nlohmann::from_json(json, (array<float, 4>&)value);
}
inline void from_json(const json_value& json, frame2f& value) {
  nlohmann::from_json(json, (array<float, 6>&)value);
}
inline void from_json(const json_value& json, frame3f& value) {
  nlohmann::from_json(json, (array<float, 12>&)value);
}
inline void from_json(const json_value& json, mat2f& value) {
  nlohmann::from_json(json, (array<float, 4>&)value);
}
inline void from_json(const json_value& json, mat3f& value) {
  nlohmann::from_json(json, (array<float, 9>&)value);
}
inline void from_json(const json_value& json, mat4f& value) {
  nlohmann::from_json(json, (array<float, 16>&)value);
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// MATH TYPE SUPPORT
// -----------------------------------------------------------------------------
namespace yocto {

static vec3f   to_math(const array<float, 3>& value) { return (vec3f&)value; }
static frame3f to_math(const array<float, 12>& value) {
  return (frame3f&)value;
}

static array<float, 3> to_array(vec3f value) { return (array<float, 3>&)value; }
static array<float, 12> to_array(const frame3f& value) {
  return (array<float, 12>&)value;
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// IMAGE IO
// -----------------------------------------------------------------------------
namespace yocto {

// Check if an image is HDR based on filename.
bool is_hdr_filename(const string& filename) {
  auto ext = path_extension(filename);
  return ext == ".hdr" || ext == ".exr" || ext == ".pfm";
}

bool is_ldr_filename(const string& filename) {
  auto ext = path_extension(filename);
  return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
         ext == ".tga";
}

// Check if an image is linear/sRGB based on filename.
bool is_linear_filename(const string& filename) {
  auto ext = path_extension(filename);
  return ext == ".hdr" || ext == ".exr" || ext == ".pfm";
}

bool is_srgb_filename(const string& filename) {
  auto ext = path_extension(filename);
  return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
         ext == ".tga";
}

// Loads a float image.
image<vec4f> load_image(const string& filename) {
  auto ext = path_extension(filename);
  if (ext == ".exr" || ext == ".EXR") {
    auto buffer = load_binary(filename);
    auto width = 0, height = 0;
    auto pixels = (float*)nullptr;
    if (LoadEXRFromMemory(&pixels, &width, &height, buffer.data(),
            buffer.size(), nullptr) != 0)
      throw io_error{"cannot read " + filename};
    auto ret = image<vec4f>{{width, height}, (vec4f*)pixels};
    free(pixels);
    return ret;
  } else if (ext == ".hdr" || ext == ".HDR") {
    auto buffer = load_binary(filename);
    auto width = 0, height = 0, ncomp = 0;
    auto pixels = stbi_loadf_from_memory(
        buffer.data(), (int)buffer.size(), &width, &height, &ncomp, 4);
    if (pixels == nullptr) throw io_error{"cannot read " + filename};
    auto ret = image<vec4f>{{width, height}, (vec4f*)pixels};
    free(pixels);
    return ret;
  } else if (ext == ".png" || ext == ".PNG" || ext == ".jpg" || ext == ".JPG" ||
             ext == ".jpeg" || ext == ".JPEG" || ext == ".tga" ||
             ext == ".TGA" || ext == ".bmp" || ext == ".BMP") {
    return byte_to_float(load_imageb(filename));
  } else if (ext == ".ypreset" || ext == ".YPRESET") {
    auto ret = make_image_preset(filename);
    return is_srgb_preset(filename) ? srgb_to_rgb(ret) : ret;
  } else {
    throw io_error{"unsupported format " + filename};
  }
}

// Loads a byte image.
image<vec4b> load_imageb(const string& filename) {
  auto ext = path_extension(filename);
  if (ext == ".exr" || ext == ".EXR" || ext == ".hdr" || ext == ".HDR") {
    return float_to_byte(load_image(filename));
  } else if (ext == ".png" || ext == ".PNG" || ext == ".jpg" || ext == ".JPG" ||
             ext == ".jpeg" || ext == ".JPEG" || ext == ".tga" ||
             ext == ".TGA" || ext == ".bmp" || ext == ".BMP") {
    auto buffer = load_binary(filename);
    auto width = 0, height = 0, ncomp = 0;
    auto pixels = stbi_load_from_memory(
        buffer.data(), (int)buffer.size(), &width, &height, &ncomp, 4);
    if (pixels == nullptr) throw io_error{"cannot read " + filename};
    auto ret = image<vec4b>{{width, height}, (vec4b*)pixels};
    free(pixels);
    return ret;
  } else if (ext == ".ypreset" || ext == ".YPRESET") {
    return float_to_byte(load_image(filename));
  } else {
    throw io_error{"unsupported format " + filename};
  }
}

bool is_linear_preset(const string& type_) {
  auto type = path_basename(type_);
  return type.find("sky") != string::npos;
}
bool is_srgb_preset(const string& type_) {
  auto type = path_basename(type_);
  return type.find("sky") == string::npos;
}
image<vec4f> make_image_preset(const string& type_) {
  auto type    = path_basename(type_);
  auto extents = vec2i{1024, 1024};
  if (type.find("sky") != string::npos) extents = {2048, 1024};
  if (type.find("images2") != string::npos) extents = {2048, 1024};
  if (type == "grid") {
    return make_grid(extents);
  } else if (type == "checker") {
    return make_checker(extents);
  } else if (type == "bumps") {
    return make_bumps(extents);
  } else if (type == "uvramp") {
    return make_uvramp(extents);
  } else if (type == "gammaramp") {
    return make_gammaramp(extents);
  } else if (type == "uvgrid") {
    return make_uvgrid(extents);
  } else if (type == "colormapramp") {
    return make_colormapramp(extents);
  } else if (type == "sky") {
    return make_sunsky(
        extents, pif / 4, 3.0f, false, 1.0f, 1.0f, vec3f{0.7f, 0.7f, 0.7f});
  } else if (type == "sunsky") {
    return make_sunsky(
        extents, pif / 4, 3.0f, true, 1.0f, 1.0f, vec3f{0.7f, 0.7f, 0.7f});
  } else if (type == "bump-normal") {
    return make_bumps(extents);
    // TODO(fabio): fix color space
    // img   = srgb_to_rgb(bump_to_normal(img, 0.05f));
  } else if (type == "images1") {
    auto sub_types  = vector<string>{"grid", "uvgrid", "checker", "gammaramp",
         "bumps", "bump-normal", "noise", "fbm", "blackbodyramp"};
    auto sub_images = vector<image<vec4f>>();
    for (auto& sub_type : sub_types)
      sub_images.push_back(make_image_preset(sub_type));
    auto montage_size = vec2i{0, 0};
    for (auto& sub_image : sub_images) {
      montage_size = {montage_size.x + sub_image.size().x,
          max(montage_size.y, sub_image.size().y)};
    }
    auto composite = image<vec4f>(montage_size);
    auto pos       = 0;
    for (auto& sub_image : sub_images) {
      set_region(composite, sub_image, {pos, 0});
      pos += sub_image.size().x;
    }
    return composite;
  } else if (type == "images2") {
    auto sub_types  = vector<string>{"sky", "sunsky"};
    auto sub_images = vector<image<vec4f>>();
    for (auto& sub_type : sub_types)
      sub_images.push_back(make_image_preset(sub_type));
    auto montage_size = vec2i{0, 0};
    for (auto& sub_image : sub_images) {
      montage_size = {montage_size.x + sub_image.size().x,
          max(montage_size.y, sub_image.size().y)};
    }
    auto composite = image<vec4f>(montage_size);
    auto pos       = 0;
    for (auto& sub_image : sub_images) {
      set_region(composite, sub_image, {pos, 0});
      pos += sub_image.size().x;
    }
    return composite;
  } else if (type == "test-floor") {
    return add_border(make_grid(extents), 0.0025f);
  } else if (type == "test-grid") {
    return make_grid(extents);
  } else if (type == "test-checker") {
    return make_checker(extents);
  } else if (type == "test-bumps") {
    return make_bumps(extents);
  } else if (type == "test-uvramp") {
    return make_uvramp(extents);
  } else if (type == "test-gammaramp") {
    return make_gammaramp(extents);
  } else if (type == "test-colormapramp") {
    return make_colormapramp(extents);
    // TODO(fabio): fix color space
    // img   = srgb_to_rgb(img);
  } else if (type == "test-uvgrid") {
    return make_uvgrid(extents);
  } else if (type == "test-sky") {
    return make_sunsky(
        extents, pif / 4, 3.0f, false, 1.0f, 1.0f, vec3f{0.7f, 0.7f, 0.7f});
  } else if (type == "test-sunsky") {
    return make_sunsky(
        extents, pif / 4, 3.0f, true, 1.0f, 1.0f, vec3f{0.7f, 0.7f, 0.7f});
  } else if (type == "test-bumps-normal") {
    return bump_to_normal(make_bumps(extents), 0.05f);
  } else if (type == "test-bumps-displacement") {
    return make_bumps(extents);
    // TODO(fabio): fix color space
    // img   = srgb_to_rgb(img);
  } else if (type == "test-checker-opacity") {
    return make_checker(extents, 1.0f, vec4f{1, 1, 1, 1}, vec4f{0, 0, 0, 0});
  } else if (type == "test-grid-opacity") {
    return make_grid(extents, 1.0f, vec4f{1, 1, 1, 1}, vec4f{0, 0, 0, 0});
  } else {
    throw io_error{"unknown preset " + type};
  }
}

// Saves a float image.
void save_image(const string& filename, const image<vec4f>& image) {
  // write data
  auto stbi_write_data = [](void* context, void* data, int size) {
    auto& buffer = *(vector<byte>*)context;
    buffer.insert(buffer.end(), (byte*)data, (byte*)data + size);
  };

  // grab data for low level apis
  auto [width, height] = image.size();
  auto num_channels    = 4;

  auto ext = path_extension(filename);
  if (ext == ".hdr" || ext == ".HDR") {
    auto buffer = vector<byte>{};
    if (!(bool)stbi_write_hdr_to_func(stbi_write_data, &buffer, width, height,
            num_channels, (const float*)image.data()))
      throw io_error{"cannot write " + filename};
    return save_binary(filename, buffer);
  } else if (ext == ".exr" || ext == ".EXR") {
    auto data  = (byte*)nullptr;
    auto count = (size_t)0;
    if (SaveEXRToMemory((const float*)image.data(), width, height, num_channels,
            1, &data, &count, nullptr) < 0)
      throw io_error{"cannot write " + filename};
    auto buffer = vector<byte>{data, data + count};
    free(data);
    return save_binary(filename, buffer);
  } else if (ext == ".png" || ext == ".PNG" || ext == ".jpg" || ext == ".JPG" ||
             ext == ".jpeg" || ext == ".JPEG" || ext == ".tga" ||
             ext == ".TGA" || ext == ".bmp" || ext == ".BMP") {
    return save_imageb(filename, float_to_byte(image));
  } else {
    throw io_error{"unsupported format " + filename};
  }
}

// Saves a byte image.
void save_imageb(const string& filename, const image<vec4b>& image) {
  // write data
  auto stbi_write_data = [](void* context, void* data, int size) {
    auto& buffer = *(vector<byte>*)context;
    buffer.insert(buffer.end(), (byte*)data, (byte*)data + size);
  };

  // grab data for low level apis
  auto [width, height] = image.size();
  auto num_channels    = 4;

  auto ext = path_extension(filename);
  if (ext == ".hdr" || ext == ".HDR" || ext == ".exr" || ext == ".EXR") {
    return save_image(filename, byte_to_float(image));
  } else if (ext == ".png" || ext == ".PNG") {
    auto buffer = vector<byte>{};
    if (!(bool)stbi_write_png_to_func(stbi_write_data, &buffer, width, height,
            num_channels, (const byte*)image.data(), width * 4))
      throw io_error{"cannot write " + filename};
    return save_binary(filename, buffer);
  } else if (ext == ".jpg" || ext == ".JPG" || ext == ".jpeg" ||
             ext == ".JPEG") {
    auto buffer = vector<byte>{};
    if (!(bool)stbi_write_jpg_to_func(stbi_write_data, &buffer, width, height,
            num_channels, (const byte*)image.data(), 75))
      throw io_error{"cannot write " + filename};
    return save_binary(filename, buffer);
  } else if (ext == ".tga" || ext == ".TGA") {
    auto buffer = vector<byte>{};
    if (!(bool)stbi_write_tga_to_func(stbi_write_data, &buffer, width, height,
            num_channels, (const byte*)image.data()))
      throw io_error{"cannot write " + filename};
    return save_binary(filename, buffer);
  } else if (ext == ".bmp" || ext == ".BMP") {
    auto buffer = vector<byte>{};
    if (!(bool)stbi_write_bmp_to_func(stbi_write_data, &buffer, width, height,
            num_channels, (const byte*)image.data()))
      throw io_error{"cannot write " + filename};
    return save_binary(filename, buffer);
  } else {
    throw io_error{"unsupported format " + filename};
  }
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// SHAPE IO
// -----------------------------------------------------------------------------
namespace yocto {

// Load mesh
shape_data load_shape(const string& filename, bool flip_texcoords) {
  auto shape = shape_data{};
  auto ext   = path_extension(filename);
  if (ext == ".ply" || ext == ".PLY") {
    auto ply = load_ply(filename);
    // TODO: remove when all as arrays
    get_positions(ply, (vector<array<float, 3>>&)shape.positions);
    get_normals(ply, (vector<array<float, 3>>&)shape.normals);
    get_texcoords(
        ply, (vector<array<float, 2>>&)shape.texcoords, flip_texcoords);
    get_colors(ply, (vector<array<float, 4>>&)shape.colors);
    get_radius(ply, shape.radius);
    get_faces(ply, (vector<array<int, 3>>&)shape.triangles,
        (vector<array<int, 4>>&)shape.quads);
    get_lines(ply, (vector<array<int, 2>>&)shape.lines);
    get_points(ply, shape.points);
    if (shape.points.empty() && shape.lines.empty() &&
        shape.triangles.empty() && shape.quads.empty())
      throw io_error{"empty shape " + filename};
  } else if (ext == ".obj" || ext == ".OBJ") {
    auto obj       = load_sobj(filename, false);
    auto materials = vector<int>{};
    // TODO: remove when all as arrays
    get_positions(obj, (vector<array<float, 3>>&)shape.positions);
    get_normals(obj, (vector<array<float, 3>>&)shape.normals);
    get_texcoords(
        obj, (vector<array<float, 2>>&)shape.texcoords, flip_texcoords);
    get_faces(obj, (vector<array<int, 3>>&)shape.triangles,
        (vector<array<int, 4>>&)shape.quads, materials);
    get_lines(obj, (vector<array<int, 2>>&)shape.lines, materials);
    get_points(obj, shape.points, materials);
    if (shape.points.empty() && shape.lines.empty() &&
        shape.triangles.empty() && shape.quads.empty())
      throw io_error{"empty shape " + filename};
  } else if (ext == ".stl" || ext == ".STL") {
    auto stl = load_stl(filename, true);
    if (stl.shapes.size() != 1) throw io_error{"empty shape " + filename};
    auto fnormals = vector<vec3f>{};
    if (!get_triangles(stl, 0, (vector<array<int, 3>>&)shape.triangles,
            (vector<array<float, 3>>&)shape.positions,
            (vector<array<float, 3>>&)fnormals))
      throw io_error{"empty shape " + filename};
  } else if (ext == ".ypreset" || ext == ".YPRESET") {
    shape = make_shape_preset(filename);
  } else {
    throw io_error("unsupported format " + filename);
  }
  return shape;
}

// Save ply mesh
void save_shape(const string& filename, const shape_data& shape,
    bool flip_texcoords, bool ascii) {
  auto ext = path_extension(filename);
  if (ext == ".ply" || ext == ".PLY") {
    auto ply = ply_model{};
    // TODO: remove when all as arrays
    add_positions(ply, (const vector<array<float, 3>>&)shape.positions);
    add_normals(ply, (const vector<array<float, 3>>&)shape.normals);
    add_texcoords(
        ply, (const vector<array<float, 2>>&)shape.texcoords, flip_texcoords);
    add_colors(ply, (const vector<array<float, 4>>&)shape.colors);
    add_radius(ply, shape.radius);
    add_faces(ply, (const vector<array<int, 3>>&)shape.triangles,
        (const vector<array<int, 4>>&)shape.quads);
    add_lines(ply, (const vector<array<int, 2>>&)shape.lines);
    add_points(ply, shape.points);
    save_ply(filename, ply);
  } else if (ext == ".obj" || ext == ".OBJ") {
    auto obj = obj_shape{};
    // TODO: remove when all as arrays
    add_positions(obj, (const vector<array<float, 3>>&)shape.positions);
    add_normals(obj, (const vector<array<float, 3>>&)shape.normals);
    add_texcoords(
        obj, (const vector<array<float, 2>>&)shape.texcoords, flip_texcoords);
    add_triangles(obj, (const vector<array<int, 3>>&)shape.triangles, 0,
        !shape.normals.empty(), !shape.texcoords.empty());
    add_quads(obj, (const vector<array<int, 4>>&)shape.quads, 0,
        !shape.normals.empty(), !shape.texcoords.empty());
    add_lines(obj, (const vector<array<int, 2>>&)shape.lines, 0,
        !shape.normals.empty(), !shape.texcoords.empty());
    add_points(
        obj, shape.points, 0, !shape.normals.empty(), !shape.texcoords.empty());
    save_obj(filename, obj);
  } else if (ext == ".stl" || ext == ".STL") {
    auto stl = stl_model{};
    if (!shape.lines.empty()) throw io_error{"empty shape " + filename};
    if (!shape.points.empty()) throw io_error{"empty shape " + filename};
    if (!shape.triangles.empty()) {
      add_triangles(stl, (const vector<array<int, 3>>&)shape.triangles,
          (const vector<array<float, 3>>&)shape.positions, {});
    } else if (!shape.quads.empty()) {
      auto triangles = quads_to_triangles(shape.quads);
      add_triangles(stl, (const vector<array<int, 3>>&)triangles,
          (const vector<array<float, 3>>&)shape.positions, {});
    } else {
      throw io_error{"empty shape " + filename};
    }
    save_stl(filename, stl);
  } else if (ext == ".cpp" || ext == ".CPP") {
    auto to_cpp = [](const string& name, const string& vname,
                      const auto& values) -> string {
      using T = typename std::remove_const_t<
          std::remove_reference_t<decltype(values)>>::value_type;
      if (values.empty()) return ""s;
      auto str = "auto " + name + "_" + vname + " = ";
      if constexpr (std::is_same_v<int, T>) str += "vector<int>{\n";
      if constexpr (std::is_same_v<float, T>) str += "vector<float>{\n";
      if constexpr (std::is_same_v<vec2i, T>) str += "vector<vec2i>{\n";
      if constexpr (std::is_same_v<vec2f, T>) str += "vector<vec2f>{\n";
      if constexpr (std::is_same_v<vec3i, T>) str += "vector<vec3i>{\n";
      if constexpr (std::is_same_v<vec3f, T>) str += "vector<vec3f>{\n";
      if constexpr (std::is_same_v<vec4i, T>) str += "vector<vec4i>{\n";
      if constexpr (std::is_same_v<vec4f, T>) str += "vector<vec4f>{\n";
      for (auto& value : values) {
        if constexpr (std::is_same_v<int, T> || std::is_same_v<float, T>) {
          str += std::to_string(value) + ",\n";
        } else if constexpr (std::is_same_v<vec2i, T> ||
                             std::is_same_v<vec2f, T>) {
          str += "{" + std::to_string(value.x) + "," + std::to_string(value.y) +
                 "},\n";
        } else if constexpr (std::is_same_v<vec3i, T> ||
                             std::is_same_v<vec3f, T>) {
          str += "{" + std::to_string(value.x) + "," + std::to_string(value.y) +
                 "," + std::to_string(value.z) + "},\n";
        } else if constexpr (std::is_same_v<vec4i, T> ||
                             std::is_same_v<vec4f, T>) {
          str += "{" + std::to_string(value.x) + "," + std::to_string(value.y) +
                 "," + std::to_string(value.z) + "," + std::to_string(value.w) +
                 "},\n";
        } else {
          throw std::invalid_argument{"cannot print this"};
        }
      }
      str += "};\n\n";
      return str;
    };

    auto name = string{"shape"};
    auto str  = ""s;
    str += to_cpp(name, "positions", shape.positions);
    str += to_cpp(name, "normals", shape.normals);
    str += to_cpp(name, "texcoords", shape.texcoords);
    str += to_cpp(name, "colors", shape.colors);
    str += to_cpp(name, "radius", shape.radius);
    str += to_cpp(name, "points", shape.points);
    str += to_cpp(name, "lines", shape.lines);
    str += to_cpp(name, "triangles", shape.triangles);
    str += to_cpp(name, "quads", shape.quads);
    save_text(filename, str);
  } else {
    throw io_error("unsupported format " + filename);
  }
}

// Load face-varying mesh
fvshape_data load_fvshape(const string& filename, bool flip_texcoords) {
  auto shape = fvshape_data{};
  auto ext   = path_extension(filename);
  if (ext == ".ply" || ext == ".PLY") {
    auto ply = load_ply(filename);
    // TODO: remove when all as arrays
    get_positions(ply, (vector<array<float, 3>>&)shape.positions);
    get_normals(ply, (vector<array<float, 3>>&)shape.normals);
    get_texcoords(
        ply, (vector<array<float, 2>>&)shape.texcoords, flip_texcoords);
    get_quads(ply, (vector<array<int, 4>>&)shape.quadspos);
    if (!shape.normals.empty()) shape.quadsnorm = shape.quadspos;
    if (!shape.texcoords.empty()) shape.quadstexcoord = shape.quadspos;
    if (shape.quadspos.empty()) throw io_error{"empty shape " + filename};
  } else if (ext == ".obj" || ext == ".OBJ") {
    auto obj = load_sobj(filename, true);
    // TODO: remove when all as arrays
    auto materials = vector<int>{};
    get_positions(obj, (vector<array<float, 3>>&)shape.positions);
    get_normals(obj, (vector<array<float, 3>>&)shape.normals);
    get_texcoords(
        obj, (vector<array<float, 2>>&)shape.texcoords, flip_texcoords);
    get_fvquads(obj, (vector<array<int, 4>>&)shape.quadspos,
        (vector<array<int, 4>>&)shape.quadsnorm,
        (vector<array<int, 4>>&)shape.quadstexcoord, materials);
    if (shape.quadspos.empty()) throw io_error{"empty shape " + filename};
  } else if (ext == ".stl" || ext == ".STL") {
    auto stl = load_stl(filename, true);
    if (stl.shapes.empty()) throw io_error{"empty shape " + filename};
    if (stl.shapes.size() > 1) throw io_error{"empty shape " + filename};
    auto fnormals  = vector<vec3f>{};
    auto triangles = vector<vec3i>{};
    if (!get_triangles(stl, 0, (vector<array<int, 3>>&)triangles,
            (vector<array<float, 3>>&)shape.positions,
            (vector<array<float, 3>>&)fnormals))
      throw io_error{"empty shape " + filename};
    shape.quadspos = triangles_to_quads(triangles);
  } else if (ext == ".ypreset" || ext == ".YPRESET") {
    shape = make_fvshape_preset(filename);
  } else {
    throw io_error("unsupported format " + filename);
  }
  return shape;
}

// Save ply mesh
void save_fvshape(const string& filename, const fvshape_data& shape,
    bool flip_texcoords, bool ascii) {
  auto ext = path_extension(filename);
  if (ext == ".ply" || ext == ".PLY") {
    return save_shape(filename, fvshape_to_shape(shape), flip_texcoords, ascii);
  } else if (ext == ".obj" || ext == ".OBJ") {
    auto obj = obj_shape{};
    // TODO: remove when all as arrays
    add_positions(obj, (const vector<array<float, 3>>&)shape.positions);
    add_normals(obj, (const vector<array<float, 3>>&)shape.normals);
    add_texcoords(
        obj, (const vector<array<float, 2>>&)shape.texcoords, flip_texcoords);
    add_fvquads(obj, (const vector<array<int, 4>>&)shape.quadspos,
        (const vector<array<int, 4>>&)shape.quadsnorm,
        (const vector<array<int, 4>>&)shape.quadstexcoord, 0);
    save_obj(filename, obj);
  } else if (ext == ".stl" || ext == ".STL") {
    return save_shape(filename, fvshape_to_shape(shape), flip_texcoords, ascii);
  } else if (ext == ".cpp" || ext == ".CPP") {
    auto to_cpp = [](const string& name, const string& vname,
                      const auto& values) -> string {
      using T = typename std::remove_const_t<
          std::remove_reference_t<decltype(values)>>::value_type;
      if (values.empty()) return ""s;
      auto str = "auto " + name + "_" + vname + " = ";
      if constexpr (std::is_same_v<int, T>) str += "vector<int>{\n";
      if constexpr (std::is_same_v<float, T>) str += "vector<float>{\n";
      if constexpr (std::is_same_v<vec2i, T>) str += "vector<vec2i>{\n";
      if constexpr (std::is_same_v<vec2f, T>) str += "vector<vec2f>{\n";
      if constexpr (std::is_same_v<vec3i, T>) str += "vector<vec3i>{\n";
      if constexpr (std::is_same_v<vec3f, T>) str += "vector<vec3f>{\n";
      if constexpr (std::is_same_v<vec4i, T>) str += "vector<vec4i>{\n";
      if constexpr (std::is_same_v<vec4f, T>) str += "vector<vec4f>{\n";
      for (auto& value : values) {
        if constexpr (std::is_same_v<int, T> || std::is_same_v<float, T>) {
          str += std::to_string(value) + ",\n";
        } else if constexpr (std::is_same_v<vec2i, T> ||
                             std::is_same_v<vec2f, T>) {
          str += "{" + std::to_string(value.x) + "," + std::to_string(value.y) +
                 "},\n";
        } else if constexpr (std::is_same_v<vec3i, T> ||
                             std::is_same_v<vec3f, T>) {
          str += "{" + std::to_string(value.x) + "," + std::to_string(value.y) +
                 "," + std::to_string(value.z) + "},\n";
        } else if constexpr (std::is_same_v<vec4i, T> ||
                             std::is_same_v<vec4f, T>) {
          str += "{" + std::to_string(value.x) + "," + std::to_string(value.y) +
                 "," + std::to_string(value.z) + "," + std::to_string(value.w) +
                 "},\n";
        } else {
          throw std::invalid_argument{"cannot print this"};
        }
      }
      str += "};\n\n";
      return str;
    };
    auto name = string{"shape"};
    auto str  = ""s;
    str += to_cpp(name, "positions", shape.positions);
    str += to_cpp(name, "normals", shape.normals);
    str += to_cpp(name, "texcoords", shape.texcoords);
    str += to_cpp(name, "quadspos", shape.quadspos);
    str += to_cpp(name, "quadsnorm", shape.quadsnorm);
    str += to_cpp(name, "quadstexcoord", shape.quadstexcoord);
    save_text(filename, str);
  } else {
    throw io_error("unsupported format " + filename);
  }
}

// Shape presets used for testing.
shape_data make_shape_preset(const string& type_) {
  auto type = path_basename(type_);
  if (type == "default-quad") {
    return make_rect();
  } else if (type == "default-quady") {
    return make_recty();
  } else if (type == "default-cube") {
    return make_box();
  } else if (type == "default-cube-rounded") {
    return make_rounded_box();
  } else if (type == "default-sphere") {
    return make_sphere();
  } else if (type == "default-matcube") {
    return make_rounded_box();
  } else if (type == "default-matsphere") {
    return make_uvspherey();
  } else if (type == "default-disk") {
    return make_disk();
  } else if (type == "default-disk-bulged") {
    return make_bulged_disk();
  } else if (type == "default-quad-bulged") {
    return make_bulged_rect();
  } else if (type == "default-uvsphere") {
    return make_uvsphere();
  } else if (type == "default-uvsphere-flipcap") {
    return make_capped_uvsphere();
  } else if (type == "default-uvspherey") {
    return make_uvspherey();
  } else if (type == "default-uvspherey-flipcap") {
    return make_capped_uvspherey();
  } else if (type == "default-uvdisk") {
    return make_uvdisk();
  } else if (type == "default-uvcylinder") {
    return make_uvcylinder();
  } else if (type == "default-uvcylinder-rounded") {
    return make_rounded_uvcylinder({32, 32, 32});
  } else if (type == "default-geosphere") {
    return make_geosphere();
  } else if (type == "default-floor") {
    return make_floor();
  } else if (type == "default-floor-bent") {
    return make_bent_floor();
  } else if (type == "default-matball") {
    return make_sphere();
  } else if (type == "default-hairball") {
    auto base = make_sphere(pow2(5), 0.8f);
    return make_hair(base, {4, 65536}, {0.2f, 0.2f}, {0.002f, 0.001f});
  } else if (type == "default-hairball-interior") {
    return make_sphere(pow2(5), 0.8f);
  } else if (type == "default-suzanne") {
    return make_monkey();
  } else if (type == "default-quady-displaced") {
    return make_recty({256, 256});
  } else if (type == "default-sphere-displaced") {
    return make_sphere(128);
  } else if (type == "test-cube") {
    auto shape = make_rounded_box(
        {32, 32, 32}, {0.075f, 0.075f, 0.075f}, {1, 1, 1}, 0.3f * 0.075f);
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    return shape;
  } else if (type == "test-uvsphere") {
    auto shape = make_uvsphere({32, 32}, 0.075f);
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    return shape;
  } else if (type == "test-uvsphere-flipcap") {
    auto shape = make_capped_uvsphere({32, 32}, 0.075f, {1, 1}, 0.3f * 0.075f);
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    return shape;
  } else if (type == "test-uvspherey") {
    auto shape = make_uvspherey({32, 32}, 0.075f);
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    return shape;
  } else if (type == "test-uvspherey-flipcap") {
    auto shape = make_capped_uvspherey({32, 32}, 0.075f, {1, 1}, 0.3f * 0.075f);
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    return shape;
  } else if (type == "test-sphere") {
    auto shape = make_sphere(32, 0.075f, 1);
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    return shape;
  } else if (type == "test-matcube") {
    auto shape = make_rounded_box(
        {32, 32, 32}, {0.075f, 0.075f, 0.075f}, {1, 1, 1}, 0.3f * 0.075f);
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    return shape;
  } else if (type == "test-matsphere") {
    auto shape = make_uvspherey({32, 32}, 0.075f, {2, 1});
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    return shape;
  } else if (type == "test-sphere-displaced") {
    auto shape = make_sphere(128, 0.075f, 1);
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    return shape;
  } else if (type == "test-smallsphere") {
    auto shape = make_sphere(32, 0.015f, 1);
    for (auto& p : shape.positions) p += {0, 0.015f, 0};
    return shape;
  } else if (type == "test-disk") {
    auto shape = make_disk(32, 0.075f, 1);
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    return shape;
  } else if (type == "test-uvcylinder") {
    auto shape = make_rounded_uvcylinder(
        {32, 32, 32}, {0.075f, 0.075f}, {1, 1, 1}, 0.3f * 0.075f);
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    return shape;
  } else if (type == "test-floor") {
    return make_floor({1, 1}, {2, 2}, {20, 20});
  } else if (type == "test-smallfloor") {
    return make_floor({1, 1}, {0.5f, 0.5f}, {1, 1});
  } else if (type == "test-quad") {
    return make_rect({1, 1}, {0.075f, 0.075f}, {1, 1});
  } else if (type == "test-quady") {
    return make_recty({1, 1}, {0.075f, 0.075f}, {1, 1});
  } else if (type == "test-quad-displaced") {
    return make_rect({256, 256}, {0.075f, 0.075f}, {1, 1});
  } else if (type == "test-quady-displaced") {
    return make_recty({256, 256}, {0.075f, 0.075f}, {1, 1});
  } else if (type == "test-matball") {
    auto shape = make_sphere(32, 0.075f);
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    return shape;
  } else if (type == "test-geosphere") {
    auto shape = make_geosphere(3, 0.075f);
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    return shape;
  } else if (type == "test-geosphere-flat") {
    auto shape = make_geosphere(3, 0.075f);
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    shape.normals = {};
    return shape;
  } else if (type == "test-geosphere-subdivided") {
    auto shape = make_geosphere(6, 0.075f);
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    return shape;
  } else if (type == "test-hairball1") {
    auto base = make_sphere(32, 0.075f * 0.8f, 1);
    for (auto& p : base.positions) p += {0, 0.075f, 0};
    return make_hair(base, {4, 65536}, {0.1f * 0.15f, 0.1f * 0.15f},
        {0.001f * 0.15f, 0.0005f * 0.15f}, {0.03f, 100});
  } else if (type == "test-hairball2") {
    auto base = make_sphere(32, 0.075f * 0.8f, 1);
    for (auto& p : base.positions) p += {0, 0.075f, 0};
    return make_hair(base, {4, 65536}, {0.1f * 0.15f, 0.1f * 0.15f},
        {0.001f * 0.15f, 0.0005f * 0.15f});
  } else if (type == "test-hairball3") {
    auto base = make_sphere(32, 0.075f * 0.8f, 1);
    for (auto& p : base.positions) p += {0, 0.075f, 0};
    return make_hair(base, {4, 65536}, {0.1f * 0.15f, 0.1f * 0.15f},
        {0.001f * 0.15f, 0.0005f * 0.15f}, {0, 0}, {0.5, 128});
  } else if (type == "test-hairball-interior") {
    auto shape = make_sphere(32, 0.075f * 0.8f, 1);
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    return shape;
  } else if (type == "test-suzanne-subdiv") {
    auto shape = make_monkey(0, 0.075f * 0.8f);
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    return shape;
  } else if (type == "test-cube-subdiv") {
    auto fvshape    = make_fvcube(0, 0.075f);
    auto shape      = shape_data{};
    shape.quads     = fvshape.quadspos;
    shape.positions = fvshape.positions;
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    return shape;
  } else if (type == "test-arealight1") {
    return make_rect({1, 1}, {0.2f, 0.2f});
  } else if (type == "test-arealight2") {
    return make_rect({1, 1}, {0.2f, 0.2f});
  } else if (type == "test-largearealight1") {
    return make_rect({1, 1}, {0.4f, 0.4f});
  } else if (type == "test-largearealight2") {
    return make_rect({1, 1}, {0.4f, 0.4f});
  } else if (type == "test-pointlight1") {
    return make_point(0);
  } else if (type == "test-pointlight2") {
    return make_point(0);
  } else if (type == "test-point") {
    return make_points(1);
  } else if (type == "test-points") {
    return make_points(4096);
  } else if (type == "test-points-random") {
    auto shape = make_random_points(4096, {0.075f, 0.075f, 0.075f});
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    return shape;
  } else if (type == "test-points-grid") {
    auto shape = make_points({256, 256}, {0.075f, 0.075f});
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    for (auto& r : shape.radius) r *= 0.075f;
    return shape;
  } else if (type == "test-lines-grid") {
    auto shape = make_lines(256, 256, {0.075f, 0.075f});
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    for (auto& r : shape.radius) r *= 0.075f;
    return shape;
  } else if (type == "test-thickpoints-grid") {
    auto shape = make_points({16, 16}, {0.075f, 0.075f});
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    for (auto& r : shape.radius) r *= 0.075f * 10;
    return shape;
  } else if (type == "test-thicklines-grid") {
    auto shape = make_lines(16, 16, {0.075f, 0.075f});
    for (auto& p : shape.positions) p += {0, 0.075f, 0};
    for (auto& r : shape.radius) r *= 0.075f * 10;
    return shape;
  } else if (type == "test-particles") {
    return make_points(4096);
  } else if (type == "test-cloth") {
    return make_rect({64, 64}, {0.2f, 0.2f});
  } else if (type == "test-clothy") {
    return make_recty({64, 64}, {0.2f, 0.2f});
  } else {
    throw io_error{"unknown preset " + type};
  }
}

// Shape presets used for testing.
fvshape_data make_fvshape_preset(const string& type) {
  return shape_to_fvshape(make_shape_preset(type));
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// TEXTURE IO
// -----------------------------------------------------------------------------
namespace yocto {

// Loads/saves an image. Chooses hdr or ldr based on file name.
texture_data load_texture(const string& filename) {
  auto ext = path_extension(filename);
  if (ext == ".exr" || ext == ".EXR" || ext == ".hdr" || ext == ".HDR") {
    return {.pixelsf = load_image(filename)};
  } else if (ext == ".png" || ext == ".PNG" || ext == ".jpg" || ext == ".JPG" ||
             ext == ".jpeg" || ext == ".JPEG" || ext == ".tga" ||
             ext == ".TGA" || ext == ".bmp" || ext == ".BMP") {
    return {.pixelsb = load_imageb(filename)};
  } else if (ext == ".ypreset" || ext == ".YPRESET") {
    return make_texture_preset(filename);
  } else {
    throw io_error("unsupported format " + filename);
  }
}

// Saves an hdr image.
void save_texture(const string& filename, const texture_data& texture) {
  if (!texture.pixelsf.empty()) {
    save_image(filename, texture.pixelsf);
  } else {
    save_imageb(filename, texture.pixelsb);
  }
}

texture_data make_texture_preset(const string& type) {
  return image_to_texture(make_image_preset(type), !is_srgb_preset(type));
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// UTILITIES
// -----------------------------------------------------------------------------
namespace yocto {

// make element name
[[maybe_unused]] static string get_element_name(
    const string& name, int idx, size_t size) {
  // there are much better ways to do this, but fine for now
  auto num_str  = std::to_string(idx + 1);
  auto size_str = std::to_string(size + 1);
  while (num_str.size() < size_str.size()) num_str = "0" + num_str;
  return name + num_str;
}

// get name
[[maybe_unused]] static string get_camera_name(
    const scene_data& scene, int idx) {
  if (idx < 0) return "";
  if (scene.camera_names.empty())
    return get_element_name("camera", idx, scene.cameras.size());
  return scene.camera_names[idx];
}
[[maybe_unused]] static string get_environment_name(
    const scene_data& scene, int idx) {
  if (idx < 0) return "";
  if (scene.environment_names.empty())
    return get_element_name("environment", idx, scene.environments.size());
  return scene.environment_names[idx];
}
[[maybe_unused]] static string get_shape_name(
    const scene_data& scene, int idx) {
  if (idx < 0) return "";
  if (scene.shape_names.empty() || scene.shape_names[idx].empty())
    return get_element_name("shape", idx, scene.shapes.size());
  return scene.shape_names[idx];
}
[[maybe_unused]] static string get_texture_name(
    const scene_data& scene, int idx) {
  if (idx < 0) return "";
  if (scene.texture_names.empty())
    return get_element_name("texture", idx, scene.textures.size());
  return scene.texture_names[idx];
}
[[maybe_unused]] static string get_instance_name(
    const scene_data& scene, int idx) {
  if (idx < 0) return "";
  if (scene.instance_names.empty())
    return get_element_name("instance", idx, scene.instances.size());
  return scene.instance_names[idx];
}
[[maybe_unused]] static string get_material_name(
    const scene_data& scene, int idx) {
  if (idx < 0) return "";
  if (scene.material_names.empty())
    return get_element_name("material", idx, scene.materials.size());
  return scene.material_names[idx];
}
[[maybe_unused]] static string get_subdiv_name(
    const scene_data& scene, int idx) {
  if (idx < 0) return "";
  if (scene.subdiv_names.empty())
    return get_element_name("subdiv", idx, scene.subdivs.size());
  return scene.subdiv_names[idx];
}

[[maybe_unused]] static string get_camera_name(
    const scene_data& scene, const camera_data& camera) {
  return get_camera_name(scene, (int)(&camera - scene.cameras.data()));
}
[[maybe_unused]] static string get_environment_name(
    const scene_data& scene, const environment_data& environment) {
  return get_environment_name(
      scene, (int)(&environment - scene.environments.data()));
}
[[maybe_unused]] static string get_shape_name(
    const scene_data& scene, const shape_data& shape) {
  return get_shape_name(scene, (int)(&shape - scene.shapes.data()));
}
[[maybe_unused]] static string get_texture_name(
    const scene_data& scene, const texture_data& texture) {
  return get_texture_name(scene, (int)(&texture - scene.textures.data()));
}
[[maybe_unused]] static string get_instance_name(
    const scene_data& scene, const instance_data& instance) {
  return get_instance_name(scene, (int)(&instance - scene.instances.data()));
}
[[maybe_unused]] static string get_material_name(
    const scene_data& scene, const material_data& material) {
  return get_material_name(scene, (int)(&material - scene.materials.data()));
}
[[maybe_unused]] static string get_subdiv_name(
    const scene_data& scene, const subdiv_data& subdiv) {
  return get_subdiv_name(scene, (int)(&subdiv - scene.subdivs.data()));
}

template <typename T>
static vector<string> make_names(const vector<T>& elements,
    const vector<string>& names, const string& prefix) {
  if (names.size() == elements.size()) return names;
  auto nnames = vector<string>(elements.size());
  for (auto idx : range(elements.size())) {
    // there are much better ways to do this, but fine for now
    auto num_str  = std::to_string(idx + 1);
    auto size_str = std::to_string(elements.size());
    while (num_str.size() < size_str.size()) num_str = "0" + num_str;
    nnames[idx] = prefix + num_str;
  }
  return nnames;
}

// Add missing cameras.
void add_missing_camera(scene_data& scene) {
  if (!scene.cameras.empty()) return;
  scene.camera_names.emplace_back("camera");
  auto& camera        = scene.cameras.emplace_back();
  camera.orthographic = false;
  camera.film         = 0.036f;
  camera.aspect       = (float)16 / (float)9;
  camera.aperture     = 0;
  camera.lens         = 0.050f;
  auto bbox           = compute_bounds(scene);
  auto center         = (bbox.max + bbox.min) / 2;
  auto bbox_radius    = length(bbox.max - bbox.min) / 2;
  auto camera_dir     = vec3f{0, 0, 1};
  auto camera_dist = bbox_radius * camera.lens / (camera.film / camera.aspect);
  camera_dist *= 2.0f;  // correction for tracer camera implementation
  auto from    = camera_dir * camera_dist + center;
  auto to      = center;
  auto up      = vec3f{0, 1, 0};
  camera.frame = lookat_frame(from, to, up);
  camera.focus = length(from - to);
}

// Add missing radius.
static void add_missing_radius(scene_data& scene, float radius = 0.001f) {
  for (auto& shape : scene.shapes) {
    if (shape.points.empty() && shape.lines.empty()) continue;
    if (!shape.radius.empty()) continue;
    shape.radius.assign(shape.positions.size(), radius);
  }
}

// Add missing cameras.
void add_missing_material(scene_data& scene) {
  auto default_material = invalidid;
  for (auto& instance : scene.instances) {
    if (instance.material >= 0) continue;
    if (default_material == invalidid) {
      auto& material   = scene.materials.emplace_back();
      material.type    = material_type::matte;
      material.color   = {0.8f, 0.8f, 0.8f};
      default_material = (int)scene.materials.size() - 1;
    }
    instance.material = default_material;
  }
}

// Add missing cameras.
void add_missing_lights(scene_data& scene) {
  if (has_lights(scene)) return;
  add_sky(scene);
}

// Reduce memory usage
static void trim_memory(scene_data& scene) {
  for (auto& shape : scene.shapes) {
    shape.points.shrink_to_fit();
    shape.lines.shrink_to_fit();
    shape.triangles.shrink_to_fit();
    shape.quads.shrink_to_fit();
    shape.positions.shrink_to_fit();
    shape.normals.shrink_to_fit();
    shape.texcoords.shrink_to_fit();
    shape.colors.shrink_to_fit();
    shape.radius.shrink_to_fit();
    shape.tangents.shrink_to_fit();
  }
  for (auto& subdiv : scene.subdivs) {
    subdiv.positions.shrink_to_fit();
    subdiv.normals.shrink_to_fit();
    subdiv.texcoords.shrink_to_fit();
    subdiv.quadspos.shrink_to_fit();
    subdiv.quadsnorm.shrink_to_fit();
    subdiv.quadstexcoord.shrink_to_fit();
  }
  scene.cameras.shrink_to_fit();
  scene.shapes.shrink_to_fit();
  scene.subdivs.shrink_to_fit();
  scene.instances.shrink_to_fit();
  scene.materials.shrink_to_fit();
  scene.textures.shrink_to_fit();
  scene.environments.shrink_to_fit();
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// TEST SCENES
// -----------------------------------------------------------------------------
namespace yocto {

enum struct test_cameras_type { standard, wide };
enum struct test_environments_type { none, sky, sunsky };
enum struct test_arealights_type { none, standard, large };
enum struct test_floor_type { none, standard };
enum struct test_instance_name_type { material, shape };
enum struct test_shapes_type {
  // clang-format off
  features1, features2, rows, bunny_sphere,
  shapes1, shapes2, shapes3
  // clang-format off
};
enum struct test_materials_type {
  // clang-format off
  features1, features2, uvgrid, hair, plastic_metal,
  materials1, materials2, materials3, materials4, materials5,
  // clang-format on
};

struct test_params {
  test_cameras_type       cameras       = test_cameras_type::standard;
  test_environments_type  environments  = test_environments_type::sky;
  test_arealights_type    arealights    = test_arealights_type::standard;
  test_floor_type         floor         = test_floor_type::standard;
  test_shapes_type        shapes        = test_shapes_type::features1;
  test_materials_type     materials     = test_materials_type::features1;
  test_instance_name_type instance_name = test_instance_name_type::material;
};

// Scene test
scene_data make_test(const test_params& params) {
  return {};
  // // cameras
  // switch (params.cameras) {
  //   case test_cameras_type::standard: {
  //     add_camera(scene, "default", {-0.75, 0.4, 0.9}, {-0.075, 0.05, -0.05},
  //         {0, 1, 0}, 0.05, 2.4, 0);
  //   } break;
  //   // TODO(fabio): fix wide camera
  //   case test_cameras_type::wide: {
  //     add_camera(scene, "default", {-0.75, 0.4, 0.9}, {-0.075, 0.05, -0.05},
  //         {0, 1, 0}, 0.05, 2.4, 0);
  //   } break;
  // }
  // // TODO(fabio): port other cameras
  // switch (params.environments) {
  //   case test_environments_type::none: break;
  //   case test_environments_type::sky: {
  //     add_environment(scene, "sky", {{1,0,0}, {0,1,0}, {0,0,1}, {0,0,0}},
  //     {0.5, 0.5, 0.5},
  //         add_texture(scene, "sky",
  //             make_sunsky(
  //                 {2048, 1024}, pif / 4, 3.0, false, 1.0, 1.0, {0.7, 0.7,
  //                 0.7}),
  //             true));
  //   } break;
  //   case test_environments_type::sunsky: {
  //     add_environment(scene, "sunsky", {{1,0,0}, {0,1,0}, {0,0,1}, {0,0,0}},
  //     {0.5, 0.5, 0.5},
  //         add_texture(scene, "sky",
  //             make_sunsky(
  //                 {2048, 1024}, pif / 4, 3.0, true, 1.0, 1.0, {0.7, 0.7,
  //                 0.7}),
  //             true));
  //   } break;
  // }
  // switch (params.arealights) {
  //   case test_arealights_type::none: break;
  //   case test_arealights_type::standard: {
  //     add_instance(scene, "arealight1",
  //         lookat_frame({-0.4, 0.8, 0.8}, {0, 0.1, 0}, {0, 1, 0}, true),
  //         add_shape(scene, "arealight1", make_rect({1, 1}, {0.2, 0.2})),
  //         add_emission_material(
  //             scene, "arealight1", {20, 20, 20}, invalidid));
  //     add_instance(scene, "arealight2",
  //         lookat_frame({+0.4, 0.8, 0.8}, {0, 0.1, 0}, {0, 1, 0}, true),
  //         add_shape(scene, "arealight2", make_rect({1, 1}, {0.2, 0.2})),
  //         add_emission_material(
  //             scene, "arealight2", {20, 20, 20}, invalidid));
  //   } break;
  //   case test_arealights_type::large: {
  //     add_instance(scene, "largearealight1",
  //         lookat_frame({-0.8, 1.6, 1.6}, {0, 0.1, 0}, {0, 1, 0}, true),
  //         add_shape(scene, "largearealight1", make_rect({1, 1}, {0.4, 0.4})),
  //         add_emission_material(
  //             scene, "largearealight1", {10, 10, 10}, invalidid));
  //     add_instance(scene, "largearealight2",
  //         lookat_frame({+0.8, 1.6, 1.6}, {0, 0.1, 0}, {0, 1, 0}, true),
  //         add_shape(scene, "largearealight2", make_rect({1, 1}, {0.4, 0.4})),
  //         add_emission_material(
  //             scene, "largearealight2", {10, 10, 10}, invalidid));
  //   } break;
  // }
  // switch (params.floor) {
  //   case test_floor_type::none: break;
  //   case test_floor_type::standard: {
  //     add_instance(scene, "floor", {{1,0,0}, {0,1,0}, {0,0,1}, {0,0,0}},
  //         add_shape(scene, "floor", make_floor({1, 1}, {2, 2}, {20, 20})),
  //         add_matte_material(scene, "floor", {1, 1, 1},
  //             add_texture(scene, "floor", make_grid({1024, 1024}))));
  //   } break;
  // }
  // auto shapes = vector<int>{}, shapesi =
  // vector<int>{}; auto subdivs   =
  // vector<int>{}; auto materials =
  // vector<int>{}; switch (params.shapes) {
  //   case test_shapes_type::features1: {
  //     auto bunny  = add_shape(scene, "sphere", make_sphere(32, 0.075, 1));
  //     auto sphere = add_shape(scene, "sphere", make_sphere(32, 0.075, 1));
  //     shapes      = {bunny, sphere, bunny, sphere, bunny};
  //   } break;
  //   case test_shapes_type::features2: {
  //     shapes  = {add_shape(scene, "sphere", make_sphere(32, 0.075, 1)),
  //         add_shape(scene, "suzanne", make_monkey(0.075f * 0.8f)),
  //         add_shape(scene, "hair",
  //             make_hair(make_sphere(32, 0.075f * 0.8f, 1), {4, 65536},
  //                 {0.1f * 0.15f, 0.1f * 0.15f},
  //                 {0.001f * 0.15f, 0.0005f * 0.15f}, {0.03, 100})),
  //         add_shape(scene, "displaced", make_sphere(128, 0.075f, 1)),
  //         add_shape(scene, "cube",
  //             make_rounded_box({32, 32, 32}, {0.075, 0.075, 0.075}, {1, 1,
  //             1},
  //                 0.3 * 0.075f))};
  //     shapesi = {invalidid, invalidid,
  //         add_shape(scene, "hairi", make_sphere(32, 0.075f * 0.8f, 1)),
  //         invalidid, invalidid};
  //     subdivs = {add_subdiv(scene, "suzanne", make_monkey(0.075f * 0.8f),
  //                    shapes[1], 2),
  //         add_subdiv(scene, "displaced", make_sphere(128, 0.075f, 1),
  //         shapes[3],
  //             0, 0.025,
  //             add_texture(scene, "bumps-displacement", make_bumps({1024,
  //             1024}),
  //                 false, true))};
  //   } break;
  //   case test_shapes_type::rows: {
  //     auto bunny  = add_shape(scene, "bunny", make_sphere(32, 0.075, 1));
  //     auto sphere = add_shape(scene, "sphere", make_sphere(32, 0.075, 1));
  //     shapes      = {bunny, bunny, bunny, bunny, bunny, sphere, sphere,
  //     sphere,
  //         sphere, sphere};
  //   } break;
  //   case test_shapes_type::bunny_sphere: {
  //     auto bunny  = add_shape(scene, "bunny", make_sphere(32, 0.075, 1));
  //     auto sphere = add_shape(scene, "sphere", make_sphere(32, 0.075, 1));
  //     shapes      = {bunny, sphere, bunny, sphere, bunny};
  //   } break;
  //   case test_shapes_type::shapes1: {
  //     shapes = {
  //         add_shape(scene, "sphere", make_sphere(32, 0.075, 1)),
  //         add_shape(scene, "uvsphere-flipcap",
  //             make_capped_uvsphere({32, 32}, 0.075, {1, 1}, 0.3 * 0.075)),
  //         add_shape(scene, "disk", make_disk(32, 0.075f, 1)),
  //         add_shape(scene, "uvcylinder",
  //             make_rounded_uvcylinder(
  //                 {32, 32, 32}, {0.075, 0.075}, {1, 1, 1}, 0.3 * 0.075)),
  //         add_shape(scene, "cube",
  //             make_rounded_box({32, 32, 32}, {0.075, 0.075, 0.075}, {1, 1,
  //             1},
  //                 0.3 * 0.075f)),
  //     };
  //   } break;
  //   case test_shapes_type::shapes2: {
  //     shapes = {
  //         add_shape(scene, "cube-subdiv", make_fvcube(0.075)),
  //         add_shape(scene, "suzanne-subdiv", make_monkey(0.075)),
  //         add_shape(scene, "displaced", make_sphere(128, 0.075f, 1)),
  //         add_shape(scene, "bunny", make_sphere(32, 0.075, 1)),
  //         add_shape(scene, "teapot", make_sphere(32, 0.075, 1)),
  //     };
  //     subdivs = {
  //         add_subdiv(scene, "cube-subdiv", make_fvcube(0.075), shapes[0], 4),
  //         add_subdiv(scene, "suzanne-subdiv", make_monkey(0.075), shapes[1],
  //         2), add_subdiv(scene, "displaced", make_sphere(128, 0.075f, 1),
  //         shapes[2],
  //             0, 0.025,
  //             add_texture(scene, "bumps-displacement", make_bumps({1024,
  //             1024}),
  //                 false, true))};
  //   } break;
  //   case test_shapes_type::shapes3: {
  //     shapes = {
  //         invalidid,
  //         add_shape(scene, "hair1",
  //             make_hair(make_sphere(32, 0.075f * 0.8f, 1), {4, 65536},
  //                 {0.1f * 0.15f, 0.1f * 0.15f},
  //                 {0.001f * 0.15f, 0.0005f * 0.15f}, {0.03, 100})),
  //         add_shape(scene, "hair2",
  //             make_hair(make_sphere(32, 0.075f * 0.8f, 1), {4, 65536},
  //                 {0.1f * 0.15f, 0.1f * 0.15f},
  //                 {0.001f * 0.15f, 0.0005f * 0.15f})),
  //         add_shape(scene, "hair3",
  //             make_hair(make_sphere(32, 0.075f * 0.8f, 1), {4, 65536},
  //                 {0.1f * 0.15f, 0.1f * 0.15f},
  //                 {0.001f * 0.15f, 0.0005f * 0.15f}, {0, 0}, {0.5, 128})),
  //         invalidid,
  //     };
  //   } break;
  // }
  // switch (params.materials) {
  //   case test_materials_type::features1: {
  //     materials = {
  //         add_plastic_material(scene, "coated", {1, 1, 1}, 0.2,
  //             add_texture(scene, "uvgrid", make_uvgrid({1024, 1024}))),
  //         add_glass_material(scene, "glass", {1, 0.5, 0.5}, 0),
  //         add_glass_material(
  //             scene, "jade", {0.5, 0.5, 0.5}, 0, {0.3, 0.6, 0.3}),
  //         add_plastic_material(scene, "bumped", {0.5, 0.7, 0.5}, 0.2,
  //             invalidid, invalidid,
  //             add_texture(scene, "bumps-normal",
  //                 bump_to_normal(make_bumps({1024, 1024}), 0.05), false,
  //                 true)),
  //         add_metal_material(scene, "metal", {0.66, 0.45, 0.34}, 0.2),
  //     };
  //   } break;
  //   case test_materials_type::features2: {
  //     auto uvgrid  = add_plastic_material(scene, "uvgrid", {1, 1, 1}, 0.2,
  //         add_texture(scene, "uvgrid", make_uvgrid({1024, 1024})));
  //     auto plastic = add_plastic_material(
  //         scene, "plastic", {0.5, 0.7, 0.5}, 0.2);
  //     auto hair = add_matte_material(scene, "hair", {0.7, 0.7, 0.7});
  //     materials = {uvgrid, plastic, hair, plastic, uvgrid};
  //   } break;
  //   case test_materials_type::uvgrid: {
  //     auto uvgrid = add_plastic_material(scene, "uvgrid", {1, 1, 1}, 0.2,
  //         add_texture(scene, "uvgrid", make_uvgrid({1024, 1024})));
  //     materials   = {uvgrid, uvgrid, uvgrid, uvgrid, uvgrid};
  //   } break;
  //   case test_materials_type::hair: {
  //     auto hair = add_matte_material(scene, "hair", {0.7, 0.7, 0.7});
  //     materials = {hair, hair, hair, hair, hair};
  //   } break;
  //   case test_materials_type::plastic_metal: {
  //     materials = {
  //         add_plastic_material(scene, "plastic1", {0.5, 0.5, 0.7}, 0.01),
  //         add_plastic_material(scene, "plastic2", {0.5, 0.7, 0.5}, 0.2),
  //         add_matte_material(scene, "matte", {0.7, 0.7, 0.7}),
  //         add_metal_material(scene, "metal1", {0.7, 0.7, 0.7}, 0),
  //         add_metal_material(scene, "metal2", {0.66, 0.45, 0.34}, 0.2),
  //     };
  //   } break;
  //   case test_materials_type::materials1: {
  //     materials = {
  //         add_plastic_material(scene, "plastic1", {0.5, 0.5, 0.7}, 0.01),
  //         add_plastic_material(scene, "plastic2", {0.5, 0.7, 0.5}, 0.2),
  //         add_matte_material(scene, "matte", {0.7, 0.7, 0.7}),
  //         add_plastic_material(scene, "metal1", {0.7, 0.7, 0.7}, 0),
  //         add_plastic_material(scene, "metal2", {0.66, 0.45, 0.34}, 0.2),
  //     };
  //   } break;
  //   case test_materials_type::materials2: {
  //     materials = {
  //         add_glass_material(scene, "glass1", {1, 1, 1}, 0),
  //         add_glass_material(scene, "glass2", {1, 0.7, 0.7}, 0.1),
  //         add_transparent_material(scene, "transparent", {0.7, 0.5, 0.5},
  //         0.2), add_thinglass_material(scene, "tglass1", {1, 1, 1}, 0),
  //         add_thinglass_material(scene, "tglass2", {1, 0.7, 0.7}, 0.1),
  //     };
  //   } break;
  //   case test_materials_type::materials3: {
  //     auto bumps_normal = add_texture(scene, "bumps-normal",
  //         bump_to_normal(make_bumps({1024, 1024}), 0.05), false, true);
  //     materials         = {
  //         add_plastic_material(scene, "plastic1", {0.5, 0.5, 0.7}, 0.01,
  //             invalidid, invalidid, bumps_normal),
  //         add_plastic_material(scene, "plastic2", {0.5, 0.7, 0.5}, 0.2),
  //         add_metal_material(scene, "metal1", {0.7, 0.7, 0.7}, 0,
  //             invalidid, invalidid, bumps_normal),
  //         add_metal_material(scene, "metal2", {0.66, 0.45, 0.34}, 0.2),
  //         add_metal_material(scene, "metal3", {0.66, 0.45, 0.34}, 0.2),
  //     };
  //   } break;
  //   case test_materials_type::materials4: {
  //     materials = {
  //         add_volume_material(
  //             scene, "cloud", {0.65, 0.65, 0.65}, {0.9, 0.9, 0.9}, 1),
  //         add_glass_material(scene, "glass", {1, 0.5, 0.5}, 0),
  //         add_glass_material(
  //             scene, "jade", {0.5, 0.5, 0.5}, 0, {0.3, 0.6, 0.3}),
  //         add_glass_material(
  //             scene, "jade2", {0.5, 0.5, 0.5}, 0, {0.3, 0.6, 0.3}),
  //         add_volume_material(scene, "smoke", {0.5, 0.5, 0.5}, {0.2, 0.2,
  //         0.2}),
  //     };
  //   } break;
  //   case test_materials_type::materials5: {
  //     materials = {
  //         add_glass_material(scene, "skin1a", {0.76, 0.48, 0.23}, 0.25,
  //             {0.436, 0.227, 0.131}, invalidid, invalidid,
  //             invalidid, 1.5, -0.8, 0.001),
  //         add_glass_material(scene, "skin2a", {0.82, 0.55, 0.4}, 0.25,
  //             {0.623, 0.433, 0.343}, invalidid, invalidid,
  //             invalidid, 1.5, -0.8, 0.001),
  //         add_glass_material(scene, "skins", {0.76, 0.48, 0.23}, 0,
  //             {0.436, 0.227, 0.131}, invalidid, invalidid,
  //             invalidid, 1.5, -0.8, 0.001),
  //         add_glass_material(scene, "skin1b", {0.76, 0.48, 0.23}, 0.25,
  //             {0.436, 0.227, 0.131}, invalidid, invalidid,
  //             invalidid, 1.5, -0.8, 0.001),
  //         add_glass_material(scene, "skin2b", {0.82, 0.55, 0.4}, 0.25,
  //             {0.623, 0.433, 0.343}, invalidid, invalidid,
  //             invalidid, 1.5, -0.8, 0.001),
  //     };
  //   } break;
  // }
  // for (auto idx : range(shapes.size())) {
  //   if (!shapes[idx]) continue;
  //   if (shapes.size() > 5) {
  //     add_instance(scene,
  //         scene.shape_names[idx] + "-" + scene.shape_names[idx % 5],
  //         {{1, 0, 0}, {0, 1, 0}, {0, 0, 1},
  //             {0.2f * (idx % 5 - 2), 0.075, -0.4f * (idx / 5)}},
  //         shapes[idx], materials[idx % 5]);
  //   } else {
  //     auto name = params.instance_name == test_instance_name_type::material
  //                     ? scene.material_names[idx]
  //                     : scene.shape_names[idx];
  //     add_instance(scene, name,
  //         {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.2f * (idx % 5 - 2), 0.075,
  //         0}}, shapes[idx], materials[idx]);
  //   }
  //   if (!shapesi.empty() && shapesi[idx]) {
  //     // TODO(fabio): fix name
  //     add_instance(scene, "",
  //         {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.2f * (idx - 2), 0.075, 0}},
  //         shapesi[idx], materials[idx]);
  //   }
  // }
}

// Scene presets used for testing.
scene_data make_scene_preset(const string& type) {
  if (type == "cornellbox") {
    return make_cornellbox();
  } else if (type == "features1") {
    return make_test({test_cameras_type::standard, test_environments_type::sky,
        test_arealights_type::standard, test_floor_type::standard,
        test_shapes_type::features1, test_materials_type::features1,
        test_instance_name_type::material});
  } else if (type == "features2") {
    return make_test({test_cameras_type::standard, test_environments_type::sky,
        test_arealights_type::standard, test_floor_type::standard,
        test_shapes_type::features2, test_materials_type::features2,
        test_instance_name_type::shape});
  } else if (type == "materials1") {
    return make_test({test_cameras_type::wide, test_environments_type::sky,
        test_arealights_type::large, test_floor_type::standard,
        test_shapes_type::rows, test_materials_type::materials1,
        test_instance_name_type::material});
  } else if (type == "materials2") {
    return make_test({test_cameras_type::wide, test_environments_type::sky,
        test_arealights_type::large, test_floor_type::standard,
        test_shapes_type::rows, test_materials_type::materials2,
        test_instance_name_type::material});
  } else if (type == "materials3") {
    return make_test({test_cameras_type::wide, test_environments_type::sky,
        test_arealights_type::large, test_floor_type::standard,
        test_shapes_type::rows, test_materials_type::materials3,
        test_instance_name_type::material});
  } else if (type == "materials4") {
    return make_test({test_cameras_type::wide, test_environments_type::sky,
        test_arealights_type::large, test_floor_type::standard,
        test_shapes_type::rows, test_materials_type::materials4,
        test_instance_name_type::material});
  } else if (type == "materials5") {
    return make_test({test_cameras_type::wide, test_environments_type::sky,
        test_arealights_type::large, test_floor_type::standard,
        test_shapes_type::rows, test_materials_type::materials5,
        test_instance_name_type::material});
  } else if (type == "shapes1") {
    return make_test({test_cameras_type::standard, test_environments_type::sky,
        test_arealights_type::large, test_floor_type::standard,
        test_shapes_type::shapes1, test_materials_type::uvgrid,
        test_instance_name_type::shape});
  } else if (type == "shapes2") {
    return make_test({test_cameras_type::standard, test_environments_type::sky,
        test_arealights_type::large, test_floor_type::standard,
        test_shapes_type::shapes2, test_materials_type::uvgrid,
        test_instance_name_type::shape});
  } else if (type == "shapes3") {
    return make_test({test_cameras_type::standard, test_environments_type::sky,
        test_arealights_type::large, test_floor_type::standard,
        test_shapes_type::shapes3, test_materials_type::hair,
        test_instance_name_type::shape});
  } else if (type == "environments1") {
    return make_test({test_cameras_type::standard, test_environments_type::sky,
        test_arealights_type::none, test_floor_type::standard,
        test_shapes_type::bunny_sphere, test_materials_type::plastic_metal,
        test_instance_name_type::material});
  } else if (type == "environments2") {
    return make_test({test_cameras_type::standard,
        test_environments_type::sunsky, test_arealights_type::none,
        test_floor_type::standard, test_shapes_type::bunny_sphere,
        test_materials_type::plastic_metal, test_instance_name_type::material});
  } else if (type == "arealights1") {
    return make_test({test_cameras_type::standard, test_environments_type::none,
        test_arealights_type::standard, test_floor_type::standard,
        test_shapes_type::bunny_sphere, test_materials_type::plastic_metal,
        test_instance_name_type::material});
  } else {
    return {};
  }
}

// Scene presets used for testing.
bool make_scene_preset(
    const string& filename, scene_data& scene, string& error) {
  auto type = path_basename(filename);
  if (type == "cornellbox") {
    scene = make_cornellbox();
    return true;
  } else if (type == "features1") {
    scene = make_test({test_cameras_type::standard, test_environments_type::sky,
        test_arealights_type::standard, test_floor_type::standard,
        test_shapes_type::features1, test_materials_type::features1,
        test_instance_name_type::material});
    return true;
  } else if (type == "features2") {
    scene = make_test({test_cameras_type::standard, test_environments_type::sky,
        test_arealights_type::standard, test_floor_type::standard,
        test_shapes_type::features2, test_materials_type::features2,
        test_instance_name_type::shape});
    return true;
  } else if (type == "materials1") {
    scene = make_test({test_cameras_type::wide, test_environments_type::sky,
        test_arealights_type::large, test_floor_type::standard,
        test_shapes_type::rows, test_materials_type::materials1,
        test_instance_name_type::material});
    return true;
  } else if (type == "materials2") {
    scene = make_test({test_cameras_type::wide, test_environments_type::sky,
        test_arealights_type::large, test_floor_type::standard,
        test_shapes_type::rows, test_materials_type::materials2,
        test_instance_name_type::material});
    return true;
  } else if (type == "materials3") {
    scene = make_test({test_cameras_type::wide, test_environments_type::sky,
        test_arealights_type::large, test_floor_type::standard,
        test_shapes_type::rows, test_materials_type::materials3,
        test_instance_name_type::material});
    return true;
  } else if (type == "materials4") {
    scene = make_test({test_cameras_type::wide, test_environments_type::sky,
        test_arealights_type::large, test_floor_type::standard,
        test_shapes_type::rows, test_materials_type::materials4,
        test_instance_name_type::material});
    return true;
  } else if (type == "materials5") {
    scene = make_test({test_cameras_type::wide, test_environments_type::sky,
        test_arealights_type::large, test_floor_type::standard,
        test_shapes_type::rows, test_materials_type::materials5,
        test_instance_name_type::material});
    return true;
  } else if (type == "shapes1") {
    scene = make_test({test_cameras_type::standard, test_environments_type::sky,
        test_arealights_type::large, test_floor_type::standard,
        test_shapes_type::shapes1, test_materials_type::uvgrid,
        test_instance_name_type::shape});
    return true;
  } else if (type == "shapes2") {
    scene = make_test({test_cameras_type::standard, test_environments_type::sky,
        test_arealights_type::large, test_floor_type::standard,
        test_shapes_type::shapes2, test_materials_type::uvgrid,
        test_instance_name_type::shape});
    return true;
  } else if (type == "shapes3") {
    scene = make_test({test_cameras_type::standard, test_environments_type::sky,
        test_arealights_type::large, test_floor_type::standard,
        test_shapes_type::shapes3, test_materials_type::hair,
        test_instance_name_type::shape});
    return true;
  } else if (type == "environments1") {
    scene = make_test({test_cameras_type::standard, test_environments_type::sky,
        test_arealights_type::none, test_floor_type::standard,
        test_shapes_type::bunny_sphere, test_materials_type::plastic_metal,
        test_instance_name_type::material});
    return true;
  } else if (type == "environments2") {
    scene = make_test({test_cameras_type::standard,
        test_environments_type::sunsky, test_arealights_type::none,
        test_floor_type::standard, test_shapes_type::bunny_sphere,
        test_materials_type::plastic_metal, test_instance_name_type::material});
    return true;
  } else if (type == "arealights1") {
    scene = make_test({test_cameras_type::standard,
        test_environments_type::none, test_arealights_type::standard,
        test_floor_type::standard, test_shapes_type::bunny_sphere,
        test_materials_type::plastic_metal, test_instance_name_type::material});
    return true;
  } else {
    error = "unknown preset";
    return false;
  }
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// GENERIC SCENE LOADING
// -----------------------------------------------------------------------------
namespace yocto {

// Load/save a scene in the builtin JSON format.
static scene_data load_json_scene(const string& filename, bool noparallel);
static void       save_json_scene(
          const string& filename, const scene_data& scene, bool noparallel);

// Load/save a scene from/to OBJ.
static scene_data load_obj_scene(const string& filename, bool noparallel);
static void       save_obj_scene(
          const string& filename, const scene_data& scene, bool noparallel);

// Load/save a scene from/to PLY. Loads/saves only one mesh with no other
// data.
static scene_data load_ply_scene(const string& filename, bool noparallel);
static void       save_ply_scene(
          const string& filename, const scene_data& scene, bool noparallel);

// Load/save a scene from/to STL. Loads/saves only one mesh with no other
// data.
static scene_data load_stl_scene(const string& filename, bool noparallel);
static void       save_stl_scene(
          const string& filename, const scene_data& scene, bool noparallel);

// Load/save a scene from/to glTF.
static scene_data load_gltf_scene(const string& filename, bool noparallel);
static void       save_gltf_scene(
          const string& filename, const scene_data& scene, bool noparallel);

// Load/save a scene from/to pbrt. This is not robust at all and only
// works on scene that have been previously adapted since the two renderers
// are too different to match.
static scene_data load_pbrt_scene(const string& filename, bool noparallel);
static void       save_pbrt_scene(
          const string& filename, const scene_data& scene, bool noparallel);

// Load/save a scene from/to mitsuba. This is not robust at all and only
// works on scene that have been previously adapted since the two renderers
// are too different to match. For now, only saving is allowed.
static scene_data load_mitsuba_scene(const string& filename, bool noparallel);
static void       save_mitsuba_scene(
          const string& filename, const scene_data& scene, bool noparallel);

// Load a scene
scene_data load_scene(const string& filename, bool noparallel) {
  auto ext = path_extension(filename);
  if (ext == ".json" || ext == ".JSON") {
    return load_json_scene(filename, noparallel);
  } else if (ext == ".obj" || ext == ".OBJ") {
    return load_obj_scene(filename, noparallel);
  } else if (ext == ".gltf" || ext == ".GLTF") {
    return load_gltf_scene(filename, noparallel);
  } else if (ext == ".pbrt" || ext == ".PBRT") {
    return load_pbrt_scene(filename, noparallel);
  } else if (ext == ".xml" || ext == ".XML") {
    return load_mitsuba_scene(filename, noparallel);
  } else if (ext == ".ply" || ext == ".PLY") {
    return load_ply_scene(filename, noparallel);
  } else if (ext == ".stl" || ext == ".STL") {
    return load_stl_scene(filename, noparallel);
  } else if (ext == ".ypreset" || ext == ".YPRESET") {
    return make_scene_preset(filename);
  } else {
    throw io_error("unsupported format " + filename);
  }
}

// Save a scene
void save_scene(
    const string& filename, const scene_data& scene, bool noparallel) {
  auto ext = path_extension(filename);
  if (ext == ".json" || ext == ".JSON") {
    return save_json_scene(filename, scene, noparallel);
  } else if (ext == ".obj" || ext == ".OBJ") {
    return save_obj_scene(filename, scene, noparallel);
  } else if (ext == ".gltf" || ext == ".GLTF") {
    return save_gltf_scene(filename, scene, noparallel);
  } else if (ext == ".pbrt" || ext == ".PBRT") {
    return save_pbrt_scene(filename, scene, noparallel);
  } else if (ext == ".xml" || ext == ".XML") {
    return save_mitsuba_scene(filename, scene, noparallel);
  } else if (ext == ".ply" || ext == ".PLY") {
    return save_ply_scene(filename, scene, noparallel);
  } else if (ext == ".stl" || ext == ".STL") {
    return save_stl_scene(filename, scene, noparallel);
  } else {
    throw io_error("unsupported format " + filename);
  }
}

// Make missing scene directories
void make_scene_directories(const string& filename, const scene_data& scene) {
  // make a directory if needed
  make_directory(path_dirname(filename));
  if (!scene.shapes.empty())
    make_directory(path_join(path_dirname(filename), "shapes"));
  if (!scene.textures.empty())
    make_directory(path_join(path_dirname(filename), "textures"));
  if (!scene.subdivs.empty())
    make_directory(path_join(path_dirname(filename), "subdivs"));
}

// Add environment
void add_environment(
    scene_data& scene, const string& name, const string& filename) {
  auto texture = load_texture(filename);
  scene.textures.push_back(std::move(texture));
  scene.environments.push_back({{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}},
      {1, 1, 1}, (int)scene.textures.size() - 1});
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// INDIVIDUAL ELEMENTS
// -----------------------------------------------------------------------------
namespace yocto {

// load instances
static void load_instance(const string& filename, vector<frame3f>& frames) {
  auto ext = path_extension(filename);
  if (ext == ".ply" || ext == ".PLY") {
    auto ply = load_ply(filename);
    // TODO: remove when all as arrays
    if (!get_values(ply, "instance",
            {"xx", "xy", "xz", "yx", "yy", "yz", "zx", "zy", "zz", "ox", "oy",
                "oz"},
            (vector<array<float, 12>>&)frames)) {
      throw io_error{"cannot parse " + filename};
    }
  } else {
    throw io_error("unsupported format " + filename);
  }
}

// save instances
[[maybe_unused]] static void save_instance(
    const string& filename, const vector<frame3f>& frames, bool ascii = false) {
  auto ext = path_extension(filename);
  if (ext == ".ply" || ext == ".PLY") {
    auto ply = ply_model{};
    // TODO: remove when all as arrays
    add_values(ply, "instance",
        {"xx", "xy", "xz", "yx", "yy", "yz", "zx", "zy", "zz", "ox", "oy",
            "oz"},
        (const vector<array<float, 12>>&)frames);
    save_ply(filename, ply);
  } else {
    throw io_error("unsupported format " + filename);
  }
}

// load subdiv
subdiv_data load_subdiv(const string& filename) {
  auto lsubdiv         = load_fvshape(filename);
  auto subdiv          = subdiv_data{};
  subdiv.quadspos      = lsubdiv.quadspos;
  subdiv.quadsnorm     = lsubdiv.quadsnorm;
  subdiv.quadstexcoord = lsubdiv.quadstexcoord;
  subdiv.positions     = lsubdiv.positions;
  subdiv.normals       = lsubdiv.normals;
  subdiv.texcoords     = lsubdiv.texcoords;
  return subdiv;
}

// save subdiv
void save_subdiv(const string& filename, const subdiv_data& subdiv) {
  auto ssubdiv          = fvshape_data{};
  ssubdiv.quadspos      = subdiv.quadspos;
  ssubdiv.quadsnorm     = subdiv.quadsnorm;
  ssubdiv.quadstexcoord = subdiv.quadstexcoord;
  ssubdiv.positions     = subdiv.positions;
  ssubdiv.normals       = subdiv.normals;
  ssubdiv.texcoords     = subdiv.texcoords;
  save_fvshape(filename, ssubdiv);
}

// save binary shape
static void save_binshape(const string& filename, const shape_data& shape) {
  auto write_values = [](vector<byte>& buffer, const auto& values) {
    if (values.empty()) return;
    buffer.insert(buffer.end(), (byte*)values.data(),
        (byte*)values.data() + values.size() * sizeof(values.front()));
  };

  auto buffer = vector<byte>{};

  write_values(buffer, shape.positions);
  write_values(buffer, shape.normals);
  write_values(buffer, shape.texcoords);
  write_values(buffer, shape.colors);
  write_values(buffer, shape.radius);
  write_values(buffer, shape.points);
  write_values(buffer, shape.lines);
  write_values(buffer, shape.triangles);
  write_values(buffer, quads_to_triangles(shape.quads));

  save_binary(filename, buffer);
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// JSON IO
// -----------------------------------------------------------------------------
namespace yocto {

// Material type
enum struct material_type40 {
  // clang-format off
  matte, glossy, metallic, transparent, refractive, subsurface, volume, gltfpbr
  // clang-format on
};

// Enum labels
static const auto material_type40_names = std::vector<std::string>{"matte",
    "glossy", "metallic", "transparent", "refractive", "subsurface", "volume",
    "gltfpbr"};

NLOHMANN_JSON_SERIALIZE_ENUM(
    material_type40, {
                         {material_type40::matte, "matte"},
                         {material_type40::glossy, "glossy"},
                         {material_type40::metallic, "metallic"},
                         {material_type40::transparent, "transparent"},
                         {material_type40::refractive, "refractive"},
                         {material_type40::subsurface, "subsurface"},
                         {material_type40::volume, "volume"},
                         {material_type40::gltfpbr, "gltfpbr"},
                     })
NLOHMANN_JSON_SERIALIZE_ENUM(
    material_type, {
                       {material_type::matte, "matte"},
                       {material_type::glossy, "glossy"},
                       {material_type::reflective, "reflective"},
                       {material_type::transparent, "transparent"},
                       {material_type::refractive, "refractive"},
                       {material_type::subsurface, "subsurface"},
                       {material_type::volumetric, "volumetric"},
                       {material_type::gltfpbr, "gltfpbr"},
                   })

// Load a scene in the builtin JSON format.
static scene_data load_json_scene_version40(
    const string& filename, const json_value& json, bool noparallel) {
  // parse json value
  auto get_opt = [](const json_value& json, const string& key, auto& value) {
    value = json.value(key, value);
  };
  auto get_of3 = [](const json_value& json, const string& key, auto& value) {
    auto valuea = json.value(key, (array<float, 12>&)value);
    value       = *(frame3f*)&valuea;
  };
  auto get_om3 = [](const json_value& json, const string& key, auto& value) {
    auto valuea = json.value(key, (array<float, 9>&)value);
    value       = *(mat3f*)&valuea;
  };

  // scene
  auto scene = scene_data{};

  // parse json reference
  auto shape_map = unordered_map<string, int>{};
  auto get_shp   = [&scene, &shape_map](
                     const json_value& json, const string& key, int& value) {
    auto name = json.value(key, string{});
    if (name.empty()) return;
    auto it = shape_map.find(name);
    if (it != shape_map.end()) {
      value = it->second;
    } else {
      scene.shape_names.emplace_back(name);
      scene.shapes.emplace_back();
      auto shape_id   = (int)scene.shapes.size() - 1;
      shape_map[name] = shape_id;
      value           = shape_id;
    }
  };

  // parse json reference
  auto material_map = unordered_map<string, int>{};
  auto get_mat      = [&material_map](
                     const json_value& json, const string& key, int& value) {
    auto name = json.value(key, string{});
    if (name.empty()) return;
    auto it = material_map.find(name);
    if (it != material_map.end()) {
      value = it->second;
    } else {
      throw std::out_of_range{"missing key"};
    }
  };

  // parse json reference
  auto texture_map = unordered_map<string, int>{};
  auto get_tex     = [&scene, &texture_map](
                     const json_value& json, const string& key, int& value) {
    auto name = json.value(key, string{});
    if (name.empty()) return;
    auto it = texture_map.find(name);
    if (it != texture_map.end()) {
      value = it->second;
    } else {
      scene.texture_names.emplace_back(name);
      scene.textures.emplace_back();
      auto texture_id   = (int)scene.textures.size() - 1;
      texture_map[name] = texture_id;
      value             = texture_id;
    }
  };

  // load json instance
  struct ply_instance {
    vector<frame3f> frames = {};
  };
  using ply_instance_handle = int;
  auto ply_instances        = vector<ply_instance>{};
  auto ply_instances_names  = vector<string>{};
  auto ply_instance_map     = unordered_map<string, ply_instance_handle>{
      {"", invalidid}};
  auto instance_ply = unordered_map<int, ply_instance_handle>{};
  auto get_ist      = [&scene, &ply_instances, &ply_instances_names,
                     &ply_instance_map, &instance_ply](const json_value& json,
                     const string& key, const instance_data& instance) {
    auto name = json.value(key, string{});
    if (name.empty()) return;
    auto instance_id = (int)(&instance - scene.instances.data());
    auto it          = ply_instance_map.find(name);
    if (it != ply_instance_map.end()) {
      instance_ply[instance_id] = it->second;
    } else {
      ply_instances_names.emplace_back(name);
      ply_instances.emplace_back(ply_instance());
      auto ply_instance_id      = (int)ply_instances.size() - 1;
      ply_instance_map[name]    = ply_instance_id;
      instance_ply[instance_id] = ply_instance_id;
    }
  };
  auto get_ply_instance_name = [&ply_instances, &ply_instances_names](
                                   const scene_data&   scene,
                                   const ply_instance& instance) -> string {
    return ply_instances_names[&instance - ply_instances.data()];
  };

  // parsing values
  try {
    if (json.contains("asset")) {
      auto& element = json.at("asset");
      get_opt(element, "copyright", scene.copyright);
    }
    if (json.contains("cameras")) {
      for (auto& [key, element] : json.at("cameras").items()) {
        auto& camera = scene.cameras.emplace_back();
        scene.camera_names.emplace_back(key);
        get_of3(element, "frame", camera.frame);
        get_opt(element, "orthographic", camera.orthographic);
        get_opt(element, "ortho", camera.orthographic);
        get_opt(element, "lens", camera.lens);
        get_opt(element, "aspect", camera.aspect);
        get_opt(element, "film", camera.film);
        get_opt(element, "focus", camera.focus);
        get_opt(element, "aperture", camera.aperture);
        if (element.contains("lookat")) {
          get_om3(element, "lookat", (mat3f&)camera.frame);
          camera.focus = length(camera.frame.x - camera.frame.y);
          camera.frame = lookat_frame(
              camera.frame.x, camera.frame.y, camera.frame.z);
        }
      }
    }
    if (json.contains("environments")) {
      for (auto& [key, element] : json.at("environments").items()) {
        auto& environment = scene.environments.emplace_back();
        scene.environment_names.emplace_back(key);
        get_of3(element, "frame", environment.frame);
        get_opt(element, "emission", environment.emission);
        get_tex(element, "emission_tex", environment.emission_tex);
        if (element.contains("lookat")) {
          get_om3(element, "lookat", (mat3f&)environment.frame);
          environment.frame = lookat_frame(environment.frame.x,
              environment.frame.y, environment.frame.z, false);
        }
      }
    }
    if (json.contains("materials")) {
      for (auto& [key, element] : json.at("materials").items()) {
        auto& material = scene.materials.emplace_back();
        scene.material_names.emplace_back(key);
        material_map[key] = (int)scene.materials.size() - 1;
        auto type40       = material_type40::matte;
        get_opt(element, "type", type40);
        material.type = (material_type)type40;
        get_opt(element, "emission", material.emission);
        get_opt(element, "color", material.color);
        get_opt(element, "metallic", material.metallic);
        get_opt(element, "roughness", material.roughness);
        get_opt(element, "ior", material.ior);
        get_opt(element, "trdepth", material.trdepth);
        get_opt(element, "scattering", material.scattering);
        get_opt(element, "scanisotropy", material.scanisotropy);
        get_opt(element, "opacity", material.opacity);
        get_tex(element, "emission_tex", material.emission_tex);
        get_tex(element, "color_tex", material.color_tex);
        get_tex(element, "roughness_tex", material.roughness_tex);
        get_tex(element, "scattering_tex", material.scattering_tex);
        get_tex(element, "normal_tex", material.normal_tex);
      }
    }
    if (json.contains("instances")) {
      for (auto& [key, element] : json.at("instances").items()) {
        auto& instance = scene.instances.emplace_back();
        scene.instance_names.emplace_back(key);
        get_of3(element, "frame", instance.frame);
        get_shp(element, "shape", instance.shape);
        get_mat(element, "material", instance.material);
        if (element.contains("lookat")) {
          get_om3(element, "lookat", (mat3f&)instance.frame);
          instance.frame = lookat_frame(
              instance.frame.x, instance.frame.y, instance.frame.z, false);
        }
      }
    }
    if (json.contains("objects")) {
      for (auto& [key, element] : json.at("objects").items()) {
        auto& instance = scene.instances.emplace_back();
        scene.instance_names.emplace_back(key);
        get_of3(element, "frame", instance.frame);
        get_shp(element, "shape", instance.shape);
        get_mat(element, "material", instance.material);
        if (element.contains("lookat")) {
          get_om3(element, "lookat", (mat3f&)instance.frame);
          instance.frame = lookat_frame(
              instance.frame.x, instance.frame.y, instance.frame.z, false);
        }
        if (element.contains("instance")) {
          get_ist(element, "instance", instance);
        }
      }
    }
    if (json.contains("subdivs")) {
      for (auto& [key, element] : json.at("subdivs").items()) {
        auto& subdiv = scene.subdivs.emplace_back();
        scene.subdiv_names.emplace_back(key);
        get_shp(element, "shape", subdiv.shape);
        get_opt(element, "subdivisions", subdiv.subdivisions);
        get_opt(element, "catmullclark", subdiv.catmullclark);
        get_opt(element, "smooth", subdiv.smooth);
        get_opt(element, "displacement", subdiv.displacement);
        get_tex(element, "displacement_tex", subdiv.displacement_tex);
      }
    }
  } catch (...) {
    throw io_error("cannot parse " + filename);
  }

  // dirname
  auto dirname = path_dirname(filename);

  // get filename from name
  auto find_path = [dirname](const string& name, const string& group,
                       const vector<string>& extensions) {
    for (auto& extension : extensions) {
      auto path = path_join(dirname, group, name + extension);
      if (path_exists(path)) return path_join(group, name + extension);
    }
    return path_join(group, name + extensions.front());
  };

  // load resources
  try {
    // load shapes
    parallel_foreach(scene.shapes, noparallel, [&](auto& shape) {
      auto path = find_path(
          get_shape_name(scene, shape), "shapes", {".ply", ".obj"});
      shape = load_shape(path_join(dirname, path));
    });
    // load subdivs
    parallel_foreach(scene.subdivs, noparallel, [&](auto& subdiv) {
      auto path = find_path(
          get_subdiv_name(scene, subdiv), "subdivs", {".ply", ".obj"});
      subdiv = load_subdiv(path_join(dirname, path));
    });
    // load textures
    parallel_foreach(scene.textures, noparallel, [&](auto& texture) {
      auto path = find_path(get_texture_name(scene, texture), "textures",
          {".hdr", ".exr", ".png", ".jpg"});
      texture   = load_texture(path_join(dirname, path));
    });
    // load instances
    parallel_foreach(ply_instances, noparallel, [&](auto& ply_instance) {
      auto path = find_path(
          get_ply_instance_name(scene, ply_instance), "instances", {".ply"});
      return load_instance(path_join(dirname, path), ply_instance.frames);
    });
  } catch (std::exception& except) {
    throw io_error(
        "cannot load " + filename + " since " + string(except.what()));
  }

  // apply instances
  if (!ply_instances.empty()) {
    auto instances      = scene.instances;
    auto instance_names = scene.instance_names;
    scene.instances.clear();
    scene.instance_names.clear();
    for (auto& instance : instances) {
      auto it = instance_ply.find((int)(&instance - instances.data()));
      if (it == instance_ply.end()) {
        auto& ninstance = scene.instances.emplace_back();
        scene.instance_names.emplace_back(
            instance_names[&instance - instances.data()]);
        ninstance.frame    = instance.frame;
        ninstance.shape    = instance.shape;
        ninstance.material = instance.material;
      } else {
        auto& ply_instance = ply_instances[it->second];
        auto  instance_id  = 0;
        for (auto& frame : ply_instance.frames) {
          auto& ninstance = scene.instances.emplace_back();
          scene.instance_names.emplace_back(
              instance_names[&instance - instances.data()] + "_" +
              std::to_string(instance_id++));
          ninstance.frame    = frame * instance.frame;
          ninstance.shape    = instance.shape;
          ninstance.material = instance.material;
        }
      }
    }
  }

  // fix scene
  add_missing_camera(scene);
  add_missing_radius(scene);
  trim_memory(scene);

  // done
  return scene;
}

// Load a scene in the builtin JSON format.
static scene_data load_json_scene_version41(
    const string& filename, json_value& json, bool noparallel) {
  // check version
  if (!json.contains("asset") || !json.at("asset").contains("version"))
    return load_json_scene_version40(filename, json, noparallel);

  // parse json value
  auto get_opt = [](const json_value& json, const string& key, auto& value) {
    value = json.value(key, value);
  };
  auto get_ref = [](const json_value& json, const string& key, int& value,
                     const unordered_map<string, int>& map) {
    auto values = json.value(key, string{});
    value       = values.empty() ? -1 : map.at(values);
  };

  // references
  auto shape_map    = unordered_map<string, int>{};
  auto texture_map  = unordered_map<string, int>{};
  auto material_map = unordered_map<string, int>{};

  // filenames
  auto shape_filenames   = vector<string>{};
  auto texture_filenames = vector<string>{};
  auto subdiv_filenames  = vector<string>{};

  // scene
  auto scene = scene_data{};

  // parsing values
  try {
    if (json.contains("asset")) {
      auto& element = json.at("asset");
      get_opt(element, "copyright", scene.copyright);
    }
    if (json.contains("cameras")) {
      auto& group = json.at("cameras");
      scene.cameras.reserve(group.size());
      scene.camera_names.reserve(group.size());
      for (auto& [key, element] : group.items()) {
        auto& camera = scene.cameras.emplace_back();
        scene.camera_names.push_back(key);
        get_opt(element, "frame", camera.frame);
        get_opt(element, "orthographic", camera.orthographic);
        get_opt(element, "ortho", camera.orthographic);
        get_opt(element, "lens", camera.lens);
        get_opt(element, "aspect", camera.aspect);
        get_opt(element, "film", camera.film);
        get_opt(element, "focus", camera.focus);
        get_opt(element, "aperture", camera.aperture);
        if (element.contains("lookat")) {
          auto lookat = mat3f{};
          get_opt(element, "lookat", lookat);
          auto from = lookat[0], to = lookat[1], up = lookat[2];
          camera.focus = length(from - to);
          camera.frame = lookat_frame(from, to, up);
        }
      }
    }
    if (json.contains("textures")) {
      auto& group = json.at("textures");
      scene.textures.reserve(group.size());
      scene.texture_names.reserve(group.size());
      texture_filenames.reserve(group.size());
      for (auto& [key, element] : group.items()) {
        [[maybe_unused]] auto& texture = scene.textures.emplace_back();
        scene.texture_names.push_back(key);
        auto& datafile   = texture_filenames.emplace_back();
        texture_map[key] = (int)scene.textures.size() - 1;
        if (element.is_string()) {
          auto filename       = element.get<string>();
          element             = json_value::object();
          element["datafile"] = filename;
        }
        get_opt(element, "datafile", datafile);
      }
    }
    if (json.contains("materials")) {
      auto& group = json.at("materials");
      scene.materials.reserve(group.size());
      scene.material_names.reserve(group.size());
      for (auto& [key, element] : json.at("materials").items()) {
        auto& material = scene.materials.emplace_back();
        scene.material_names.push_back(key);
        material_map[key] = (int)scene.materials.size() - 1;
        get_opt(element, "type", material.type);
        get_opt(element, "emission", material.emission);
        get_opt(element, "color", material.color);
        get_opt(element, "metallic", material.metallic);
        get_opt(element, "roughness", material.roughness);
        get_opt(element, "ior", material.ior);
        get_opt(element, "trdepth", material.trdepth);
        get_opt(element, "scattering", material.scattering);
        get_opt(element, "scanisotropy", material.scanisotropy);
        get_opt(element, "opacity", material.opacity);
        get_ref(element, "emission_tex", material.emission_tex, texture_map);
        get_ref(element, "color_tex", material.color_tex, texture_map);
        get_ref(element, "roughness_tex", material.roughness_tex, texture_map);
        get_ref(
            element, "scattering_tex", material.scattering_tex, texture_map);
        get_ref(element, "normal_tex", material.normal_tex, texture_map);
      }
    }
    if (json.contains("shapes")) {
      auto& group = json.at("shapes");
      scene.shapes.reserve(group.size());
      scene.shape_names.reserve(group.size());
      shape_filenames.reserve(group.size());
      for (auto& [key, element] : group.items()) {
        [[maybe_unused]] auto& shape = scene.shapes.emplace_back();
        scene.shape_names.push_back(key);
        auto& datafile = shape_filenames.emplace_back();
        shape_map[key] = (int)scene.shapes.size() - 1;
        if (element.is_string()) {
          auto filename       = element.get<string>();
          element             = json_value::object();
          element["datafile"] = filename;
        }
        get_opt(element, "datafile", datafile);
      }
    }
    if (json.contains("subdivs")) {
      auto& group = json.at("subdivs");
      scene.subdivs.reserve(group.size());
      scene.subdiv_names.reserve(group.size());
      subdiv_filenames.reserve(group.size());
      for (auto& [key, element] : group.items()) {
        auto& subdiv = scene.subdivs.emplace_back();
        scene.subdiv_names.emplace_back(key);
        auto& datafile = subdiv_filenames.emplace_back();
        get_opt(element, "datafile", datafile);
        get_ref(element, "shape", subdiv.shape, shape_map);
        get_opt(element, "subdivisions", subdiv.subdivisions);
        get_opt(element, "catmullclark", subdiv.catmullclark);
        get_opt(element, "smooth", subdiv.smooth);
        get_opt(element, "displacement", subdiv.displacement);
        get_ref(
            element, "displacement_tex", subdiv.displacement_tex, texture_map);
      }
    }
    if (json.contains("instances")) {
      auto& group = json.at("instances");
      scene.instances.reserve(group.size());
      scene.instance_names.reserve(group.size());
      for (auto& [key, element] : group.items()) {
        auto& instance = scene.instances.emplace_back();
        scene.instance_names.emplace_back(key);
        get_opt(element, "frame", instance.frame);
        get_ref(element, "shape", instance.shape, shape_map);
        get_ref(element, "material", instance.material, material_map);
        if (element.contains("lookat")) {
          auto lookat = mat3f{};
          get_opt(element, "lookat", lookat);
          auto from = lookat[0], to = lookat[1], up = lookat[2];
          instance.frame = lookat_frame(from, to, up, false);
        }
      }
    }
    if (json.contains("environments")) {
      auto& group = json.at("environments");
      scene.instances.reserve(group.size());
      scene.instance_names.reserve(group.size());
      for (auto& [key, element] : group.items()) {
        auto& environment = scene.environments.emplace_back();
        scene.environment_names.push_back(key);
        get_opt(element, "frame", environment.frame);
        get_opt(element, "emission", environment.emission);
        get_ref(element, "emission_tex", environment.emission_tex, texture_map);
        if (element.contains("lookat")) {
          auto lookat = mat3f{};
          get_opt(element, "lookat", lookat);
          auto from = lookat[0], to = lookat[1], up = lookat[2];
          environment.frame = lookat_frame(from, to, up, false);
        }
      }
    }
  } catch (...) {
    throw io_error("cannot parse " + filename);
  }

  // prepare data
  auto dirname = path_dirname(filename);

  // fix paths
  for (auto& datafile : shape_filenames)
    datafile = path_join(dirname, "shapes", datafile);
  for (auto& datafile : texture_filenames)
    datafile = path_join(dirname, "textures", datafile);
  for (auto& datafile : subdiv_filenames)
    datafile = path_join(dirname, "subdivs", datafile);

  // load resources
  try {
    // load shapes
    parallel_zip(shape_filenames, scene.shapes, noparallel,
        [&](auto&& filename, auto&& shape) { shape = load_shape(filename); });
    // load subdivs
    parallel_zip(subdiv_filenames, scene.subdivs, noparallel,
        [&](auto&& filename, auto&& subdiv) {
          subdiv = load_subdiv(filename);
        });
    // load textures
    parallel_zip(texture_filenames, scene.textures, noparallel,
        [&](auto&& filename, auto&& texture) {
          texture = load_texture(filename);
        });
  } catch (std::exception& except) {
    throw io_error(
        "cannot load " + filename + " since " + string(except.what()));
  }

  // fix scene
  add_missing_camera(scene);
  add_missing_radius(scene);
  trim_memory(scene);

  // done
  return scene;
}

// Load a scene in the builtin JSON format.
static scene_data load_json_scene(const string& filename, bool noparallel) {
  // open file
  auto json = load_json(filename);

  // check version
  if (!json.contains("asset") || !json.at("asset").contains("version"))
    return load_json_scene_version40(filename, json, noparallel);
  if (json.contains("asset") && json.at("asset").contains("version") &&
      json.at("asset").at("version") == "4.1")
    return load_json_scene_version41(filename, json, noparallel);

  // parse json value
  auto get_opt = [](const json_value& json, const string& key, auto& value) {
    value = json.value(key, value);
  };

  // filenames
  auto shape_filenames   = vector<string>{};
  auto texture_filenames = vector<string>{};
  auto subdiv_filenames  = vector<string>{};

  // scene
  auto scene = scene_data{};

  // parsing values
  try {
    if (json.contains("asset")) {
      auto& element = json.at("asset");
      get_opt(element, "copyright", scene.copyright);
      auto version = string{};
      get_opt(element, "version", version);
      if (version != "4.2" && version != "5.0")
        throw io_error("unsupported format version " + filename);
    }
    if (json.contains("cameras")) {
      auto& group = json.at("cameras");
      scene.cameras.reserve(group.size());
      scene.camera_names.reserve(group.size());
      for (auto& element : group) {
        auto& camera = scene.cameras.emplace_back();
        auto& name   = scene.camera_names.emplace_back();
        get_opt(element, "name", name);
        get_opt(element, "frame", camera.frame);
        get_opt(element, "orthographic", camera.orthographic);
        get_opt(element, "lens", camera.lens);
        get_opt(element, "aspect", camera.aspect);
        get_opt(element, "film", camera.film);
        get_opt(element, "focus", camera.focus);
        get_opt(element, "aperture", camera.aperture);
        if (element.contains("lookat")) {
          get_opt(element, "lookat", (mat3f&)camera.frame);
          camera.focus = length(camera.frame.x - camera.frame.y);
          camera.frame = lookat_frame(
              camera.frame.x, camera.frame.y, camera.frame.z);
        }
      }
    }
    if (json.contains("textures")) {
      auto& group = json.at("textures");
      scene.textures.reserve(group.size());
      scene.texture_names.reserve(group.size());
      texture_filenames.reserve(group.size());
      for (auto& element : group) {
        [[maybe_unused]] auto& texture = scene.textures.emplace_back();
        auto&                  name    = scene.texture_names.emplace_back();
        auto&                  uri     = texture_filenames.emplace_back();
        get_opt(element, "name", name);
        get_opt(element, "uri", uri);
      }
    }
    if (json.contains("materials")) {
      auto& group = json.at("materials");
      scene.materials.reserve(group.size());
      scene.material_names.reserve(group.size());
      for (auto& element : json.at("materials")) {
        auto& material = scene.materials.emplace_back();
        auto& name     = scene.material_names.emplace_back();
        get_opt(element, "name", name);
        get_opt(element, "type", material.type);
        get_opt(element, "emission", material.emission);
        get_opt(element, "color", material.color);
        get_opt(element, "metallic", material.metallic);
        get_opt(element, "roughness", material.roughness);
        get_opt(element, "ior", material.ior);
        get_opt(element, "trdepth", material.trdepth);
        get_opt(element, "scattering", material.scattering);
        get_opt(element, "scanisotropy", material.scanisotropy);
        get_opt(element, "opacity", material.opacity);
        get_opt(element, "emission_tex", material.emission_tex);
        get_opt(element, "color_tex", material.color_tex);
        get_opt(element, "roughness_tex", material.roughness_tex);
        get_opt(element, "scattering_tex", material.scattering_tex);
        get_opt(element, "normal_tex", material.normal_tex);
      }
    }
    if (json.contains("shapes")) {
      auto& group = json.at("shapes");
      scene.shapes.reserve(group.size());
      scene.shape_names.reserve(group.size());
      shape_filenames.reserve(group.size());
      for (auto& element : group) {
        [[maybe_unused]] auto& shape = scene.shapes.emplace_back();
        auto&                  name  = scene.shape_names.emplace_back();
        auto&                  uri   = shape_filenames.emplace_back();
        get_opt(element, "name", name);
        get_opt(element, "uri", uri);
      }
    }
    if (json.contains("subdivs")) {
      auto& group = json.at("subdivs");
      scene.subdivs.reserve(group.size());
      scene.subdiv_names.reserve(group.size());
      subdiv_filenames.reserve(group.size());
      for (auto& element : group) {
        auto& subdiv = scene.subdivs.emplace_back();
        auto& name   = scene.subdiv_names.emplace_back();
        auto& uri    = subdiv_filenames.emplace_back();
        get_opt(element, "name", name);
        get_opt(element, "uri", uri);
        get_opt(element, "shape", subdiv.shape);
        get_opt(element, "subdivisions", subdiv.subdivisions);
        get_opt(element, "catmullclark", subdiv.catmullclark);
        get_opt(element, "smooth", subdiv.smooth);
        get_opt(element, "displacement", subdiv.displacement);
        get_opt(element, "displacement_tex", subdiv.displacement_tex);
      }
    }
    if (json.contains("instances")) {
      auto& group = json.at("instances");
      scene.instances.reserve(group.size());
      scene.instance_names.reserve(group.size());
      for (auto& element : group) {
        auto& instance = scene.instances.emplace_back();
        auto& name     = scene.instance_names.emplace_back();
        get_opt(element, "name", name);
        get_opt(element, "frame", instance.frame);
        get_opt(element, "shape", instance.shape);
        get_opt(element, "material", instance.material);
        if (element.contains("lookat")) {
          get_opt(element, "lookat", (mat3f&)instance.frame);
          instance.frame = lookat_frame(
              instance.frame.x, instance.frame.y, instance.frame.z, true);
        }
      }
    }
    if (json.contains("environments")) {
      auto& group = json.at("environments");
      scene.instances.reserve(group.size());
      scene.instance_names.reserve(group.size());
      for (auto& element : group) {
        auto& environment = scene.environments.emplace_back();
        auto& name        = scene.environment_names.emplace_back();
        get_opt(element, "name", name);
        get_opt(element, "frame", environment.frame);
        get_opt(element, "emission", environment.emission);
        get_opt(element, "emission_tex", environment.emission_tex);
        if (element.contains("lookat")) {
          get_opt(element, "lookat", (mat3f&)environment.frame);
          environment.frame = lookat_frame(environment.frame.x,
              environment.frame.y, environment.frame.z, true);
        }
      }
    }
  } catch (...) {
    throw io_error("cannot parse " + filename);
  }

  // prepare data
  auto dirname = path_dirname(filename);

  // load resources
  try {
    // load shapes
    parallel_zip(shape_filenames, scene.shapes, noparallel,
        [&](auto&& filename, auto&& shape) {
          shape = load_shape(path_join(dirname, filename));
        });
    // load subdivs
    parallel_zip(subdiv_filenames, scene.subdivs, noparallel,
        [&](auto&& filename, auto&& subdiv) {
          subdiv = load_subdiv(path_join(dirname, filename));
        });
    // load textures
    parallel_zip(texture_filenames, scene.textures, noparallel,
        [&](auto&& filename, auto&& texture) {
          texture = load_texture(path_join(dirname, filename));
        });
  } catch (std::exception& except) {
    throw io_error(
        "cannot load " + filename + " since " + string(except.what()));
  }

  // fix scene
  add_missing_camera(scene);
  add_missing_radius(scene);
  trim_memory(scene);

  // done
  return scene;
}

// Save a scene in the builtin JSON format.
static void save_json_scene(
    const string& filename, const scene_data& scene, bool noparallel) {
  // helpers to handel old code paths
  auto add_object = [](json_value& json, const string& name) -> json_value& {
    auto& item = json[name];
    item       = json_value::object();
    return item;
  };
  auto add_array = [](json_value& json, const string& name) -> json_value& {
    auto& item = json[name];
    item       = json_value::array();
    return item;
  };
  auto append_object = [](json_value& json) -> json_value& {
    auto& item = json.emplace_back();
    item       = json_value::object();
    return item;
  };
  auto set_val = [](json_value& json, const string& name, const auto& value,
                     const auto& def) {
    if (value == def) return;
    json[name] = value;
  };
  auto set_ref = [](json_value& json, const string& name, int value) {
    if (value < 0) return;
    json[name] = value;
  };
  auto reserve_values = [](json_value& json, size_t size) {
    json.get_ptr<json_value::array_t*>()->reserve(size);
  };

  // names
  auto get_name = [](const vector<string>& names, size_t idx) -> string {
    return (idx < names.size()) ? names[idx] : "";
  };
  auto get_filename = [](const vector<string>& names, size_t idx,
                          const string& basename,
                          const string& extension) -> string {
    if (idx < names.size()) {
      return basename + "s/" + names[idx] + extension;
    } else {
      return basename + "s/" + basename + std::to_string(idx) + extension;
    }
  };

  // filenames
  auto shape_filenames   = vector<string>(scene.shapes.size());
  auto texture_filenames = vector<string>(scene.textures.size());
  auto subdiv_filenames  = vector<string>(scene.subdivs.size());
  for (auto idx : range(shape_filenames.size())) {
    shape_filenames[idx] = get_filename(
        scene.shape_names, idx, "shape", ".ply");
  }
  for (auto idx : range(texture_filenames.size())) {
    texture_filenames[idx] = get_filename(scene.texture_names, idx, "texture",
        (scene.textures[idx].pixelsf.empty() ? ".png" : ".hdr"));
  }
  for (auto idx : range(subdiv_filenames.size())) {
    subdiv_filenames[idx] = get_filename(
        scene.subdiv_names, idx, "subdiv", ".obj");
  }

  // save json file
  auto json = json_value::object();

  // asset
  {
    auto& element = add_object(json, "asset");
    set_val(element, "copyright", scene.copyright, "");
    set_val(element, "generator",
        "Yocto/GL - https://github.com/xelatihy/yocto-gl"s, ""s);
    set_val(element, "version", "4.2"s, ""s);
  }

  if (!scene.cameras.empty()) {
    auto  default_ = camera_data{};
    auto& group    = add_array(json, "cameras");
    reserve_values(group, scene.cameras.size());
    for (auto&& [idx, camera] : enumerate(scene.cameras)) {
      auto& element = append_object(group);
      set_val(element, "name", get_name(scene.camera_names, idx), "");
      set_val(element, "frame", camera.frame, default_.frame);
      set_val(
          element, "orthographic", camera.orthographic, default_.orthographic);
      set_val(element, "lens", camera.lens, default_.lens);
      set_val(element, "aspect", camera.aspect, default_.aspect);
      set_val(element, "film", camera.film, default_.film);
      set_val(element, "focus", camera.focus, default_.focus);
      set_val(element, "aperture", camera.aperture, default_.aperture);
    }
  }

  if (!scene.textures.empty()) {
    auto& group = add_array(json, "textures");
    reserve_values(group, scene.textures.size());
    for (auto&& [idx, texture] : enumerate(scene.textures)) {
      auto& element = append_object(group);
      set_val(element, "name", get_name(scene.texture_names, idx), "");
      set_val(element, "uri", texture_filenames[idx], ""s);
    }
  }

  if (!scene.materials.empty()) {
    auto  default_ = material_data{};
    auto& group    = add_array(json, "materials");
    reserve_values(group, scene.materials.size());
    for (auto&& [idx, material] : enumerate(scene.materials)) {
      auto& element = append_object(group);
      set_val(element, "name", get_name(scene.material_names, idx), "");
      set_val(element, "type", material.type, default_.type);
      set_val(element, "emission", material.emission, default_.emission);
      set_val(element, "color", material.color, default_.color);
      set_val(element, "metallic", material.metallic, default_.metallic);
      set_val(element, "roughness", material.roughness, default_.roughness);
      set_val(element, "ior", material.ior, default_.ior);
      set_val(element, "trdepth", material.trdepth, default_.trdepth);
      set_val(element, "scattering", material.scattering, default_.scattering);
      set_val(element, "scanisotropy", material.scanisotropy,
          default_.scanisotropy);
      set_val(element, "opacity", material.opacity, default_.opacity);
      set_val(element, "emission_tex", material.emission_tex,
          default_.emission_tex);
      set_val(element, "color_tex", material.color_tex, default_.color_tex);
      set_val(element, "roughness_tex", material.roughness_tex,
          default_.roughness_tex);
      set_val(element, "scattering_tex", material.scattering_tex,
          default_.scattering_tex);
      set_val(element, "normal_tex", material.normal_tex, default_.normal_tex);
    }
  }

  if (!scene.shapes.empty()) {
    auto& group = add_array(json, "shapes");
    reserve_values(group, scene.shapes.size());
    for (auto&& [idx, shape] : enumerate(scene.shapes)) {
      auto& element = append_object(group);
      set_val(element, "name", get_name(scene.shape_names, idx), "");
      set_val(element, "uri", shape_filenames[idx], "");
    }
  }

  if (!scene.subdivs.empty()) {
    auto  default_ = subdiv_data{};
    auto& group    = add_array(json, "subdivs");
    reserve_values(group, scene.subdivs.size());
    for (auto&& [idx, subdiv] : enumerate(scene.subdivs)) {
      auto& element = append_object(group);
      set_val(element, "name", get_name(scene.subdiv_names, idx), "");
      set_ref(element, "shape", subdiv.shape);
      set_val(element, "uri", subdiv_filenames[idx], "");
      set_val(
          element, "subdivisions", subdiv.subdivisions, default_.subdivisions);
      set_val(
          element, "catmullclark", subdiv.catmullclark, default_.catmullclark);
      set_val(element, "smooth", subdiv.smooth, default_.smooth);
      set_val(
          element, "displacement", subdiv.displacement, default_.displacement);
      set_val(element, "displacement_tex",
          get_texture_name(scene, subdiv.displacement_tex), "");
    }
  }

  if (!scene.instances.empty()) {
    auto  default_ = instance_data{};
    auto& group    = add_array(json, "instances");
    reserve_values(group, scene.instances.size());
    for (auto&& [idx, instance] : enumerate(scene.instances)) {
      auto& element = append_object(group);
      set_val(element, "name", get_name(scene.instance_names, idx), "");
      set_val(element, "frame", instance.frame, default_.frame);
      set_val(element, "shape", instance.shape, default_.shape);
      set_val(element, "material", instance.material, default_.material);
    }
  }

  if (!scene.environments.empty()) {
    auto  default_ = environment_data{};
    auto& group    = add_array(json, "environments");
    reserve_values(group, scene.environments.size());
    for (auto&& [idx, environment] : enumerate(scene.environments)) {
      auto& element = append_object(group);
      set_val(element, "name", get_name(scene.environment_names, idx), "");
      set_val(element, "frame", environment.frame, default_.frame);
      set_val(element, "emission", environment.emission, default_.emission);
      set_val(element, "emission_tex", environment.emission_tex,
          default_.emission_tex);
    }
  }

  // save json
  save_json(filename, json);

  // prepare data
  auto dirname = path_dirname(filename);

  // dirname
  try {
    // save shapes
    parallel_zip(shape_filenames, scene.shapes, noparallel,
        [&](auto&& filename, auto&& shape) {
          return save_shape(path_join(dirname, filename), shape);
        });
    // save subdivs
    parallel_zip(subdiv_filenames, scene.subdivs, noparallel,
        [&](auto&& filename, auto&& subdiv) {
          return save_subdiv(path_join(dirname, filename), subdiv);
        });
    // save textures
    parallel_zip(texture_filenames, scene.textures, noparallel,
        [&](auto&& filename, auto&& texture) {
          return save_texture(path_join(dirname, filename), texture);
        });
  } catch (std::exception& except) {
    throw io_error(
        "cannot save " + filename + " since " + string(except.what()));
  }
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// OBJ CONVERSION
// -----------------------------------------------------------------------------
namespace yocto {

// Loads an OBJ
static scene_data load_obj_scene(const string& filename, bool noparallel) {
  // load obj
  auto obj = load_obj(filename, false, true);

  // scene
  auto scene = scene_data{};

  // convert cameras
  scene.cameras.reserve(obj.cameras.size());
  for (auto& ocamera : obj.cameras) {
    auto& camera        = scene.cameras.emplace_back();
    camera.frame        = to_math(ocamera.frame);
    camera.orthographic = ocamera.ortho;
    camera.film         = ocamera.film;
    camera.aspect       = ocamera.aspect;
    camera.focus        = ocamera.focus;
    camera.lens         = ocamera.lens;
    camera.aperture     = ocamera.aperture;
  }

  // convert between roughness and exponent
  auto exponent_to_roughness = [](float exponent) {
    if (exponent >= 1000) return 0.0f;
    auto roughness = exponent;
    roughness      = pow(2 / (roughness + 2), 1 / 4.0f);
    if (roughness < 0.01f) roughness = 0;
    if (roughness > 0.99f) roughness = 1;
    return roughness;
  };

  // handler for textures
  auto texture_paths = vector<string>{};
  for (auto& otexture : obj.textures) {
    scene.textures.emplace_back();
    texture_paths.emplace_back(otexture.path);
  }

  // handler for materials
  scene.materials.reserve(obj.materials.size());
  for (auto& omaterial : obj.materials) {
    auto& material        = scene.materials.emplace_back();
    material.type         = material_type::gltfpbr;
    material.emission     = to_math(omaterial.emission);
    material.emission_tex = omaterial.emission_tex;
    if (max(to_math(omaterial.transmission)) > 0.1) {
      material.type      = material_type::transparent;
      material.color     = to_math(omaterial.transmission);
      material.color_tex = omaterial.transmission_tex;
    } else if (max(to_math(omaterial.specular)) > 0.2) {
      material.type      = material_type::reflective;
      material.color     = to_math(omaterial.specular);
      material.color_tex = omaterial.specular_tex;
    } else if (max(to_math(omaterial.specular)) > 0) {
      material.type      = material_type::glossy;
      material.color     = to_math(omaterial.diffuse);
      material.color_tex = omaterial.diffuse_tex;
    } else {
      material.type      = material_type::matte;
      material.color     = to_math(omaterial.diffuse);
      material.color_tex = omaterial.diffuse_tex;
    }
    material.roughness  = exponent_to_roughness(omaterial.exponent);
    material.ior        = omaterial.ior;
    material.metallic   = 0;
    material.opacity    = omaterial.opacity;
    material.normal_tex = omaterial.normal_tex;
  }

  // convert shapes
  scene.shapes.reserve(obj.shapes.size());
  scene.instances.reserve(obj.shapes.size());
  for (auto& oshape : obj.shapes) {
    if (oshape.elements.empty()) continue;
    auto& shape       = scene.shapes.emplace_back();
    auto& instance    = scene.instances.emplace_back();
    instance.shape    = (int)scene.shapes.size() - 1;
    instance.material = oshape.elements.front().material;
    get_positions(oshape, (vector<array<float, 3>>&)shape.positions);
    get_normals(oshape, (vector<array<float, 3>>&)shape.normals);
    get_texcoords(oshape, (vector<array<float, 2>>&)shape.texcoords, true);
    get_faces(oshape, instance.material,
        (vector<array<int, 3>>&)shape.triangles,
        (vector<array<int, 4>>&)shape.quads);
    get_lines(oshape, instance.material, (vector<array<int, 2>>&)shape.lines);
    get_points(oshape, instance.material, shape.points);
  }

  // convert environments
  scene.environments.reserve(obj.environments.size());
  for (auto& oenvironment : obj.environments) {
    auto& environment        = scene.environments.emplace_back();
    environment.frame        = to_math(oenvironment.frame);
    environment.emission     = to_math(oenvironment.emission);
    environment.emission_tex = oenvironment.emission_tex;
  }

  // names
  scene.camera_names   = make_names(scene.cameras, {}, "camera");
  scene.texture_names  = make_names(scene.textures, {}, "texture");
  scene.material_names = make_names(scene.materials, {}, "material");
  scene.shape_names    = make_names(scene.shapes, {}, "shape");
  scene.subdiv_names   = make_names(scene.subdivs, {}, "subdiv");
  scene.instance_names = make_names(scene.instances, {}, "instance");

  // dirname
  auto dirname = path_dirname(filename);

  try {
    // load textures
    parallel_zip(texture_paths, scene.textures, noparallel,
        [&](auto&& path, auto&& texture) {
          texture = load_texture(path_join(dirname, path));
        });
  } catch (std::exception& except) {
    throw io_error(
        "cannot load " + filename + " since " + string(except.what()));
  }

  // fix scene
  add_missing_camera(scene);
  add_missing_radius(scene);

  // done
  return scene;
}

static void save_obj_scene(
    const string& filename, const scene_data& scene, bool noparallel) {
  // build obj
  auto obj = obj_model{};

  // convert cameras
  for (auto& camera : scene.cameras) {
    auto& ocamera    = obj.cameras.emplace_back();
    ocamera.name     = get_camera_name(scene, camera);
    ocamera.frame    = to_array(camera.frame);
    ocamera.ortho    = camera.orthographic;
    ocamera.film     = camera.film;
    ocamera.aspect   = camera.aspect;
    ocamera.focus    = camera.focus;
    ocamera.lens     = camera.lens;
    ocamera.aperture = camera.aperture;
  }

  // helper
  auto roughness_to_exponent = [](float roughness) -> float {
    if (roughness < 0.01f) return 10000;
    if (roughness > 0.99f) return 10;
    return 2 / pow(roughness, 4.0f) - 2;
  };

  // convert textures
  for (auto& texture : scene.textures) {
    auto& otexture = obj.textures.emplace_back();
    otexture.path  = "textures/" + get_texture_name(scene, texture) +
                    (!texture.pixelsf.empty() ? ".hdr"s : ".png"s);
  }

  // convert materials
  for (auto& material : scene.materials) {
    auto& omaterial        = obj.materials.emplace_back();
    omaterial.name         = get_material_name(scene, material);
    omaterial.illum        = 2;
    omaterial.emission     = to_array(material.emission);
    omaterial.diffuse      = to_array(material.color);
    omaterial.specular     = {0, 0, 0};
    omaterial.exponent     = roughness_to_exponent(material.roughness);
    omaterial.opacity      = material.opacity;
    omaterial.emission_tex = material.emission_tex;
    omaterial.diffuse_tex  = material.color_tex;
    omaterial.normal_tex   = material.normal_tex;
  }

  // convert objects
  for (auto& instance : scene.instances) {
    auto& shape     = scene.shapes[instance.shape];
    auto  positions = shape.positions, normals = shape.normals;
    for (auto& p : positions) p = transform_point(instance.frame, p);
    for (auto& n : normals) n = transform_normal(instance.frame, n);
    auto& oshape = obj.shapes.emplace_back();
    oshape.name  = get_shape_name(scene, shape);
    add_positions(oshape, (const vector<array<float, 3>>&)positions);
    add_normals(oshape, (const vector<array<float, 3>>&)normals);
    add_texcoords(
        oshape, (const vector<array<float, 2>>&)shape.texcoords, true);
    add_triangles(oshape, (const vector<array<int, 3>>&)shape.triangles,
        instance.material, !shape.normals.empty(), !shape.texcoords.empty());
    add_quads(oshape, (const vector<array<int, 4>>&)shape.quads,
        instance.material, !shape.normals.empty(), !shape.texcoords.empty());
    add_lines(oshape, (const vector<array<int, 2>>&)shape.lines,
        instance.material, !shape.normals.empty(), !shape.texcoords.empty());
    add_points(oshape, shape.points, instance.material, !shape.normals.empty(),
        !shape.texcoords.empty());
  }

  // convert environments
  for (auto& environment : scene.environments) {
    auto& oenvironment        = obj.environments.emplace_back();
    oenvironment.name         = get_environment_name(scene, environment);
    oenvironment.frame        = to_array(environment.frame);
    oenvironment.emission     = to_array(environment.emission);
    oenvironment.emission_tex = environment.emission_tex;
  }

  // save obj
  save_obj(filename, obj);

  // dirname
  auto dirname = path_dirname(filename);

  try {
    // save textures
    parallel_foreach(scene.textures, noparallel, [&](auto& texture) {
      auto path = "textures/" + get_texture_name(scene, texture) +
                  (!texture.pixelsf.empty() ? ".hdr"s : ".png"s);
      return save_texture(path_join(dirname, path), texture);
    });
  } catch (std::exception& except) {
    throw io_error(
        "cannot save " + filename + " since " + string(except.what()));
  }
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// PLY CONVERSION
// -----------------------------------------------------------------------------
namespace yocto {

static scene_data load_ply_scene(const string& filename, bool noparallel) {
  // scene
  auto scene = scene_data{};

  // load ply mesh and make instance
  auto shape = load_shape(filename);
  scene.shapes.push_back(shape);
  scene.instances.push_back({{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}},
      (int)scene.shapes.size() - 1, -1});

  // fix scene
  add_missing_material(scene);
  add_missing_camera(scene);
  add_missing_radius(scene);
  add_missing_lights(scene);

  // done
  return scene;
}

static void save_ply_scene(
    const string& filename, const scene_data& scene, bool noparallel) {
  // save shape
  if (scene.shapes.empty()) throw std::invalid_argument{"empty shape"};
  return save_shape(filename, scene.shapes.front());
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// STL CONVERSION
// -----------------------------------------------------------------------------
namespace yocto {

static scene_data load_stl_scene(const string& filename, bool noparallel) {
  // scene
  auto scene = scene_data{};

  // load ply mesh and make instance
  auto shape = load_shape(filename);
  scene.instances.push_back({{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}},
      (int)scene.shapes.size() - 1, -1});

  // fix scene
  add_missing_material(scene);
  add_missing_camera(scene);
  add_missing_radius(scene);
  add_missing_lights(scene);

  // done
  return scene;
}

static void save_stl_scene(
    const string& filename, const scene_data& scene, bool noparallel) {
  // save shape
  if (scene.shapes.empty()) throw std::invalid_argument{"empty shape"};
  return save_shape(filename, scene.shapes.front());
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// GLTF CONVESION
// -----------------------------------------------------------------------------
namespace yocto {

// Load a scene
static scene_data load_gltf_scene(const string& filename, bool noparallel) {
  // load gltf data
  auto data = load_binary(filename);

  // parse glTF
  auto options = cgltf_options{};
  memset(&options, 0, sizeof(options));
  auto cgltf_ptr    = (cgltf_data*)nullptr;
  auto cgltf_result = cgltf_parse(
      &options, data.data(), data.size(), &cgltf_ptr);
  if (cgltf_result != cgltf_result_success) {
    throw io_error{"cannot parse " + filename};
  }

  // load buffers
  auto dirname_      = path_dirname(filename);
  dirname_           = dirname_.empty() ? string{"./"} : (dirname_ + "/");
  auto buffer_result = cgltf_load_buffers(
      &options, cgltf_ptr, dirname_.c_str());
  if (buffer_result != cgltf_result_success) {
    cgltf_free(cgltf_ptr);
    throw io_error{"cannot load " + filename + " since cannot load buffers"};
  }

  // setup parsing
  auto& cgltf       = *cgltf_ptr;
  auto  cgltf_guard = std::unique_ptr<cgltf_data, void (*)(cgltf_data*)>(
      cgltf_ptr, cgltf_free);

  // scene
  auto scene = scene_data{};

  // convert cameras
  auto cameras = vector<camera_data>{};
  for (auto idx : range(cgltf.cameras_count)) {
    auto& gcamera = cgltf.cameras[idx];
    auto& camera  = cameras.emplace_back();
    if (gcamera.type == cgltf_camera_type_orthographic) {
      auto& gortho  = gcamera.data.orthographic;
      auto  xmag    = gortho.xmag;
      auto  ymag    = gortho.ymag;
      camera.aspect = xmag / ymag;
      camera.lens   = ymag;  // this is probably bogus
      camera.film   = 0.036f;
    } else if (gcamera.type == cgltf_camera_type_perspective) {
      auto& gpersp  = gcamera.data.perspective;
      camera.aspect = gpersp.has_aspect_ratio ? gpersp.aspect_ratio : 0.0f;
      auto yfov     = gpersp.yfov;
      if (camera.aspect == 0) camera.aspect = 16.0f / 9.0f;
      camera.film = 0.036f;
      if (camera.aspect >= 1) {
        camera.lens = (camera.film / camera.aspect) / (2 * tan(yfov / 2));
      } else {
        camera.lens = camera.film / (2 * tan(yfov / 2));
      }
      camera.focus = 1;
    } else {
      throw io_error{
          "cannot load " + filename + " for unsupported camera type"};
    }
  }

  // convert color textures
  auto get_texture = [&cgltf](const cgltf_texture_view& gview) -> int {
    if (gview.texture == nullptr) return -1;
    auto& gtexture = *gview.texture;
    if (gtexture.image == nullptr) return -1;
    return (int)(gtexture.image - cgltf.images);
  };

  // https://stackoverflow.com/questions/3418231/replace-part-of-a-string-with-another-string
  auto replace = [](const std::string& str_, const std::string& from,
                     const std::string& to) -> string {
    auto str = str_;
    if (from.empty()) return str;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
      str.replace(start_pos, from.length(), to);
      start_pos += to.length();
    }
    return str;
  };

  // convert textures
  auto texture_paths = vector<string>{};
  for (auto idx : range(cgltf.images_count)) {
    auto& gimage = cgltf.images[idx];
    scene.textures.emplace_back();
    texture_paths.push_back(replace(gimage.uri, "%20", " "));
  }

  // convert materials
  for (auto idx : range(cgltf.materials_count)) {
    auto& gmaterial   = cgltf.materials[idx];
    auto& material    = scene.materials.emplace_back();
    material.type     = material_type::gltfpbr;
    material.emission = {gmaterial.emissive_factor[0],
        gmaterial.emissive_factor[1], gmaterial.emissive_factor[2]};
    if (gmaterial.has_emissive_strength)
      material.emission *= gmaterial.emissive_strength.emissive_strength;
    material.emission_tex = get_texture(gmaterial.emissive_texture);
    material.normal_tex   = get_texture(gmaterial.normal_texture);
    if (gmaterial.has_pbr_metallic_roughness) {
      auto& gpbr        = gmaterial.pbr_metallic_roughness;
      material.type     = material_type::gltfpbr;
      material.color    = {gpbr.base_color_factor[0], gpbr.base_color_factor[1],
             gpbr.base_color_factor[2]};
      material.opacity  = gpbr.base_color_factor[3];
      material.metallic = gpbr.metallic_factor;
      material.roughness     = gpbr.roughness_factor;
      material.color_tex     = get_texture(gpbr.base_color_texture);
      material.roughness_tex = get_texture(gpbr.metallic_roughness_texture);
    }
    if (gmaterial.has_transmission) {
      auto& gtransmission = gmaterial.transmission;
      auto  transmission  = gtransmission.transmission_factor;
      if (transmission > 0) {
        material.type      = material_type::transparent;
        material.color     = {transmission, transmission, transmission};
        material.color_tex = get_texture(gtransmission.transmission_texture);
        // material.roughness = 0; // leave it set from before
      }
    }
  }

  // convert meshes
  auto mesh_primitives = vector<vector<instance_data>>{};
  for (auto idx : range(cgltf.meshes_count)) {
    auto& gmesh      = cgltf.meshes[idx];
    auto& primitives = mesh_primitives.emplace_back();
    if (gmesh.primitives == nullptr) continue;
    for (auto idx : range(gmesh.primitives_count)) {
      auto& gprimitive = gmesh.primitives[idx];
      if (gprimitive.attributes == nullptr) continue;
      auto& shape       = scene.shapes.emplace_back();
      auto& instance    = primitives.emplace_back();
      instance.shape    = (int)scene.shapes.size() - 1;
      instance.material = gprimitive.material
                              ? (int)(gprimitive.material - cgltf.materials)
                              : -1;
      for (auto idx : range(gprimitive.attributes_count)) {
        auto& gattribute = gprimitive.attributes[idx];
        auto& gaccessor  = *gattribute.data;
        if (gaccessor.is_sparse)
          throw io_error{
              "cannot load " + filename + " for unsupported sparse accessor"};
        auto gname       = string{gattribute.name};
        auto count       = gaccessor.count;
        auto components  = cgltf_num_components(gaccessor.type);
        auto dcomponents = components;
        auto data        = (float*)nullptr;
        if (gname == "POSITION") {
          if (components != 3)
            throw io_error{"cannot load " + filename +
                           " for unsupported position components"};
          shape.positions.resize(count);
          data = (float*)shape.positions.data();
        } else if (gname == "NORMAL") {
          if (components != 3)
            throw io_error{"cannot load " + filename +
                           " for unsupported normal components"};
          shape.normals.resize(count);
          data = (float*)shape.normals.data();
        } else if (gname == "TEXCOORD" || gname == "TEXCOORD_0") {
          if (components != 2)
            throw io_error{"cannot load " + filename +
                           " for unsupported texture components"};
          shape.texcoords.resize(count);
          data = (float*)shape.texcoords.data();
        } else if (gname == "COLOR" || gname == "COLOR_0") {
          if (components != 3 && components != 4)
            throw io_error{"cannot load " + filename +
                           " for unsupported color components"};
          shape.colors.resize(count);
          data = (float*)shape.colors.data();
          if (components == 3) {
            dcomponents = 4;
            for (auto& c : shape.colors) c.w = 1;
          }
        } else if (gname == "TANGENT") {
          if (components != 4)
            throw io_error{"cannot load " + filename +
                           " for unsupported tangent components"};
          shape.tangents.resize(count);
          data = (float*)shape.tangents.data();
        } else if (gname == "RADIUS") {
          if (components != 1)
            throw io_error{"cannot load " + filename +
                           " for unsupported radius components"};
          shape.radius.resize(count);
          data = (float*)shape.radius.data();
        } else {
          // ignore
          continue;
        }
        // convert values
        for (auto idx : range(count)) {
          if (!cgltf_accessor_read_float(
                  &gaccessor, idx, &data[idx * dcomponents], components))
            throw io_error{"cannot load " + filename +
                           " for unsupported accessor conversion"};
        }
        // fixes
        if (gname == "TANGENT") {
          for (auto& t : shape.tangents) t.w = -t.w;
        }
      }
      // indices
      if (gprimitive.indices == nullptr) {
        if (gprimitive.type == cgltf_primitive_type_triangles) {
          shape.triangles.resize(shape.positions.size() / 3);
          for (auto i = 0; i < (int)shape.positions.size() / 3; i++)
            shape.triangles[i] = {i * 3 + 0, i * 3 + 1, i * 3 + 2};
        } else if (gprimitive.type == cgltf_primitive_type_triangle_fan) {
          shape.triangles.resize(shape.positions.size() - 2);
          for (auto i = 2; i < (int)shape.positions.size(); i++)
            shape.triangles[i - 2] = {0, i - 1, i};
        } else if (gprimitive.type == cgltf_primitive_type_triangle_strip) {
          shape.triangles.resize(shape.positions.size() - 2);
          for (auto i = 2; i < (int)shape.positions.size(); i++)
            shape.triangles[i - 2] = {i - 2, i - 1, i};
        } else if (gprimitive.type == cgltf_primitive_type_lines) {
          shape.lines.resize(shape.positions.size() / 2);
          for (auto i = 0; i < (int)shape.positions.size() / 2; i++)
            shape.lines[i] = {i * 2 + 0, i * 2 + 1};
        } else if (gprimitive.type == cgltf_primitive_type_line_loop) {
          shape.lines.resize(shape.positions.size());
          for (auto i = 1; i < (int)shape.positions.size(); i++)
            shape.lines[i - 1] = {i - 1, i};
          shape.lines.back() = {(int)shape.positions.size() - 1, 0};
        } else if (gprimitive.type == cgltf_primitive_type_line_strip) {
          shape.lines.resize(shape.positions.size() - 1);
          for (auto i = 1; i < (int)shape.positions.size(); i++)
            shape.lines[i - 1] = {i - 1, i};
        } else if (gprimitive.type == cgltf_primitive_type_points) {
          throw io_error{
              "cannot load " + filename + " for unsupported point primitive"};
        } else {
          throw io_error{
              "cannot load " + filename + " for unsupported primitive type"};
        }
      } else {
        auto& gaccessor = *gprimitive.indices;
        if (gaccessor.type != cgltf_type_scalar)
          throw io_error{"cannot load " + filename +
                         " for unsupported non-scalar indices"};
        auto indices = vector<int>(gaccessor.count);
        for (auto idx : range(gaccessor.count)) {
          if (!cgltf_accessor_read_uint(
                  &gaccessor, idx, (cgltf_uint*)&indices[idx], 1))
            throw io_error{
                "cannot load " + filename + " for unsupported accessor type"};
        }
        if (gprimitive.type == cgltf_primitive_type_triangles) {
          shape.triangles.resize(indices.size() / 3);
          for (auto i = 0; i < (int)indices.size() / 3; i++) {
            shape.triangles[i] = {
                indices[i * 3 + 0], indices[i * 3 + 1], indices[i * 3 + 2]};
          }
        } else if (gprimitive.type == cgltf_primitive_type_triangle_fan) {
          shape.triangles.resize(indices.size() - 2);
          for (auto i = 2; i < (int)indices.size(); i++) {
            shape.triangles[i - 2] = {
                indices[0], indices[i - 1], indices[i + 0]};
          }
        } else if (gprimitive.type == cgltf_primitive_type_triangle_strip) {
          shape.triangles.resize(indices.size() - 2);
          for (auto i = 2; i < (int)indices.size(); i++) {
            shape.triangles[i - 2] = {
                indices[i - 2], indices[i - 1], indices[i + 0]};
          }
        } else if (gprimitive.type == cgltf_primitive_type_lines) {
          shape.lines.resize(indices.size() / 2);
          for (auto i = 0; i < (int)indices.size() / 2; i++) {
            shape.lines[i] = {indices[i * 2 + 0], indices[i * 2 + 1]};
          }
        } else if (gprimitive.type == cgltf_primitive_type_line_loop) {
          shape.lines.resize(indices.size());
          for (auto i : range(indices.size())) {
            shape.lines[i] = {
                indices[i + 0], indices[i + 1] % (int)indices.size()};
          }
        } else if (gprimitive.type == cgltf_primitive_type_line_strip) {
          shape.lines.resize(indices.size() - 1);
          for (auto i = 0; i < (int)indices.size() - 1; i++) {
            shape.lines[i] = {indices[i + 0], indices[i + 1]};
          }
        } else if (gprimitive.type == cgltf_primitive_type_points) {
          throw io_error{
              "cannot load " + filename + " for unsupported points indices"};
        } else {
          throw io_error{
              "cannot load " + filename + " for unsupported primitive type"};
        }
      }
    }
  }

  // convert nodes
  for (auto idx : range(cgltf.nodes_count)) {
    auto& gnode = cgltf.nodes[idx];
    if (gnode.camera != nullptr) {
      auto& camera = scene.cameras.emplace_back();
      camera       = cameras.at(gnode.camera - cgltf.cameras);
      auto xform   = mat4f{
            {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
      cgltf_node_transform_world(&gnode, &xform.x.x);
      camera.frame = mat_to_frame(xform);
    }
    if (gnode.mesh != nullptr) {
      for (auto& primitive : mesh_primitives.at(gnode.mesh - cgltf.meshes)) {
        auto& instance = scene.instances.emplace_back();
        instance       = primitive;
        auto xform     = mat4f{
                {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
        cgltf_node_transform_world(&gnode, &xform.x.x);
        instance.frame = mat_to_frame(xform);
      }
    }
  }

  // dirname
  auto dirname = path_dirname(filename);

  try {
    // load textures
    parallel_foreach(scene.textures, noparallel, [&](auto& texture) {
      auto& path = texture_paths[&texture - &scene.textures.front()];
      texture    = load_texture(path_join(dirname, path));
    });
  } catch (std::exception& except) {
    throw io_error(
        "cannot load " + filename + " since " + string(except.what()));
  }

  // fix scene
  add_missing_material(scene);
  add_missing_camera(scene);
  add_missing_radius(scene);
  add_missing_lights(scene);

  // done
  return scene;
}

// Load a scene
static void save_gltf_scene(
    const string& filename, const scene_data& scene, bool noparallel) {
  // prepare data
  auto cgltf_ptr = (cgltf_data*)malloc(sizeof(cgltf_data));
  memset(cgltf_ptr, 0, sizeof(cgltf_data));
  auto& cgltf = *cgltf_ptr;

  // helpers
  auto copy_string = [](const string& str) -> char* {
    if (str.empty()) return nullptr;
    auto copy = (char*)malloc(str.size() + 1);
    if (copy == nullptr) return nullptr;
    memcpy(copy, str.c_str(), str.size() + 1);
    return copy;
  };
  auto alloc_array = [](cgltf_size& size, auto*& ptr, size_t count) {
    using type =
        typename std::remove_pointer_t<std::remove_reference_t<decltype(ptr)>>;
    size = count;
    ptr  = (type*)malloc(count * sizeof(type));
    memset(ptr, 0, count * sizeof(type));
  };

  // asset
  cgltf.asset.version   = copy_string("2.0");
  cgltf.asset.generator = copy_string(
      "Yocto/GL - https://github.com/xelatihy/yocto-gl");
  cgltf.asset.copyright = copy_string(scene.copyright);

  // cameras
  if (!scene.cameras.empty()) {
    alloc_array(cgltf.cameras_count, cgltf.cameras, scene.cameras.size());
    for (auto idx : range(scene.cameras.size())) {
      auto& camera       = scene.cameras[idx];
      auto& gcamera      = cgltf.cameras[idx];
      gcamera.name       = copy_string(get_camera_name(scene, camera));
      gcamera.type       = cgltf_camera_type_perspective;
      auto& gperspective = gcamera.data.perspective;
      gperspective.has_aspect_ratio = true;
      gperspective.aspect_ratio     = camera.aspect;
      gperspective.yfov             = 0.660593;  // TODO(fabio): yfov
      gperspective.znear            = 0.001;     // TODO(fabio): configurable?
    }
  }

  // textures
  if (!scene.textures.empty()) {
    alloc_array(cgltf.textures_count, cgltf.textures, scene.textures.size());
    alloc_array(cgltf.images_count, cgltf.images, scene.textures.size());
    alloc_array(cgltf.samplers_count, cgltf.samplers, 1);
    auto& gsampler  = cgltf.samplers[0];
    gsampler.name   = copy_string("sampler");
    gsampler.wrap_s = 10497;
    gsampler.wrap_t = 10497;
    for (auto idx : range(scene.textures.size())) {
      auto& texture    = scene.textures[idx];
      auto& gtexture   = cgltf.textures[idx];
      auto& gimage     = cgltf.images[idx];
      auto  name       = get_texture_name(scene, texture);
      gimage.name      = copy_string(name);
      gimage.uri       = copy_string("textures/" + name + ".png");
      gtexture.name    = copy_string(name);
      gtexture.sampler = &gsampler;
      gtexture.image   = &gimage;
    }
  }

  // materials
  if (!scene.materials.empty()) {
    alloc_array(cgltf.materials_count, cgltf.materials, scene.materials.size());
    for (auto idx : range(scene.materials.size())) {
      auto& material  = scene.materials[idx];
      auto& gmaterial = cgltf.materials[idx];
      gmaterial.name  = copy_string(get_material_name(scene, material));
      auto emission_scale =
          max(material.emission) > 1.0f ? max(material.emission) : 1.0f;
      gmaterial.emissive_factor[0] = material.emission.x / emission_scale;
      gmaterial.emissive_factor[1] = material.emission.y / emission_scale;
      gmaterial.emissive_factor[2] = material.emission.z / emission_scale;
      if (emission_scale > 1.0f) {
        gmaterial.has_emissive_strength               = true;
        gmaterial.emissive_strength.emissive_strength = emission_scale;
      }
      gmaterial.has_pbr_metallic_roughness = true;
      auto& gpbr                           = gmaterial.pbr_metallic_roughness;
      gpbr.base_color_factor[0]            = material.color.x;
      gpbr.base_color_factor[1]            = material.color.y;
      gpbr.base_color_factor[2]            = material.color.z;
      gpbr.base_color_factor[3]            = material.opacity;
      gpbr.metallic_factor                 = material.metallic;
      gpbr.roughness_factor                = material.roughness;
      if (material.emission_tex != invalidid) {
        gmaterial.emissive_texture.texture = cgltf.textures +
                                             material.emission_tex;
        gmaterial.emissive_texture.scale = 1.0f;
      }
      if (material.normal_tex != invalidid) {
        gmaterial.normal_texture.texture = cgltf.textures + material.normal_tex;
        gmaterial.normal_texture.scale   = 1.0f;
      }
      if (material.color_tex != invalidid) {
        gpbr.base_color_texture.texture = cgltf.textures + material.color_tex;
        gpbr.base_color_texture.scale   = 1.0f;
      }
      if (material.roughness_tex != invalidid) {
        gpbr.metallic_roughness_texture.texture = cgltf.textures +
                                                  material.roughness_tex;
        gpbr.metallic_roughness_texture.scale = 1.0f;
      }
    }
  }

  // buffers
  auto shape_accessor_start = vector<int>{};
  if (!scene.shapes.empty()) {
    alloc_array(cgltf.buffers_count, cgltf.buffers, scene.shapes.size());
    alloc_array(
        cgltf.accessors_count, cgltf.accessors, scene.shapes.size() * 6);
    alloc_array(
        cgltf.buffer_views_count, cgltf.buffer_views, scene.shapes.size() * 6);
    shape_accessor_start.resize(scene.shapes.size(), 0);
    cgltf.accessors_count    = 0;
    cgltf.buffer_views_count = 0;
    auto add_vertex = [](cgltf_data& cgltf, cgltf_buffer& gbuffer, size_t count,
                          cgltf_type type, const float* data) {
      if (count == 0) return;
      auto  components         = cgltf_num_components(type);
      auto& gbufferview        = cgltf.buffer_views[cgltf.buffer_views_count++];
      gbufferview.buffer       = &gbuffer;
      gbufferview.offset       = gbuffer.size;
      gbufferview.size         = sizeof(float) * components * count;
      gbufferview.type         = cgltf_buffer_view_type_vertices;
      auto& gaccessor          = cgltf.accessors[cgltf.accessors_count++];
      gaccessor.buffer_view    = &gbufferview;
      gaccessor.count          = count;
      gaccessor.type           = type;
      gaccessor.component_type = cgltf_component_type_r_32f;
      gaccessor.has_min        = true;
      gaccessor.has_max        = true;
      for (auto component : range(components)) {
        gaccessor.min[component] = flt_max;
        gaccessor.max[component] = flt_min;
        for (auto idx : range(count)) {
          gaccessor.min[component] = min(
              gaccessor.min[component], data[idx * components + component]);
          gaccessor.max[component] = max(
              gaccessor.max[component], data[idx * components + component]);
        }
      }
      gbuffer.size += gbufferview.size;
    };
    auto add_element = [](cgltf_data& cgltf, cgltf_buffer& gbuffer,
                           size_t count, cgltf_type type) {
      if (count == 0) return;
      auto  components         = cgltf_num_components(type);
      auto& gbufferview        = cgltf.buffer_views[cgltf.buffer_views_count++];
      gbufferview.buffer       = &gbuffer;
      gbufferview.offset       = gbuffer.size;
      gbufferview.size         = sizeof(int) * components * count;
      gbufferview.type         = cgltf_buffer_view_type_indices;
      auto& gaccessor          = cgltf.accessors[cgltf.accessors_count++];
      gaccessor.buffer_view    = &gbufferview;
      gaccessor.count          = count * components;
      gaccessor.type           = cgltf_type_scalar;
      gaccessor.component_type = cgltf_component_type_r_32u;
      gbuffer.size += gbufferview.size;
    };
    for (auto idx : range(scene.shapes.size())) {
      auto& shape               = scene.shapes[idx];
      auto& gbuffer             = cgltf.buffers[idx];
      shape_accessor_start[idx] = (int)cgltf.accessors_count;
      gbuffer.uri               = copy_string(
          "shapes/" + get_shape_name(scene, shape) + ".bin");
      add_vertex(cgltf, gbuffer, shape.positions.size(), cgltf_type_vec3,
          (const float*)shape.positions.data());
      add_vertex(cgltf, gbuffer, shape.normals.size(), cgltf_type_vec3,
          (const float*)shape.normals.data());
      add_vertex(cgltf, gbuffer, shape.texcoords.size(), cgltf_type_vec2,
          (const float*)shape.texcoords.data());
      add_vertex(cgltf, gbuffer, shape.colors.size(), cgltf_type_vec4,
          (const float*)shape.colors.data());
      add_vertex(cgltf, gbuffer, shape.radius.size(), cgltf_type_scalar,
          (const float*)shape.radius.data());
      add_element(cgltf, gbuffer, shape.points.size(), cgltf_type_scalar);
      add_element(cgltf, gbuffer, shape.lines.size(), cgltf_type_vec2);
      add_element(cgltf, gbuffer, shape.triangles.size(), cgltf_type_vec3);
      add_element(cgltf, gbuffer, quads_to_triangles(shape.quads).size(),
          cgltf_type_vec3);
    }
  }

  // meshes
  using mesh_key = pair<int, int>;
  struct mesh_hash {
    auto operator()(const pair<int, int>& key) const {
      auto packed = ((uint64_t)key.first << 32) | (uint64_t)key.second;
      return std::hash<uint64_t>()(packed);
    }
  };
  auto mesh_map = unordered_map<mesh_key, size_t, mesh_hash>{};
  if (!scene.instances.empty()) {
    alloc_array(cgltf.meshes_count, cgltf.meshes, scene.instances.size());
    cgltf.meshes_count = 0;
    for (auto idx : range(scene.instances.size())) {
      auto& instance = scene.instances[idx];
      if (mesh_map.find({instance.shape, instance.material}) != mesh_map.end())
        continue;
      mesh_map[{instance.shape, instance.material}] = (int)cgltf.meshes_count;
      auto& shape = scene.shapes[instance.shape];
      auto& gmesh = cgltf.meshes[cgltf.meshes_count++];
      alloc_array(gmesh.primitives_count, gmesh.primitives, 1);
      auto& gprimitive    = gmesh.primitives[0];
      gprimitive.material = cgltf.materials + instance.material;
      alloc_array(gprimitive.attributes_count, gprimitive.attributes, 5);
      gprimitive.attributes_count = 0;
      auto cur_accessor           = shape_accessor_start[instance.shape];
      if (!shape.positions.empty()) {
        auto& gattribute = gprimitive.attributes[gprimitive.attributes_count++];
        gattribute.type  = cgltf_attribute_type_position;
        gattribute.name  = copy_string("POSITION");
        gattribute.data  = cgltf.accessors + cur_accessor++;
      }
      if (!shape.normals.empty()) {
        auto& gattribute = gprimitive.attributes[gprimitive.attributes_count++];
        gattribute.type  = cgltf_attribute_type_normal;
        gattribute.name  = copy_string("NORMAL");
        gattribute.data  = cgltf.accessors + cur_accessor++;
      }
      if (!shape.texcoords.empty()) {
        auto& gattribute = gprimitive.attributes[gprimitive.attributes_count++];
        gattribute.type  = cgltf_attribute_type_texcoord;
        gattribute.name  = copy_string("TEXCOORD_0");
        gattribute.data  = cgltf.accessors + cur_accessor++;
      }
      if (!shape.colors.empty()) {
        auto& gattribute = gprimitive.attributes[gprimitive.attributes_count++];
        gattribute.type  = cgltf_attribute_type_color;
        gattribute.name  = copy_string("COLOR_0");
        gattribute.data  = cgltf.accessors + cur_accessor++;
      }
      if (!shape.radius.empty()) {
        auto& gattribute = gprimitive.attributes[gprimitive.attributes_count++];
        gattribute.type  = cgltf_attribute_type_invalid;
        gattribute.name  = copy_string("RADIUS");
        gattribute.data  = cgltf.accessors + cur_accessor++;
      }
      if (!shape.points.empty()) {
        gprimitive.type    = cgltf_primitive_type_points;
        gprimitive.indices = cgltf.accessors + cur_accessor++;
      } else if (!shape.lines.empty()) {
        gprimitive.type    = cgltf_primitive_type_lines;
        gprimitive.indices = cgltf.accessors + cur_accessor++;
      } else if (!shape.triangles.empty()) {
        gprimitive.type    = cgltf_primitive_type_triangles;
        gprimitive.indices = cgltf.accessors + cur_accessor++;
      } else if (!shape.quads.empty()) {
        gprimitive.type    = cgltf_primitive_type_triangles;
        gprimitive.indices = cgltf.accessors + cur_accessor++;
      }
    }
  }

  // nodes
  if (!scene.cameras.empty() || !scene.instances.empty()) {
    alloc_array(cgltf.nodes_count, cgltf.nodes,
        scene.cameras.size() + scene.instances.size() + 1);
    for (auto idx : range(scene.cameras.size())) {
      auto& camera = scene.cameras[idx];
      auto& gnode  = cgltf.nodes[idx];
      gnode.name   = copy_string(get_camera_name(scene, camera));
      auto xform   = frame_to_mat(camera.frame);
      memcpy(gnode.matrix, &xform, sizeof(mat4f));
      gnode.has_matrix = true;
      gnode.camera     = cgltf.cameras + idx;
    }
    for (auto idx : range(scene.instances.size())) {
      auto& instance = scene.instances[idx];
      auto& gnode    = cgltf.nodes[idx + scene.cameras.size()];
      gnode.name     = copy_string(get_instance_name(scene, instance));
      auto xform     = frame_to_mat(instance.frame);
      memcpy(gnode.matrix, &xform, sizeof(mat4f));
      gnode.has_matrix = true;
      gnode.mesh       = cgltf.meshes +
                   mesh_map.at({instance.shape, instance.material});
    }
    // root children
    auto& groot = cgltf.nodes[cgltf.nodes_count - 1];
    groot.name  = copy_string("root");
    alloc_array(groot.children_count, groot.children, cgltf.nodes_count - 1);
    for (auto idx : range(cgltf.nodes_count - 1))
      groot.children[idx] = cgltf.nodes + idx;
    // scene
    alloc_array(cgltf.scenes_count, cgltf.scenes, 1);
    auto& gscene = cgltf.scenes[0];
    alloc_array(gscene.nodes_count, gscene.nodes, 1);
    gscene.nodes[0] = cgltf.nodes + cgltf.nodes_count - 1;
    cgltf.scene     = cgltf.scenes;
  }

  // save gltf
  auto options = cgltf_options{};
  memset(&options, 0, sizeof(options));
  auto result = cgltf_write_file(&options, filename.c_str(), &cgltf);
  if (result != cgltf_result_success) {
    cgltf_free(&cgltf);
    throw io_error{"cannot save " + filename};
  }

  // cleanup
  cgltf_free(&cgltf);

  // get filename from name
  auto make_filename = [filename](const string& name, const string& group,
                           const string& extension) {
    return path_join(path_dirname(filename), group, name + extension);
  };

  // dirname
  auto dirname = path_dirname(filename);

  try {
    // save shapes
    parallel_foreach(scene.shapes, noparallel, [&](auto& shape) {
      auto path = "shapes/" + get_shape_name(scene, shape) + ".bin";
      return save_binshape(path_join(dirname, path), shape);
    });
    // save textures
    parallel_foreach(scene.textures, noparallel, [&](auto& texture) {
      auto path = "textures/" + get_texture_name(scene, texture) + ".png";
      return save_texture(path_join(dirname, path), texture);
    });
  } catch (std::exception& except) {
    throw io_error(
        "cannot save " + filename + " since " + string(except.what()));
  }
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// IMPLEMENTATION OF PBRT
// -----------------------------------------------------------------------------
namespace yocto {

// load pbrt scenes
static scene_data load_pbrt_scene(const string& filename, bool noparallel) {
  // load pbrt
  auto pbrt = load_pbrt(filename);

  // scene
  auto scene = scene_data{};

  // convert cameras
  for (auto& pcamera : pbrt.cameras) {
    auto& camera  = scene.cameras.emplace_back();
    camera.frame  = to_math(pcamera.frame);
    camera.aspect = pcamera.aspect;
    camera.film   = 0.036f;
    camera.lens   = pcamera.lens;
    camera.focus  = pcamera.focus;
  }

  // convert material
  auto texture_paths = vector<string>{};
  for (auto& ptexture : pbrt.textures) {
    scene.textures.emplace_back();
    texture_paths.push_back(ptexture.filename);
  }

  // material type map
  auto material_type_map = unordered_map<pbrt_mtype, material_type>{
      {pbrt_mtype::matte, material_type::matte},
      {pbrt_mtype::plastic, material_type::glossy},
      {pbrt_mtype::metal, material_type::reflective},
      {pbrt_mtype::glass, material_type::refractive},
      {pbrt_mtype::thinglass, material_type::transparent},
      {pbrt_mtype::subsurface, material_type::matte},
  };

  // convert material
  for (auto& pmaterial : pbrt.materials) {
    auto& material = scene.materials.emplace_back();
    material.type  = material_type_map.at(pmaterial.type);
    if (to_math(pmaterial.emission) != vec3f{0, 0, 0}) {
      material.type = material_type::matte;
    }
    material.emission  = to_math(pmaterial.emission);
    material.color     = to_math(pmaterial.color);
    material.ior       = pmaterial.ior;
    material.roughness = pmaterial.roughness;
    material.opacity   = pmaterial.opacity;
    material.color_tex = pmaterial.color_tex;
  }

  // convert shapes
  auto shapes_paths = vector<string>{};
  for (auto& pshape : pbrt.shapes) {
    auto& shape = scene.shapes.emplace_back();
    shapes_paths.emplace_back(pshape.filename_);
    shape.positions = (const vector<vec3f>&)pshape.positions;
    shape.normals   = (const vector<vec3f>&)pshape.normals;
    shape.texcoords = (const vector<vec2f>&)pshape.texcoords;
    shape.triangles = (const vector<vec3i>&)pshape.triangles;
    for (auto& uv : shape.texcoords) uv.y = 1 - uv.y;
    if (!pshape.instanced) {
      auto& instance    = scene.instances.emplace_back();
      instance.frame    = to_math(pshape.frame);
      instance.shape    = (int)scene.shapes.size() - 1;
      instance.material = pshape.material;
    } else {
      for (auto& frame : pshape.instances) {
        auto& instance    = scene.instances.emplace_back();
        instance.frame    = to_math(frame) * to_math(pshape.frame);
        instance.shape    = (int)scene.shapes.size() - 1;
        instance.material = pshape.material;
      }
    }
  }

  // convert environments
  for (auto& penvironment : pbrt.environments) {
    auto& environment        = scene.environments.emplace_back();
    environment.frame        = to_math(penvironment.frame);
    environment.emission     = to_math(penvironment.emission);
    environment.emission_tex = penvironment.emission_tex;
  }

  // lights
  for (auto& plight : pbrt.lights) {
    auto& shape = scene.shapes.emplace_back();
    shapes_paths.emplace_back();
    shape.triangles   = (const vector<vec3i>&)plight.area_triangles;
    shape.positions   = (const vector<vec3f>&)plight.area_positions;
    shape.normals     = (const vector<vec3f>&)plight.area_normals;
    auto& material    = scene.materials.emplace_back();
    material.emission = to_math(plight.area_emission);
    auto& instance    = scene.instances.emplace_back();
    instance.shape    = (int)scene.shapes.size() - 1;
    instance.material = (int)scene.materials.size() - 1;
    instance.frame    = to_math(plight.area_frame);
  }

  // dirname
  auto dirname = path_dirname(filename);

  try {
    // load shapes
    parallel_foreach(scene.shapes, noparallel, [&](auto& shape) {
      auto& path = shapes_paths[&shape - &scene.shapes.front()];
      if (path.empty()) return;
      shape = load_shape(path_join(dirname, path));
    });
    // load textures
    parallel_foreach(scene.textures, noparallel, [&](auto& texture) {
      auto& path = texture_paths[&texture - &scene.textures.front()];
      texture    = load_texture(path_join(dirname, path));
    });
  } catch (std::exception& except) {
    throw io_error(
        "cannot load " + filename + " since " + string(except.what()));
  }

  // fix scene
  add_missing_camera(scene);
  add_missing_radius(scene);

  // done
  return scene;
}

// Save a pbrt scene
static void save_pbrt_scene(
    const string& filename, const scene_data& scene, bool noparallel) {
  // save pbrt
  auto pbrt = pbrt_model{};

  // convert camera
  auto& camera       = scene.cameras.front();
  auto& pcamera      = pbrt.cameras.emplace_back();
  pcamera.frame      = to_array(camera.frame);
  pcamera.lens       = camera.lens;
  pcamera.aspect     = camera.aspect;
  pcamera.resolution = {1280, (int)(1280 / pcamera.aspect)};

  // convert textures
  for (auto& texture : scene.textures) {
    auto& ptexture    = pbrt.textures.emplace_back();
    ptexture.filename = "textures/" + get_texture_name(scene, texture) +
                        (!texture.pixelsf.empty() ? ".hdr" : ".png");
  }

  // material type map
  auto material_type_map = unordered_map<material_type, pbrt_mtype>{
      {material_type::matte, pbrt_mtype::matte},
      {material_type::glossy, pbrt_mtype::plastic},
      {material_type::reflective, pbrt_mtype::metal},
      {material_type::refractive, pbrt_mtype::glass},
      {material_type::transparent, pbrt_mtype::thinglass},
      {material_type::subsurface, pbrt_mtype::matte},
      {material_type::volumetric, pbrt_mtype::matte},
  };

  // convert materials
  for (auto& material : scene.materials) {
    auto& pmaterial     = pbrt.materials.emplace_back();
    pmaterial.name      = get_material_name(scene, material);
    pmaterial.type      = material_type_map.at(material.type);
    pmaterial.emission  = to_array(material.emission);
    pmaterial.color     = to_array(material.color);
    pmaterial.roughness = material.roughness;
    pmaterial.ior       = material.ior;
    pmaterial.opacity   = material.opacity;
    pmaterial.color_tex = material.color_tex;
  }

  // convert instances
  for (auto& instance : scene.instances) {
    auto& pshape     = pbrt.shapes.emplace_back();
    pshape.filename_ = get_shape_name(scene, instance.shape) + ".ply";
    pshape.frame     = to_array(instance.frame);
    pshape.frend     = to_array(instance.frame);
    pshape.material  = instance.material;
  }

  // convert environments
  for (auto& environment : scene.environments) {
    auto& penvironment        = pbrt.environments.emplace_back();
    penvironment.emission     = to_array(environment.emission);
    penvironment.emission_tex = environment.emission_tex;
  }

  // save pbrt
  save_pbrt(filename, pbrt);

  // dirname
  auto dirname = path_dirname(filename);

  try {
    // save shapes
    parallel_foreach(scene.shapes, noparallel, [&](auto& shape) {
      auto path = "shapes/" + get_shape_name(scene, shape) + ".ply";
      return save_shape(path_join(dirname, path), shape);
    });
    // save textures
    parallel_foreach(scene.textures, noparallel, [&](auto& texture) {
      auto path = "textures/" + get_texture_name(scene, texture) +
                  (!texture.pixelsf.empty() ? ".hdr"s : ".png"s);
      return save_texture(path_join(dirname, path), texture);
    });
  } catch (std::exception& except) {
    throw io_error(
        "cannot save " + filename + " since " + string(except.what()));
  }
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// IMPLEMENTATION OF MITSUBA
// -----------------------------------------------------------------------------
namespace yocto {

// load mitsuba scenes
static scene_data load_mitsuba_scene(const string& filename, bool noparallel) {
  // not implemented
  throw io_error(
      "cannot load " + filename + " since format is not supported for reading");
}

// To xml helpers
static void xml_attribute(string& xml, const string& name, bool value) {
  xml += " " + name + "=\"" + (value ? string("true") : string("false")) + "\"";
}
static void xml_attribute(string& xml, const string& name, int value) {
  xml += " " + name + "=\"" + std::to_string(value) + "\"";
}
static void xml_attribute(string& xml, const string& name, float value) {
  xml += " " + name + "=\"" + std::to_string(value) + "\"";
}
static void xml_attribute(string& xml, const string& name, const char* value) {
  xml += " " + name + "=\"" + string(value) + "\"";
}
static void xml_attribute(
    string& xml, const string& name, const string& value) {
  xml += " " + name + "=\"" + value + "\"";
}
static void xml_attribute(string& xml, const string& name, vec3f value) {
  xml += " " + name + "=\"" + std::to_string(value.x) + " " +
         std::to_string(value.y) + " " + std::to_string(value.z) + "\"";
}
static void xml_attribute(
    string& xml, const string& name, const frame3f& value) {
  xml += " " + name + "=\"" + std::to_string(value.x.x) + " " +
         std::to_string(value.y.x) + " " + std::to_string(value.z.x) + " " +
         std::to_string(value.o.x) + " " + std::to_string(value.x.y) + " " +
         std::to_string(value.y.y) + " " + std::to_string(value.z.y) + " " +
         std::to_string(value.o.y) + " " + std::to_string(value.x.z) + " " +
         std::to_string(value.y.z) + " " + std::to_string(value.z.z) + " " +
         std::to_string(value.o.z) + " 0 0 0 1\"";
}
template <typename T, typename... Ts>
static void xml_attributes(
    string& xml, const string& name, const T& value, const Ts&... attributes) {
  xml_attribute(xml, name, value);
  if constexpr (sizeof...(attributes) != 0) xml_attributes(xml, attributes...);
}
template <typename... Ts>
static void xml_element(string& xml, const string& indent, const string& name,
    const Ts&... attributes) {
  xml += indent + "<" + name;
  xml_attributes(xml, attributes...);
  xml += "/>\n";
}
template <typename... Ts>
static void xml_begin(
    string& xml, string& indent, const string& name, const Ts&... attributes) {
  xml += indent + "<" + name;
  xml_attributes(xml, attributes...);
  xml += ">\n";
  indent += "  ";
}
static void xml_end(string& xml, string& indent, const string& name) {
  indent = indent.substr(0, indent.size() - 2);
  xml += indent + "</" + name + ">\n";
}
template <typename T>
static void xml_default(
    string& xml, const string& indent, const string& name, const T& value) {
  xml_element(xml, indent, "default", "name", name, "value", value);
}
template <typename T>
static void xml_property(string& xml, const string& indent, const string& type,
    const string& name, const T& value, const string& ref) {
  if (ref.empty()) {
    if (name.empty()) {
      xml_element(xml, indent, type, "value", value);
    } else {
      xml_element(xml, indent, type, "name", name, "value", value);
    }
  } else {
    xml_element(xml, indent, type, "name", name, "value", ref);
  }
}
static void xml_property(string& xml, const string& indent, const string& name,
    int value, const string& ref = "") {
  xml_property(xml, indent, "integer", name, value, ref);
}
static void xml_property(string& xml, const string& indent, const string& name,
    float value, const string& ref = "") {
  xml_property(xml, indent, "float", name, value, ref);
}
static void xml_property(string& xml, const string& indent, const string& name,
    bool value, const string& ref = "") {
  xml_property(xml, indent, "boolean", name, value, ref);
}
static void xml_property(string& xml, const string& indent, const string& name,
    const char* value, const string& ref = "") {
  xml_property(xml, indent, "string", name, value, ref);
}
static void xml_property(string& xml, const string& indent, const string& name,
    const string& value, const string& ref = "") {
  xml_property(xml, indent, "string", name, value, ref);
}
static void xml_property(string& xml, const string& indent, const string& name,
    const frame3f& value, const string& ref = "") {
  xml_property(xml, indent, "matrix", name, value, ref);
}
static void xml_property(string& xml, const string& indent, const string& name,
    vec3f value, const string& ref = "") {
  xml_property(xml, indent, "rgb", name, value, ref);
}

// Save a mitsuba scene
static void save_mitsuba_scene(
    const string& filename, const scene_data& scene, bool noparallel) {
  // write xml directly
  auto xml = string{};

  // begin
  auto indent = string{};
  xml_begin(xml, indent, "scene", "version", "3.0.0");

  // defaults
  xml_default(xml, indent, "integrator", "path");
  xml_default(xml, indent, "spp", 64);
  xml_default(xml, indent, "resx", 1440);
  xml_default(xml, indent, "resy", 720);
  xml_default(xml, indent, "pixel_format", "rgb");
  xml_default(xml, indent, "max_depth", 8);
  xml_default(xml, indent, "rr_depth", 64);

  // integrator
  xml_begin(xml, indent, "integrator", "type", "$integrator");
  xml_property(xml, indent, "max_depth", 0, "$max_depth");
  xml_property(xml, indent, "rr_depth", 0, "$rr_depth");
  xml_property(xml, indent, "hide_emitters", false);
  xml_end(xml, indent, "integrator");

  // film
  xml_begin(xml, indent, "film", "type", "hdrfilm", "id", "film");
  xml_property(xml, indent, "width", 0, "$resx");
  xml_property(xml, indent, "height", 0, "$resy");
  xml_element(xml, indent, "rfilter", "type", "box");
  xml_property(xml, indent, "pixel_format", "", "$pixel_format");
  xml_end(xml, indent, "film");

  // sampler
  xml_begin(xml, indent, "sampler", "type", "independent", "id", "sampler");
  xml_property(xml, indent, "sample_count", 0, "$spp");
  xml_end(xml, indent, "sampler");

  // camera
  auto& camera = scene.cameras.at(0);
  xml_begin(xml, indent, "sensor", "type", "perspective");
  xml_property(xml, indent, "fov_axis", "smaller");
  xml_property(xml, indent, "fov", 20.0f);
  xml_begin(xml, indent, "transform", "name", "to_world");
  xml_element(xml, indent, "lookat", "origin", camera.frame.o, "target",
      camera.frame.o - camera.frame.z, "up", vec3f{0, 1, 0});
  xml_end(xml, indent, "transform");
  xml_element(xml, indent, "ref", "id", "sampler");
  xml_element(xml, indent, "ref", "id", "film");
  xml_end(xml, indent, "sensor");

  // textures
  auto tid = 0;
  for (auto& texture : scene.textures) {
    if (texture.pixelsf.empty()) {
      xml_begin(xml, indent, "texture", "type", "bitmap", "id",
          "texture" + std::to_string(tid));
      xml_property(xml, indent, "filename",
          "textures/" + get_texture_name(scene, texture) +
              (texture.pixelsf.empty() ? ".png" : ".hdr"));
      xml_end(xml, indent, "texture");
    }
    tid += 1;
  }

  // environments
  for (auto& environment : scene.environments) {
    if (environment.emission_tex != invalidid) {
      auto& texture = scene.textures.at(environment.emission_tex);
      xml_begin(xml, indent, "emitter", "type", "envmap");
      xml_property(xml, indent, "scale", mean(environment.emission));
      xml_property(xml, indent, "filename",
          "textures/" + get_texture_name(scene, texture) + ".hdr");
      xml_end(xml, indent, "emitter");
    } else {
      xml_begin(xml, indent, "emitter", "type", "constant");
      xml_property(xml, indent, "rgb", "radiance", environment.emission, "");
      xml_end(xml, indent, "emitter");
    }
  }

  // property or texture
  auto xml_property_or_texture = [](string& xml, const string& indent,
                                     const string& name, auto value,
                                     int texture) {
    if (texture == invalidid) {
      xml_property(xml, indent, "rgb", name, value, "");
    } else {
      xml_element(xml, indent, "ref", "id", "texture" + std::to_string(texture),
          "name", name);
    }
  };

  // materials
  auto mid = 0;
  for (auto& material : scene.materials) {
    // xml_begin(xml, indent, "bsdf", "type", "twosided", "id",
    //     "material" + std::to_string(mid));
    switch (material.type) {
      case material_type::matte: {
        xml_begin(xml, indent, "bsdf", "type", "diffuse", "id",
            "material" + std::to_string(mid));
        xml_property_or_texture(
            xml, indent, "reflectance", material.color, material.color_tex);
        xml_end(xml, indent, "bsdf");
      } break;
      case material_type::reflective: {
        xml_begin(xml, indent, "bsdf", "type",
            material.roughness < 0.03f ? "conductor" : "roughconductor", "id",
            "material" + std::to_string(mid));
        xml_property_or_texture(
            xml, indent, "eta", reflectivity_to_eta(material.color), invalidid);
        xml_property_or_texture(xml, indent, "k", vec3f{0, 0, 0}, invalidid);
        if (material.roughness >= 0.03f) {
          xml_property(
              xml, indent, "alpha", material.roughness * material.roughness);
        }
        xml_end(xml, indent, "bsdf");
      } break;
      case material_type::glossy: {
        xml_begin(xml, indent, "bsdf", "type",
            material.roughness < 0.03f ? "plastic" : "roughplastic", "id",
            "material" + std::to_string(mid));
        xml_property_or_texture(xml, indent, "diffuse_reflectance",
            material.color, material.color_tex);
        if (material.roughness >= 0.03f) {
          xml_property(
              xml, indent, "alpha", material.roughness * material.roughness);
        }
        xml_end(xml, indent, "bsdf");
      } break;
      case material_type::transparent: {
        xml_begin(xml, indent, "bsdf", "type",
            material.roughness < 0.03f ? "conductor" : "roughconductor", "id",
            "material" + std::to_string(mid));
        xml_property_or_texture(
            xml, indent, "eta", reflectivity_to_eta(material.color), invalidid);
        xml_property_or_texture(xml, indent, "k", vec3f{0, 0, 0}, invalidid);
        if (material.roughness >= 0.03f) {
          xml_property_or_texture(
              xml, indent, "alpha", material.roughness, invalidid);
        }
        xml_end(xml, indent, "bsdf");
      } break;
      case material_type::volumetric:
      case material_type::subsurface:
      case material_type::refractive: {
        xml_begin(xml, indent, "bsdf", "type",
            material.roughness < 0.03f ? "dielectric" : "roughdielectric", "id",
            "material" + std::to_string(mid));
        xml_property(xml, indent, "int_ior", 1.5f);
        if (material.roughness >= 0.03f) {
          xml_property_or_texture(
              xml, indent, "alpha", material.roughness, invalidid);
        }
        xml_end(xml, indent, "bsdf");
        if (material.color != vec3f{1, 1, 1}) {
          xml_begin(xml, indent, "medium", "type", "homogeneous", "id",
              "medium" + std::to_string(mid));
          xml_property(xml, indent, "albedo", material.scattering);
          auto density = -log(clamp(material.color, 0.0001f, 1.0f)) /
                         material.trdepth;
          xml_property(xml, indent, "sigma_t", mean(density));
          xml_end(xml, indent, "medium");
        }
      } break;
      case material_type::gltfpbr: {
        xml_begin(xml, indent, "bsdf", "type", "diffuse", "id",
            "material" + std::to_string(mid));
        xml_property_or_texture(
            xml, indent, "reflectance", material.color, material.color_tex);
        xml_end(xml, indent, "bsdf");
      } break;
    }
    // todo
    // xml_end(xml, indent, "bsdf");
    mid += 1;
  }

  // flattened instances
  for (auto& instance : scene.instances) {
    auto& shape    = scene.shapes[instance.shape];
    auto& material = scene.materials[instance.material];
    xml_begin(xml, indent, "shape", "type", "ply");
    xml_property(xml, indent, "filename",
        "shapes/" + get_shape_name(scene, shape) + ".ply");
    if (instance.frame != frame3f{}) {
      xml_begin(xml, indent, "transform", "name", "to_world");
      xml_property(xml, indent, "", instance.frame);
      xml_end(xml, indent, "transform");
    }
    if (material.emission != vec3f{0, 0, 0}) {
      xml_property(xml, indent, "flip_normals", true);
      xml_begin(xml, indent, "emitter", "type", "area");
      xml_property(xml, indent, "rgb", "radiance", material.emission, "");
      xml_end(xml, indent, "emitter");
    }
    xml_element(xml, indent, "ref", "id",
        "material" + std::to_string(instance.material));
    if (material.type == material_type::refractive &&
        material.color != vec3f{1, 1, 1}) {
      xml_element(xml, indent, "ref", "name", "interior", "id",
          "medium" + std::to_string(instance.material));
    }
    xml_end(xml, indent, "shape");
  }

  // close
  xml_end(xml, indent, "scene");

  // save xml
  save_text(filename, xml);

  // dirname
  auto dirname = path_dirname(filename);

  auto triangulate = [](const shape_data& shape) -> shape_data {
    if (shape.quads.empty()) return shape;
    auto tshape      = shape;
    tshape.triangles = quads_to_triangles(shape.quads);
    tshape.quads.clear();
    return tshape;
  };

  try {
    // save shapes
    parallel_foreach(scene.shapes, noparallel, [&](auto& shape) {
      auto path = "shapes/" + get_shape_name(scene, shape) + ".ply";
      return save_shape(path_join(dirname, path), triangulate(shape));
    });
    // save textures
    parallel_foreach(scene.textures, noparallel, [&](auto& texture) {
      auto path = "textures/" + get_texture_name(scene, texture) +
                  (!texture.pixelsf.empty() ? ".hdr"s : ".png"s);
      return save_texture(path_join(dirname, path), texture);
    });
  } catch (std::exception& except) {
    throw io_error(
        "cannot save " + filename + " since " + string(except.what()));
  }
}

}  // namespace yocto
