//
// Implementation for Yocto/Trace.
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

#include "yocto_trace.h"

#include <algorithm>
#include <cstring>
#include <future>
#include <memory>
#include <stdexcept>
#include <utility>

#include "yocto_color.h"
#include "yocto_geometry.h"
#include "yocto_sampling.h"
#include "yocto_shading.h"
#include "yocto_shape.h"

#ifdef YOCTO_DENOISE
#include <OpenImageDenoise/oidn.hpp>
#endif

// -----------------------------------------------------------------------------
// PARALLEL HELPERS
// -----------------------------------------------------------------------------
namespace yocto {

// Simple parallel for used since our target platforms do not yet support
// parallel algorithms. `Func` takes the two integer indices.
template <typename T, typename Func>
inline void parallel_for(T num1, T num2, Func&& func) {
  auto              futures  = vector<std::future<void>>{};
  auto              nthreads = std::thread::hardware_concurrency();
  std::atomic<T>    next_idx(0);
  std::atomic<bool> has_error(false);
  for (auto thread_id = 0; thread_id < (int)nthreads; thread_id++) {
    futures.emplace_back(std::async(
        std::launch::async, [&func, &next_idx, &has_error, num1, num2]() {
          try {
            while (true) {
              auto j = next_idx.fetch_add(1);
              if (j >= num2) break;
              if (has_error) break;
              for (auto i = (T)0; i < num1; i++) func(i, j);
            }
          } catch (...) {
            has_error = true;
            throw;
          }
        }));
  }
  for (auto& f : futures) f.get();
}

// Simple parallel for used since our target platforms do not yet support
// parallel algorithms. `Func` takes the two integer indices.
template <typename Func>
inline void parallel_for_batch(vec2i num, Func&& func) {
  auto              futures  = vector<std::future<void>>{};
  auto              nthreads = std::thread::hardware_concurrency();
  std::atomic<int>  next_idx(0);
  std::atomic<bool> has_error(false);
  for (auto thread_id = 0; thread_id < (int)nthreads; thread_id++) {
    futures.emplace_back(
        std::async(std::launch::async, [&func, &next_idx, &has_error, num]() {
          try {
            while (true) {
              auto j = next_idx.fetch_add(1);
              if (j >= num[1]) break;
              if (has_error) break;
              for (auto i = 0; i < num[0]; i++) func(vec2i{i, j});
            }
          } catch (...) {
            has_error = true;
            throw;
          }
        }));
  }
  for (auto& f : futures) f.get();
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// IMPLEMENTATION OF RAY-SCENE INTERSECTION
// -----------------------------------------------------------------------------
namespace yocto {

// Build the Bvh acceleration structure.
trace_bvh make_trace_bvh(const scene_data& scene, const trace_params& params) {
  if (params.embreebvh && embree_supported()) {
    return {
        {}, make_scene_ebvh(scene, params.highqualitybvh, params.noparallel)};
  } else {
    return {
        make_scene_bvh(scene, params.highqualitybvh, params.noparallel), {}};
  }
}

// Ray-intersection shortcuts
static scene_intersection intersect_scene(const trace_bvh& bvh,
    const scene_data& scene, const ray3f& ray, bool find_any = false) {
  if (bvh.ebvh.ebvh) {
    return intersect_scene_ebvh(bvh.ebvh, scene, ray, find_any);
  } else {
    return intersect_scene_bvh(bvh.bvh, scene, ray, find_any);
  }
}
static scene_intersection intersect_instance(const trace_bvh& bvh,
    const scene_data& scene, int instance, const ray3f& ray,
    bool find_any = false) {
  if (bvh.ebvh.ebvh) {
    return intersect_instance_ebvh(bvh.ebvh, scene, instance, ray, find_any);
  } else {
    return intersect_instance_bvh(bvh.bvh, scene, instance, ray, find_any);
  }
}

}  // namespace yocto

// -----------------------------------------------------------------------------
// IMPLEMENTATION FOR PATH TRACING
// -----------------------------------------------------------------------------
namespace yocto {

// Evaluates/sample the BRDF scaled by the cosine of the incoming direction.
static vec3f eval_emission(
    const material_point& material, vec3f normal, vec3f outgoing) {
  return dot(normal, outgoing) >= 0 ? material.emission : vec3f{0, 0, 0};
}

// Evaluates/sample the BRDF scaled by the cosine of the incoming direction.
static vec3f eval_bsdfcos(const material_point& material, vec3f normal,
    vec3f outgoing, vec3f incoming) {
  if (material.roughness == 0) return {0, 0, 0};

  if (material.type == material_type::matte) {
    return eval_matte(material.color, normal, outgoing, incoming);
  } else if (material.type == material_type::glossy) {
    return eval_glossy(material.color, material.ior, material.roughness, normal,
        outgoing, incoming);
  } else if (material.type == material_type::reflective) {
    return eval_reflective(
        material.color, material.roughness, normal, outgoing, incoming);
  } else if (material.type == material_type::transparent) {
    return eval_transparent(material.color, material.ior, material.roughness,
        normal, outgoing, incoming);
  } else if (material.type == material_type::refractive) {
    return eval_refractive(material.color, material.ior, material.roughness,
        normal, outgoing, incoming);
  } else if (material.type == material_type::subsurface) {
    return eval_refractive(material.color, material.ior, material.roughness,
        normal, outgoing, incoming);
  } else if (material.type == material_type::gltfpbr) {
    return eval_gltfpbr(material.color, material.ior, material.roughness,
        material.metallic, normal, outgoing, incoming);
  } else {
    return {0, 0, 0};
  }
}

static vec3f eval_delta(const material_point& material, vec3f normal,
    vec3f outgoing, vec3f incoming) {
  if (material.roughness != 0) return {0, 0, 0};

  if (material.type == material_type::reflective) {
    return eval_reflective(material.color, normal, outgoing, incoming);
  } else if (material.type == material_type::transparent) {
    return eval_transparent(
        material.color, material.ior, normal, outgoing, incoming);
  } else if (material.type == material_type::refractive) {
    return eval_refractive(
        material.color, material.ior, normal, outgoing, incoming);
  } else if (material.type == material_type::volumetric) {
    return eval_passthrough(material.color, normal, outgoing, incoming);
  } else {
    return {0, 0, 0};
  }
}

// Picks a direction based on the BRDF
static vec3f sample_bsdfcos(const material_point& material, vec3f normal,
    vec3f outgoing, float rnl, vec2f rn) {
  if (material.roughness == 0) return {0, 0, 0};

  if (material.type == material_type::matte) {
    return sample_matte(material.color, normal, outgoing, rn);
  } else if (material.type == material_type::glossy) {
    return sample_glossy(material.color, material.ior, material.roughness,
        normal, outgoing, rnl, rn);
  } else if (material.type == material_type::reflective) {
    return sample_reflective(
        material.color, material.roughness, normal, outgoing, rn);
  } else if (material.type == material_type::transparent) {
    return sample_transparent(material.color, material.ior, material.roughness,
        normal, outgoing, rnl, rn);
  } else if (material.type == material_type::refractive) {
    return sample_refractive(material.color, material.ior, material.roughness,
        normal, outgoing, rnl, rn);
  } else if (material.type == material_type::subsurface) {
    return sample_refractive(material.color, material.ior, material.roughness,
        normal, outgoing, rnl, rn);
  } else if (material.type == material_type::gltfpbr) {
    return sample_gltfpbr(material.color, material.ior, material.roughness,
        material.metallic, normal, outgoing, rnl, rn);
  } else {
    return {0, 0, 0};
  }
}

static vec3f sample_delta(
    const material_point& material, vec3f normal, vec3f outgoing, float rnl) {
  if (material.roughness != 0) return {0, 0, 0};

  if (material.type == material_type::reflective) {
    return sample_reflective(material.color, normal, outgoing);
  } else if (material.type == material_type::transparent) {
    return sample_transparent(
        material.color, material.ior, normal, outgoing, rnl);
  } else if (material.type == material_type::refractive) {
    return sample_refractive(
        material.color, material.ior, normal, outgoing, rnl);
  } else if (material.type == material_type::volumetric) {
    return sample_passthrough(material.color, normal, outgoing);
  } else {
    return {0, 0, 0};
  }
}

// Compute the weight for sampling the BRDF
static float sample_bsdfcos_pdf(const material_point& material, vec3f normal,
    vec3f outgoing, vec3f incoming) {
  if (material.roughness == 0) return 0;

  if (material.type == material_type::matte) {
    return sample_matte_pdf(material.color, normal, outgoing, incoming);
  } else if (material.type == material_type::glossy) {
    return sample_glossy_pdf(material.color, material.ior, material.roughness,
        normal, outgoing, incoming);
  } else if (material.type == material_type::reflective) {
    return sample_reflective_pdf(
        material.color, material.roughness, normal, outgoing, incoming);
  } else if (material.type == material_type::transparent) {
    return sample_tranparent_pdf(material.color, material.ior,
        material.roughness, normal, outgoing, incoming);
  } else if (material.type == material_type::refractive) {
    return sample_refractive_pdf(material.color, material.ior,
        material.roughness, normal, outgoing, incoming);
  } else if (material.type == material_type::subsurface) {
    return sample_refractive_pdf(material.color, material.ior,
        material.roughness, normal, outgoing, incoming);
  } else if (material.type == material_type::gltfpbr) {
    return sample_gltfpbr_pdf(material.color, material.ior, material.roughness,
        material.metallic, normal, outgoing, incoming);
  } else {
    return 0;
  }
}

static float sample_delta_pdf(const material_point& material, vec3f normal,
    vec3f outgoing, vec3f incoming) {
  if (material.roughness != 0) return 0;

  if (material.type == material_type::reflective) {
    return sample_reflective_pdf(material.color, normal, outgoing, incoming);
  } else if (material.type == material_type::transparent) {
    return sample_tranparent_pdf(
        material.color, material.ior, normal, outgoing, incoming);
  } else if (material.type == material_type::refractive) {
    return sample_refractive_pdf(
        material.color, material.ior, normal, outgoing, incoming);
  } else if (material.type == material_type::volumetric) {
    return sample_passthrough_pdf(material.color, normal, outgoing, incoming);
  } else {
    return 0;
  }
}

static vec3f eval_scattering(
    const material_point& material, vec3f outgoing, vec3f incoming) {
  if (material.density == vec3f{0, 0, 0}) return {0, 0, 0};
  return material.scattering * material.density *
         eval_phasefunction(material.scanisotropy, outgoing, incoming);
}

static vec3f sample_scattering(
    const material_point& material, vec3f outgoing, float rnl, vec2f rn) {
  if (material.density == vec3f{0, 0, 0}) return {0, 0, 0};
  return sample_phasefunction(material.scanisotropy, outgoing, rn);
}

static float sample_scattering_pdf(
    const material_point& material, vec3f outgoing, vec3f incoming) {
  if (material.density == vec3f{0, 0, 0}) return 0;
  return sample_phasefunction_pdf(material.scanisotropy, outgoing, incoming);
}

// Sample camera
static ray3f sample_camera(const camera_data& camera, vec2i ij,
    vec2i image_size, vec2f puv, vec2f luv, bool tent) {
  if (!tent) {
    auto uv = vec2f{
        (ij.x + puv.x) / image_size.x, (ij.y + puv.y) / image_size.y};
    return eval_camera(camera, uv, sample_disk(luv));
  } else {
    const auto width  = 2.0f;
    const auto offset = 0.5f;
    auto       fuv =
        width *
            vec2f{
                puv.x < 0.5f ? sqrt(2 * puv.x) - 1 : 1 - sqrt(2 - 2 * puv.x),
                puv.y < 0.5f ? sqrt(2 * puv.y) - 1 : 1 - sqrt(2 - 2 * puv.y),
            } +
        offset;
    auto uv = vec2f{
        (ij.x + fuv.x) / image_size.x, (ij.y + fuv.y) / image_size.y};
    return eval_camera(camera, uv, sample_disk(luv));
  }
}

// Sample lights wrt solid angle
static vec3f sample_lights(const scene_data& scene, const trace_lights& lights,
    vec3f position, float rl, float rel, vec2f ruv) {
  auto  light_id = sample_uniform((int)lights.lights.size(), rl);
  auto& light    = lights.lights[light_id];
  if (light.instance != invalidid) {
    auto& instance  = scene.instances[light.instance];
    auto& shape     = scene.shapes[instance.shape];
    auto  element   = sample_discrete(light.elements_cdf, rel);
    auto  uv        = (!shape.triangles.empty()) ? sample_triangle(ruv) : ruv;
    auto  lposition = eval_position(scene, instance, element, uv);
    return normalize(lposition - position);
  } else if (light.environment != invalidid) {
    auto& environment = scene.environments[light.environment];
    if (environment.emission_tex != invalidid) {
      auto& texture = scene.textures[environment.emission_tex];
      auto  idx     = sample_discrete(light.elements_cdf, rel);
      auto  size    = max(texture.pixelsf.size(), texture.pixelsb.size());
      auto  uv      = vec2f{
          ((idx % size.x) + 0.5f) / size.x, ((idx / size.x) + 0.5f) / size.y};
      return transform_direction(environment.frame,
          {cos(uv.x * 2 * pif) * sin(uv.y * pif), cos(uv.y * pif),
              sin(uv.x * 2 * pif) * sin(uv.y * pif)});
    } else {
      return sample_sphere(ruv);
    }
  } else {
    return {0, 0, 0};
  }
}

// Sample lights pdf
static float sample_lights_pdf(const scene_data& scene, const trace_bvh& bvh,
    const trace_lights& lights, vec3f position, vec3f direction) {
  auto pdf = 0.0f;
  for (auto& light : lights.lights) {
    if (light.instance != invalidid) {
      auto& instance = scene.instances[light.instance];
      // check all intersection
      auto lpdf          = 0.0f;
      auto next_position = position;
      for (auto bounce = 0; bounce < 100; bounce++) {
        auto intersection = intersect_instance(
            bvh, scene, light.instance, {next_position, direction});
        if (!intersection.hit) break;
        // accumulate pdf
        auto lposition = eval_position(
            scene, instance, intersection.element, intersection.uv);
        auto lnormal = eval_element_normal(
            scene, instance, intersection.element);
        // prob triangle * area triangle = area triangle mesh
        auto area = light.elements_cdf.back();
        lpdf += distance_squared(lposition, position) /
                (abs(dot(lnormal, direction)) * area);
        // continue
        next_position = lposition + direction * 1e-3f;
      }
      pdf += lpdf;
    } else if (light.environment != invalidid) {
      auto& environment = scene.environments[light.environment];
      if (environment.emission_tex != invalidid) {
        auto& emission_tex = scene.textures[environment.emission_tex];
        auto  wl = transform_direction(inverse(environment.frame), direction);
        auto  texcoord = vec2f{atan2(wl.z, wl.x) / (2 * pif),
            acos(clamp(wl.y, -1.0f, 1.0f)) / pif};
        if (texcoord.x < 0) texcoord.x += 1;
        auto size = max(
            emission_tex.pixelsf.size(), emission_tex.pixelsb.size());
        auto ij   = clamp((vec2i)(texcoord * (vec2f)size), zero2i, size - 1);
        auto prob = sample_discrete_pdf(
                        light.elements_cdf, ij.y * size.x + ij.x) /
                    light.elements_cdf.back();
        auto angle = (2 * pif / size.x) * (pif / size.y) *
                     sin(pif * (ij.y + 0.5f) / size.y);
        pdf += prob / angle;
      } else {
        pdf += 1 / (4 * pif);
      }
    }
  }
  pdf *= sample_uniform_pdf((int)lights.lights.size());
  return pdf;
}

struct trace_result {
  vec3f radiance = {0, 0, 0};
  bool  hit      = false;
  vec3f albedo   = {0, 0, 0};
  vec3f normal   = {0, 0, 0};
};

// Recursive path tracing.
static trace_result trace_path(const scene_data& scene, const trace_bvh& bvh,
    const trace_lights& lights, const ray3f& ray_, rng_state& rng,
    const trace_params& params) {
  // initialize
  auto radiance      = vec3f{0, 0, 0};
  auto weight        = vec3f{1, 1, 1};
  auto ray           = ray_;
  auto volume_stack  = vector<material_point>{};
  auto max_roughness = 0.0f;
  auto hit           = false;
  auto hit_albedo    = vec3f{0, 0, 0};
  auto hit_normal    = vec3f{0, 0, 0};
  auto opbounce      = 0;

  // trace  path
  for (auto bounce = 0; bounce < params.bounces; bounce++) {
    // intersect next point
    auto intersection = intersect_scene(bvh, scene, ray);
    if (!intersection.hit) {
      if (bounce > 0 || !params.envhidden)
        radiance += weight * eval_environment(scene, ray.d);
      break;
    }

    // handle transmission if inside a volume
    auto in_volume = false;
    if (!volume_stack.empty()) {
      auto& vsdf     = volume_stack.back();
      auto  distance = sample_transmittance(
          vsdf.density, intersection.distance, rand1f(rng), rand1f(rng));
      weight *= eval_transmittance(vsdf.density, distance) /
                sample_transmittance_pdf(
                    vsdf.density, distance, intersection.distance);
      in_volume             = distance < intersection.distance;
      intersection.distance = distance;
    }

    // switch between surface and volume
    if (!in_volume) {
      // prepare shading point
      auto outgoing = -ray.d;
      auto position = eval_shading_position(scene, intersection, outgoing);
      auto normal   = eval_shading_normal(scene, intersection, outgoing);
      auto material = eval_material(scene, intersection);

      // correct roughness
      if (params.nocaustics) {
        max_roughness      = max(material.roughness, max_roughness);
        material.roughness = max_roughness;
      }

      // handle opacity
      if (material.opacity < 1 && rand1f(rng) >= material.opacity) {
        if (opbounce++ > 128) break;
        ray = {position + ray.d * 1e-2f, ray.d};
        bounce -= 1;
        continue;
      }

      // set hit variables
      if (bounce == 0) {
        hit        = true;
        hit_albedo = material.color;
        hit_normal = normal;
      }

      // accumulate emission
      radiance += weight * eval_emission(material, normal, outgoing);

      // next direction
      auto incoming = vec3f{0, 0, 0};
      if (!is_delta(material)) {
        if (rand1f(rng) < 0.5f) {
          incoming = sample_bsdfcos(
              material, normal, outgoing, rand1f(rng), rand2f(rng));
        } else {
          incoming = sample_lights(
              scene, lights, position, rand1f(rng), rand1f(rng), rand2f(rng));
        }
        if (incoming == vec3f{0, 0, 0}) break;
        weight *=
            eval_bsdfcos(material, normal, outgoing, incoming) /
            (0.5f * sample_bsdfcos_pdf(material, normal, outgoing, incoming) +
                0.5f *
                    sample_lights_pdf(scene, bvh, lights, position, incoming));
      } else {
        incoming = sample_delta(material, normal, outgoing, rand1f(rng));
        weight *= eval_delta(material, normal, outgoing, incoming) /
                  sample_delta_pdf(material, normal, outgoing, incoming);
      }

      // update volume stack
      if (is_volumetric(scene, intersection) &&
          dot(normal, outgoing) * dot(normal, incoming) < 0) {
        if (volume_stack.empty()) {
          auto material = eval_material(scene, intersection);
          volume_stack.push_back(material);
        } else {
          volume_stack.pop_back();
        }
      }

      // setup next iteration
      ray = {position, incoming};
    } else {
      // prepare shading point
      auto  outgoing = -ray.d;
      auto  position = ray.o + ray.d * intersection.distance;
      auto& vsdf     = volume_stack.back();

      // accumulate emission
      // radiance += weight * eval_volemission(emission, outgoing);

      // next direction
      auto incoming = vec3f{0, 0, 0};
      if (rand1f(rng) < 0.5f) {
        incoming = sample_scattering(vsdf, outgoing, rand1f(rng), rand2f(rng));
      } else {
        incoming = sample_lights(
            scene, lights, position, rand1f(rng), rand1f(rng), rand2f(rng));
      }
      if (incoming == vec3f{0, 0, 0}) break;
      weight *=
          eval_scattering(vsdf, outgoing, incoming) /
          (0.5f * sample_scattering_pdf(vsdf, outgoing, incoming) +
              0.5f * sample_lights_pdf(scene, bvh, lights, position, incoming));

      // setup next iteration
      ray = {position, incoming};
    }

    // check weight
    if (weight == vec3f{0, 0, 0} || !isfinite(weight)) break;

    // russian roulette
    if (bounce > 3) {
      auto rr_prob = min((float)0.99, max(weight));
      if (rand1f(rng) >= rr_prob) break;
      weight *= 1 / rr_prob;
    }
  }

  return {radiance, hit, hit_albedo, hit_normal};
}

// Recursive path tracing.
static trace_result trace_pathdirect(const scene_data& scene,
    const trace_bvh& bvh, const trace_lights& lights, const ray3f& ray_,
    rng_state& rng, const trace_params& params) {
  // initialize
  auto radiance      = vec3f{0, 0, 0};
  auto weight        = vec3f{1, 1, 1};
  auto ray           = ray_;
  auto volume_stack  = vector<material_point>{};
  auto max_roughness = 0.0f;
  auto hit           = false;
  auto hit_albedo    = vec3f{0, 0, 0};
  auto hit_normal    = vec3f{0, 0, 0};
  auto next_emission = true;
  auto opbounce      = 0;

  // trace  path
  for (auto bounce = 0; bounce < params.bounces; bounce++) {
    // intersect next point
    auto intersection = intersect_scene(bvh, scene, ray);
    if (!intersection.hit) {
      if ((bounce > 0 || !params.envhidden) && next_emission)
        radiance += weight * eval_environment(scene, ray.d);
      break;
    }

    // handle transmission if inside a volume
    auto in_volume = false;
    if (!volume_stack.empty()) {
      auto& vsdf     = volume_stack.back();
      auto  distance = sample_transmittance(
          vsdf.density, intersection.distance, rand1f(rng), rand1f(rng));
      weight *= eval_transmittance(vsdf.density, distance) /
                sample_transmittance_pdf(
                    vsdf.density, distance, intersection.distance);
      in_volume             = distance < intersection.distance;
      intersection.distance = distance;
    }

    // switch between surface and volume
    if (!in_volume) {
      // prepare shading point
      auto outgoing = -ray.d;
      auto position = eval_shading_position(scene, intersection, outgoing);
      auto normal   = eval_shading_normal(scene, intersection, outgoing);
      auto material = eval_material(scene, intersection);

      // correct roughness
      if (params.nocaustics) {
        max_roughness      = max(material.roughness, max_roughness);
        material.roughness = max_roughness;
      }

      // handle opacity
      if (material.opacity < 1 && rand1f(rng) >= material.opacity) {
        if (opbounce++ > 128) break;
        ray = {position + ray.d * 1e-2f, ray.d};
        bounce -= 1;
        continue;
      }

      // set hit variables
      if (bounce == 0) {
        hit        = true;
        hit_albedo = material.color;
        hit_normal = normal;
      }

      // accumulate emission
      if (next_emission)
        radiance += weight * eval_emission(material, normal, outgoing);

      // direct
      if (!is_delta(material)) {
        auto incoming = sample_lights(
            scene, lights, position, rand1f(rng), rand1f(rng), rand2f(rng));
        auto pdf = sample_lights_pdf(scene, bvh, lights, position, incoming);
        auto bsdfcos = eval_bsdfcos(material, normal, outgoing, incoming);
        if (bsdfcos != vec3f{0, 0, 0} && pdf > 0) {
          auto intersection = intersect_scene(bvh, scene, {position, incoming});
          auto emission =
              !intersection.hit
                  ? eval_environment(scene, incoming)
                  : eval_emission(eval_material(scene,
                                      scene.instances[intersection.instance],
                                      intersection.element, intersection.uv),
                        eval_shading_normal(scene,
                            scene.instances[intersection.instance],
                            intersection.element, intersection.uv, -incoming),
                        -incoming);
          radiance += weight * bsdfcos * emission / pdf;
        }
        next_emission = false;
      } else {
        next_emission = true;
      }

      // next direction
      auto incoming = vec3f{0, 0, 0};
      if (!is_delta(material)) {
        if (rand1f(rng) < 0.5f) {
          incoming = sample_bsdfcos(
              material, normal, outgoing, rand1f(rng), rand2f(rng));
        } else {
          incoming = sample_lights(
              scene, lights, position, rand1f(rng), rand1f(rng), rand2f(rng));
        }
        if (incoming == vec3f{0, 0, 0}) break;
        weight *=
            eval_bsdfcos(material, normal, outgoing, incoming) /
            (0.5f * sample_bsdfcos_pdf(material, normal, outgoing, incoming) +
                0.5f *
                    sample_lights_pdf(scene, bvh, lights, position, incoming));
      } else {
        incoming = sample_delta(material, normal, outgoing, rand1f(rng));
        if (incoming == vec3f{0, 0, 0}) break;
        weight *= eval_delta(material, normal, outgoing, incoming) /
                  sample_delta_pdf(material, normal, outgoing, incoming);
      }

      // update volume stack
      if (is_volumetric(scene, intersection) &&
          dot(normal, outgoing) * dot(normal, incoming) < 0) {
        if (volume_stack.empty()) {
          auto material = eval_material(scene, intersection);
          volume_stack.push_back(material);
        } else {
          volume_stack.pop_back();
        }
      }

      // setup next iteration
      ray = {position, incoming};
    } else {
      // prepare shading point
      auto  outgoing = -ray.d;
      auto  position = ray.o + ray.d * intersection.distance;
      auto& vsdf     = volume_stack.back();

      // next direction
      auto incoming = vec3f{0, 0, 0};
      if (rand1f(rng) < 0.5f) {
        incoming = sample_scattering(vsdf, outgoing, rand1f(rng), rand2f(rng));
      } else {
        incoming = sample_lights(
            scene, lights, position, rand1f(rng), rand1f(rng), rand2f(rng));
      }
      if (incoming == vec3f{0, 0, 0}) break;
      weight *=
          eval_scattering(vsdf, outgoing, incoming) /
          (0.5f * sample_scattering_pdf(vsdf, outgoing, incoming) +
              0.5f * sample_lights_pdf(scene, bvh, lights, position, incoming));

      // setup next iteration
      ray = {position, incoming};
    }

    // check weight
    if (weight == vec3f{0, 0, 0} || !isfinite(weight)) break;

    // russian roulette
    if (bounce > 3) {
      auto rr_prob = min((float)0.99, max(weight));
      if (rand1f(rng) >= rr_prob) break;
      weight *= 1 / rr_prob;
    }
  }

  return {radiance, hit, hit_albedo, hit_normal};
}

// Recursive path tracing with MIS.
static trace_result trace_pathmis(const scene_data& scene, const trace_bvh& bvh,
    const trace_lights& lights, const ray3f& ray_, rng_state& rng,
    const trace_params& params) {
  // initialize
  auto radiance      = vec3f{0, 0, 0};
  auto weight        = vec3f{1, 1, 1};
  auto ray           = ray_;
  auto volume_stack  = vector<material_point>{};
  auto max_roughness = 0.0f;
  auto hit           = false;
  auto hit_albedo    = vec3f{0, 0, 0};
  auto hit_normal    = vec3f{0, 0, 0};
  auto opbounce      = 0;

  // MIS helpers
  auto mis_heuristic = [](float this_pdf, float other_pdf) {
    return (this_pdf * this_pdf) /
           (this_pdf * this_pdf + other_pdf * other_pdf);
  };
  auto next_emission     = true;
  auto next_intersection = scene_intersection{};

  // trace  path
  for (auto bounce = 0; bounce < params.bounces; bounce++) {
    // intersect next point
    auto intersection = next_emission ? intersect_scene(bvh, scene, ray)
                                      : next_intersection;
    if (!intersection.hit) {
      if ((bounce > 0 || !params.envhidden) && next_emission)
        radiance += weight * eval_environment(scene, ray.d);
      break;
    }

    // handle transmission if inside a volume
    auto in_volume = false;
    if (!volume_stack.empty()) {
      auto& vsdf     = volume_stack.back();
      auto  distance = sample_transmittance(
          vsdf.density, intersection.distance, rand1f(rng), rand1f(rng));
      weight *= eval_transmittance(vsdf.density, distance) /
                sample_transmittance_pdf(
                    vsdf.density, distance, intersection.distance);
      in_volume             = distance < intersection.distance;
      intersection.distance = distance;
    }

    // switch between surface and volume
    if (!in_volume) {
      // prepare shading point
      auto outgoing = -ray.d;
      auto position = eval_shading_position(scene, intersection, outgoing);
      auto normal   = eval_shading_normal(scene, intersection, outgoing);
      auto material = eval_material(scene, intersection);

      // correct roughness
      if (params.nocaustics) {
        max_roughness      = max(material.roughness, max_roughness);
        material.roughness = max_roughness;
      }

      // handle opacity
      if (material.opacity < 1 && rand1f(rng) >= material.opacity) {
        if (opbounce++ > 128) break;
        ray = {position + ray.d * 1e-2f, ray.d};
        bounce -= 1;
        continue;
      }

      // set hit variables
      if (bounce == 0) {
        hit        = true;
        hit_albedo = material.color;
        hit_normal = normal;
      }

      // accumulate emission
      if (next_emission) {
        radiance += weight * eval_emission(material, normal, outgoing);
      }

      // next direction
      auto incoming = vec3f{0, 0, 0};
      if (!is_delta(material)) {
        // direct with MIS --- light
        for (auto sample_light : {true, false}) {
          incoming = sample_light ? sample_lights(scene, lights, position,
                                        rand1f(rng), rand1f(rng), rand2f(rng))
                                  : sample_bsdfcos(material, normal, outgoing,
                                        rand1f(rng), rand2f(rng));
          if (incoming == vec3f{0, 0, 0}) break;
          auto bsdfcos   = eval_bsdfcos(material, normal, outgoing, incoming);
          auto light_pdf = sample_lights_pdf(
              scene, bvh, lights, position, incoming);
          auto bsdf_pdf = sample_bsdfcos_pdf(
              material, normal, outgoing, incoming);
          auto mis_weight = sample_light
                                ? mis_heuristic(light_pdf, bsdf_pdf) / light_pdf
                                : mis_heuristic(bsdf_pdf, light_pdf) / bsdf_pdf;
          if (bsdfcos != vec3f{0, 0, 0} && mis_weight != 0) {
            auto intersection = intersect_scene(
                bvh, scene, {position, incoming});
            if (!sample_light) next_intersection = intersection;
            auto emission = vec3f{0, 0, 0};
            if (!intersection.hit) {
              emission = eval_environment(scene, incoming);
            } else {
              auto material = eval_material(scene,
                  scene.instances[intersection.instance], intersection.element,
                  intersection.uv);
              emission      = eval_emission(material,
                       eval_shading_normal(scene,
                           scene.instances[intersection.instance],
                           intersection.element, intersection.uv, -incoming),
                       -incoming);
            }
            radiance += weight * bsdfcos * emission * mis_weight;
          }
        }

        // indirect
        weight *= eval_bsdfcos(material, normal, outgoing, incoming) /
                  sample_bsdfcos_pdf(material, normal, outgoing, incoming);
        next_emission = false;
      } else {
        incoming = sample_delta(material, normal, outgoing, rand1f(rng));
        weight *= eval_delta(material, normal, outgoing, incoming) /
                  sample_delta_pdf(material, normal, outgoing, incoming);
        next_emission = true;
      }

      // update volume stack
      if (is_volumetric(scene, intersection) &&
          dot(normal, outgoing) * dot(normal, incoming) < 0) {
        if (volume_stack.empty()) {
          auto material = eval_material(scene, intersection);
          volume_stack.push_back(material);
        } else {
          volume_stack.pop_back();
        }
      }

      // setup next iteration
      ray = {position, incoming};
    } else {
      // prepare shading point
      auto  outgoing = -ray.d;
      auto  position = ray.o + ray.d * intersection.distance;
      auto& vsdf     = volume_stack.back();

      // next direction
      auto incoming = vec3f{0, 0, 0};
      if (rand1f(rng) < 0.5f) {
        incoming = sample_scattering(vsdf, outgoing, rand1f(rng), rand2f(rng));
        next_emission = true;
      } else {
        incoming = sample_lights(
            scene, lights, position, rand1f(rng), rand1f(rng), rand2f(rng));
        next_emission = true;
      }
      weight *=
          eval_scattering(vsdf, outgoing, incoming) /
          (0.5f * sample_scattering_pdf(vsdf, outgoing, incoming) +
              0.5f * sample_lights_pdf(scene, bvh, lights, position, incoming));

      // setup next iteration
      ray = {position, incoming};
    }

    // check weight
    if (weight == vec3f{0, 0, 0} || !isfinite(weight)) break;

    // russian roulette
    if (bounce > 3) {
      auto rr_prob = min((float)0.99, max(weight));
      if (rand1f(rng) >= rr_prob) break;
      weight *= 1 / rr_prob;
    }
  }

  return {radiance, hit, hit_albedo, hit_normal};
}

// Recursive path tracing.
static trace_result trace_pathtest(const scene_data& scene,
    const trace_bvh& bvh, const trace_lights& lights, const ray3f& ray_,
    rng_state& rng, const trace_params& params) {
  // initialize
  auto radiance      = vec3f{0, 0, 0};
  auto weight        = vec3f{1, 1, 1};
  auto ray           = ray_;
  auto max_roughness = 0.0f;
  auto hit           = false;
  auto hit_albedo    = vec3f{0, 0, 0};
  auto hit_normal    = vec3f{0, 0, 0};
  auto opbounce      = 0;

  // trace  path
  for (auto bounce = 0; bounce < params.bounces; bounce++) {
    // intersect next point
    auto intersection = intersect_scene(bvh, scene, ray);
    if (!intersection.hit) {
      if (bounce > 0 || !params.envhidden)
        radiance += weight * eval_environment(scene, ray.d);
      break;
    }

    // prepare shading point
    auto outgoing = -ray.d;
    auto position = eval_shading_position(scene, intersection, outgoing);
    auto normal   = eval_shading_normal(scene, intersection, outgoing);
    auto material = eval_material(scene, intersection);
    material.type = material_type::matte;

    // set hit variables
    if (bounce == 0) {
      hit        = true;
      hit_albedo = material.color;
      hit_normal = normal;
    }

    // accumulate emission
    radiance += weight * eval_emission(material, normal, outgoing);

    // next direction
    auto incoming = vec3f{0, 0, 0};
    if (!is_delta(material)) {
      if (rand1f(rng) < 0.5f) {
        incoming = sample_bsdfcos(
            material, normal, outgoing, rand1f(rng), rand2f(rng));
      } else {
        incoming = sample_lights(
            scene, lights, position, rand1f(rng), rand1f(rng), rand2f(rng));
      }
      if (incoming == vec3f{0, 0, 0}) break;
      weight *=
          eval_bsdfcos(material, normal, outgoing, incoming) /
          (0.5f * sample_bsdfcos_pdf(material, normal, outgoing, incoming) +
              0.5f * sample_lights_pdf(scene, bvh, lights, position, incoming));
    } else {
      incoming = sample_delta(material, normal, outgoing, rand1f(rng));
      weight *= eval_delta(material, normal, outgoing, incoming) /
                sample_delta_pdf(material, normal, outgoing, incoming);
    }

    // setup next iteration
    ray = {position, incoming};

    // check weight
    if (weight == vec3f{0, 0, 0} || !isfinite(weight)) break;

    // russian roulette
    if (bounce > 3) {
      auto rr_prob = min((float)0.99, max(weight));
      if (rand1f(rng) >= rr_prob) break;
      weight *= 1 / rr_prob;
    }
  }

  return {radiance, hit, hit_albedo, hit_normal};
}

// Recursive path tracing by sampling lights.
static trace_result trace_lightsampling(const scene_data& scene,
    const trace_bvh& bvh, const trace_lights& lights, const ray3f& ray_,
    rng_state& rng, const trace_params& params) {
  // initialize
  auto radiance      = vec3f{0, 0, 0};
  auto weight        = vec3f{1, 1, 1};
  auto ray           = ray_;
  auto hit           = false;
  auto hit_albedo    = vec3f{0, 0, 0};
  auto hit_normal    = vec3f{0, 0, 0};
  auto next_emission = true;
  auto opbounce      = 0;

  // trace  path
  for (auto bounce = 0; bounce < params.bounces; bounce++) {
    // intersect next point
    auto intersection = intersect_scene(bvh, scene, ray);
    if (!intersection.hit) {
      if ((bounce > 0 || !params.envhidden) && next_emission)
        radiance += weight * eval_environment(scene, ray.d);
      break;
    }

    // prepare shading point
    auto outgoing = -ray.d;
    auto position = eval_shading_position(scene, intersection, outgoing);
    auto normal   = eval_shading_normal(scene, intersection, outgoing);
    auto material = eval_material(scene, intersection);

    // handle opacity
    if (material.opacity < 1 && rand1f(rng) >= material.opacity) {
      if (opbounce++ > 128) break;
      ray = {position + ray.d * 1e-2f, ray.d};
      bounce -= 1;
      continue;
    }

    // set hit variables
    if (bounce == 0) {
      hit        = true;
      hit_albedo = material.color;
      hit_normal = normal;
    }

    // accumulate emission
    if (next_emission)
      radiance += weight * eval_emission(material, normal, outgoing);

    // direct
    if (!is_delta(material)) {
      auto incoming = sample_lights(
          scene, lights, position, rand1f(rng), rand1f(rng), rand2f(rng));
      auto pdf     = sample_lights_pdf(scene, bvh, lights, position, incoming);
      auto bsdfcos = eval_bsdfcos(material, normal, outgoing, incoming);
      if (bsdfcos != vec3f{0, 0, 0} && pdf > 0) {
        auto intersection = intersect_scene(bvh, scene, {position, incoming});
        auto emission =
            !intersection.hit
                ? eval_environment(scene, incoming)
                : eval_emission(eval_material(scene,
                                    scene.instances[intersection.instance],
                                    intersection.element, intersection.uv),
                      eval_shading_normal(scene,
                          scene.instances[intersection.instance],
                          intersection.element, intersection.uv, -incoming),
                      -incoming);
        radiance += weight * bsdfcos * emission / pdf;
      }
      next_emission = false;
    } else {
      next_emission = true;
    }

    // next direction
    auto incoming = vec3f{0, 0, 0};
    if (material.roughness != 0) {
      incoming = sample_bsdfcos(
          material, normal, outgoing, rand1f(rng), rand2f(rng));
      if (incoming == vec3f{0, 0, 0}) break;
      weight *= eval_bsdfcos(material, normal, outgoing, incoming) /
                sample_bsdfcos_pdf(material, normal, outgoing, incoming);
    } else {
      incoming = sample_delta(material, normal, outgoing, rand1f(rng));
      if (incoming == vec3f{0, 0, 0}) break;
      weight *= eval_delta(material, normal, outgoing, incoming) /
                sample_delta_pdf(material, normal, outgoing, incoming);
    }

    // check weight
    if (weight == vec3f{0, 0, 0} || !isfinite(weight)) break;

    // russian roulette
    if (bounce > 3) {
      auto rr_prob = min((float)0.99, max(weight));
      if (rand1f(rng) >= rr_prob) break;
      weight *= 1 / rr_prob;
    }

    // setup next iteration
    ray = {position, incoming};
  }

  return {radiance, hit, hit_albedo, hit_normal};
}

// Recursive path tracing.
static trace_result trace_naive(const scene_data& scene, const trace_bvh& bvh,
    const trace_lights& lights, const ray3f& ray_, rng_state& rng,
    const trace_params& params) {
  // initialize
  auto radiance   = vec3f{0, 0, 0};
  auto weight     = vec3f{1, 1, 1};
  auto ray        = ray_;
  auto hit        = false;
  auto hit_albedo = vec3f{0, 0, 0};
  auto hit_normal = vec3f{0, 0, 0};
  auto opbounce   = 0;

  // trace  path
  for (auto bounce = 0; bounce < params.bounces; bounce++) {
    // intersect next point
    auto intersection = intersect_scene(bvh, scene, ray);
    if (!intersection.hit) {
      if (bounce > 0 || !params.envhidden)
        radiance += weight * eval_environment(scene, ray.d);
      break;
    }

    // prepare shading point
    auto outgoing = -ray.d;
    auto position = eval_shading_position(scene, intersection, outgoing);
    auto normal   = eval_shading_normal(scene, intersection, outgoing);
    auto material = eval_material(scene, intersection);

    // handle opacity
    if (material.opacity < 1 && rand1f(rng) >= material.opacity) {
      if (opbounce++ > 128) break;
      ray = {position + ray.d * 1e-2f, ray.d};
      bounce -= 1;
      continue;
    }

    // set hit variables
    if (bounce == 0) {
      hit        = true;
      hit_albedo = material.color;
      hit_normal = normal;
    }

    // accumulate emission
    radiance += weight * eval_emission(material, normal, outgoing);

    // next direction
    auto incoming = vec3f{0, 0, 0};
    if (!is_delta(material)) {
      incoming = sample_bsdfcos(
          material, normal, outgoing, rand1f(rng), rand2f(rng));
      if (incoming == vec3f{0, 0, 0}) break;
      weight *= eval_bsdfcos(material, normal, outgoing, incoming) /
                sample_bsdfcos_pdf(material, normal, outgoing, incoming);
    } else {
      incoming = sample_delta(material, normal, outgoing, rand1f(rng));
      if (incoming == vec3f{0, 0, 0}) break;
      weight *= eval_delta(material, normal, outgoing, incoming) /
                sample_delta_pdf(material, normal, outgoing, incoming);
    }

    // check weight
    if (weight == vec3f{0, 0, 0} || !isfinite(weight)) break;

    // russian roulette
    if (bounce > 3) {
      auto rr_prob = min((float)0.99, max(weight));
      if (rand1f(rng) >= rr_prob) break;
      weight *= 1 / rr_prob;
    }

    // setup next iteration
    ray = {position, incoming};
  }

  return {radiance, hit, hit_albedo, hit_normal};
}

// Eyelight for quick previewing.
static trace_result trace_eyelight(const scene_data& scene,
    const trace_bvh& bvh, const trace_lights& lights, const ray3f& ray_,
    rng_state& rng, const trace_params& params) {
  // initialize
  auto radiance   = vec3f{0, 0, 0};
  auto weight     = vec3f{1, 1, 1};
  auto ray        = ray_;
  auto hit        = false;
  auto hit_albedo = vec3f{0, 0, 0};
  auto hit_normal = vec3f{0, 0, 0};
  auto opbounce   = 0;

  // trace  path
  for (auto bounce = 0; bounce < max(params.bounces, 4); bounce++) {
    // intersect next point
    auto intersection = intersect_scene(bvh, scene, ray);
    if (!intersection.hit) {
      if (bounce > 0 || !params.envhidden)
        radiance += weight * eval_environment(scene, ray.d);
      break;
    }

    // prepare shading point
    auto outgoing = -ray.d;
    auto position = eval_shading_position(scene, intersection, outgoing);
    auto normal   = eval_shading_normal(scene, intersection, outgoing);
    auto material = eval_material(scene, intersection);

    // handle opacity
    if (material.opacity < 1 && rand1f(rng) >= material.opacity) {
      if (opbounce++ > 128) break;
      ray = {position + ray.d * 1e-2f, ray.d};
      bounce -= 1;
      continue;
    }

    // set hit variables
    if (bounce == 0) {
      hit        = true;
      hit_albedo = material.color;
      hit_normal = normal;
    }

    // accumulate emission
    auto incoming = outgoing;
    radiance += weight * eval_emission(material, normal, outgoing);

    // brdf * light
    radiance += weight * pif *
                eval_bsdfcos(material, normal, outgoing, incoming);

    // continue path
    if (!is_delta(material)) break;
    incoming = sample_delta(material, normal, outgoing, rand1f(rng));
    if (incoming == vec3f{0, 0, 0}) break;
    weight *= eval_delta(material, normal, outgoing, incoming) /
              sample_delta_pdf(material, normal, outgoing, incoming);
    if (weight == vec3f{0, 0, 0} || !isfinite(weight)) break;

    // setup next iteration
    ray = {position, incoming};
  }

  return {radiance, hit, hit_albedo, hit_normal};
}

// Furnace test.
static trace_result trace_furnace(const scene_data& scene, const trace_bvh& bvh,
    const trace_lights& lights, const ray3f& ray_, rng_state& rng,
    const trace_params& params) {
  // initialize
  auto radiance   = vec3f{0, 0, 0};
  auto weight     = vec3f{1, 1, 1};
  auto ray        = ray_;
  auto hit        = false;
  auto hit_albedo = vec3f{0, 0, 0};
  auto hit_normal = vec3f{0, 0, 0};
  auto opbounce   = 0;
  auto in_volume  = false;

  // trace  path
  for (auto bounce = 0; bounce < params.bounces; bounce++) {
    // exit loop
    if (bounce > 0 && !in_volume) {
      radiance += weight * eval_environment(scene, ray.d);
      break;
    }

    // intersect next point
    auto intersection = intersect_scene(bvh, scene, ray);
    if (!intersection.hit) {
      if (bounce > 0 || !params.envhidden)
        radiance += weight * eval_environment(scene, ray.d);
      break;
    }

    // prepare shading point
    auto  outgoing = -ray.d;
    auto& instance = scene.instances[intersection.instance];
    auto  element  = intersection.element;
    auto  uv       = intersection.uv;
    auto  position = eval_position(scene, instance, element, uv);
    auto  normal = eval_shading_normal(scene, instance, element, uv, outgoing);
    auto  material = eval_material(scene, instance, element, uv);

    // handle opacity
    if (material.opacity < 1 && rand1f(rng) >= material.opacity) {
      if (opbounce++ > 128) break;
      ray = {position + ray.d * 1e-2f, ray.d};
      bounce -= 1;
      continue;
    }

    // set hit variables
    if (bounce == 0) {
      hit        = true;
      hit_albedo = material.color;
      hit_normal = normal;
    }

    // accumulate emission
    radiance += weight * eval_emission(material, normal, outgoing);

    // next direction
    auto incoming = vec3f{0, 0, 0};
    if (material.roughness != 0) {
      incoming = sample_bsdfcos(
          material, normal, outgoing, rand1f(rng), rand2f(rng));
      if (incoming == vec3f{0, 0, 0}) break;
      weight *= eval_bsdfcos(material, normal, outgoing, incoming) /
                sample_bsdfcos_pdf(material, normal, outgoing, incoming);
    } else {
      incoming = sample_delta(material, normal, outgoing, rand1f(rng));
      if (incoming == vec3f{0, 0, 0}) break;
      weight *= eval_delta(material, normal, outgoing, incoming) /
                sample_delta_pdf(material, normal, outgoing, incoming);
    }

    // check weight
    if (weight == vec3f{0, 0, 0} || !isfinite(weight)) break;

    // russian roulette
    if (bounce > 3) {
      auto rr_prob = min((float)0.99, max(weight));
      if (rand1f(rng) >= rr_prob) break;
      weight *= 1 / rr_prob;
    }

    // update volume stack
    if (dot(normal, outgoing) * dot(normal, incoming) < 0)
      in_volume = !in_volume;

    // setup next iteration
    ray = {position, incoming};
  }

  // done
  return {radiance, hit, hit_albedo, hit_normal};
}

// False color rendering
static trace_result trace_falsecolor(const scene_data& scene,
    const trace_bvh& bvh, const trace_lights& lights, const ray3f& ray,
    rng_state& rng, const trace_params& params) {
  // intersect next point
  auto intersection = intersect_scene(bvh, scene, ray);
  if (!intersection.hit) return {};

  // prepare shading point
  auto outgoing = -ray.d;
  auto position = eval_shading_position(scene, intersection, outgoing);
  auto normal   = eval_shading_normal(scene, intersection, outgoing);
  auto gnormal  = eval_element_normal(scene, intersection);
  auto texcoord = eval_texcoord(scene, intersection);
  auto material = eval_material(scene, intersection);
  auto delta    = is_delta(material) ? 1.0f : 0.0f;

  // hash color
  auto hashed_color = [](int id) {
    auto hashed = std::hash<int>()(id);
    auto rng    = make_rng(trace_default_seed, hashed);
    return pow(0.5f + 0.5f * rand3f(rng), 2.2f);
  };

  // compute result
  auto result = vec3f{0, 0, 0};
  switch (params.falsecolor) {
    case trace_falsecolor_type::position:
      result = position * 0.5f + 0.5f;
      break;
    case trace_falsecolor_type::normal: result = normal * 0.5f + 0.5f; break;
    case trace_falsecolor_type::frontfacing:
      result = dot(normal, -ray.d) > 0 ? vec3f{0, 1, 0} : vec3f{1, 0, 0};
      break;
    case trace_falsecolor_type::gnormal: result = gnormal * 0.5f + 0.5f; break;
    case trace_falsecolor_type::gfrontfacing:
      result = dot(gnormal, -ray.d) > 0 ? vec3f{0, 1, 0} : vec3f{1, 0, 0};
      break;
    case trace_falsecolor_type::mtype:
      result = hashed_color((int)material.type);
      break;
    case trace_falsecolor_type::texcoord:
      result = {fmod(texcoord.x, 1.0f), fmod(texcoord.y, 1.0f), 0};
      break;
    case trace_falsecolor_type::color: result = material.color; break;
    case trace_falsecolor_type::emission: result = material.emission; break;
    case trace_falsecolor_type::roughness:
      result = {material.roughness, material.roughness, material.roughness};
      break;
    case trace_falsecolor_type::opacity:
      result = {material.opacity, material.opacity, material.opacity};
      break;
    case trace_falsecolor_type::metallic:
      result = {material.metallic, material.metallic, material.metallic};
      break;
    case trace_falsecolor_type::delta: result = {delta, delta, delta}; break;
    case trace_falsecolor_type::element:
      result = hashed_color(intersection.element);
      break;
    case trace_falsecolor_type::instance:
      result = hashed_color(intersection.instance);
      break;
    case trace_falsecolor_type::shape:
      result = hashed_color(scene.instances[intersection.instance].shape);
      break;
    case trace_falsecolor_type::material:
      result = hashed_color(scene.instances[intersection.instance].material);
      break;
    case trace_falsecolor_type::highlight: {
      if (material.emission == vec3f{0, 0, 0})
        material.emission = {0.2f, 0.2f, 0.2f};
      result = material.emission * abs(dot(-ray.d, normal));
      break;
    } break;
    default: result = {0, 0, 0};
  }

  // done
  return {srgb_to_rgb(result), true, material.color, normal};
}

// Trace a single ray from the camera using the given algorithm.
using sampler_func = trace_result (*)(const scene_data& scene,
    const trace_bvh& bvh, const trace_lights& lights, const ray3f& ray,
    rng_state& rng, const trace_params& params);
static sampler_func get_trace_sampler_func(const trace_params& params) {
  switch (params.sampler) {
    case trace_sampler_type::path: return trace_path;
    case trace_sampler_type::pathdirect: return trace_pathdirect;
    case trace_sampler_type::pathmis: return trace_pathmis;
    case trace_sampler_type::pathtest: return trace_pathtest;
    case trace_sampler_type::lightsampling: return trace_lightsampling;
    case trace_sampler_type::naive: return trace_naive;
    case trace_sampler_type::eyelight: return trace_eyelight;
    case trace_sampler_type::furnace: return trace_furnace;
    case trace_sampler_type::falsecolor: return trace_falsecolor;
    default: {
      throw std::runtime_error("sampler unknown");
      return nullptr;
    }
  }
}

// Check is a sampler requires lights
bool is_sampler_lit(const trace_params& params) {
  switch (params.sampler) {
    case trace_sampler_type::path: return true;
    case trace_sampler_type::pathdirect: return true;
    case trace_sampler_type::pathmis: return true;
    case trace_sampler_type::naive: return true;
    case trace_sampler_type::eyelight: return false;
    case trace_sampler_type::furnace: return true;
    case trace_sampler_type::falsecolor: return false;
    default: {
      throw std::runtime_error("sampler unknown");
      return false;
    }
  }
}

// Trace a block of samples
void trace_sample(trace_state& state, const scene_data& scene,
    const trace_bvh& bvh, const trace_lights& lights, vec2i ij, int sample,
    const trace_params& params) {
  auto& camera  = scene.cameras[params.camera];
  auto  sampler = get_trace_sampler_func(params);
  auto  ray     = sample_camera(camera, ij, state.render.size(),
           rand2f(state.rngs[ij]), rand2f(state.rngs[ij]), params.tentfilter);
  auto [radiance, hit, albedo, normal] = sampler(
      scene, bvh, lights, ray, state.rngs[ij], params);
  if (!isfinite(radiance)) radiance = {0, 0, 0};
  if (max(radiance) > params.clamp)
    radiance = radiance * (params.clamp / max(radiance));
  auto weight = 1.0f / (sample + 1);
  if (hit) {
    state.render[ij] = lerp(
        state.render[ij], {radiance.x, radiance.y, radiance.z, 1}, weight);
    state.albedo[ij] = lerp(state.albedo[ij], albedo, weight);
    state.normal[ij] = lerp(state.normal[ij], normal, weight);
    state.hits[ij] += 1;
  } else if (!params.envhidden && !scene.environments.empty()) {
    state.render[ij] = lerp(
        state.render[ij], {radiance.x, radiance.y, radiance.z, 1}, weight);
    state.albedo[ij] = lerp(state.albedo[ij], {1, 1, 1}, weight);
    state.normal[ij] = lerp(state.normal[ij], -ray.d, weight);
    state.hits[ij] += 1;
  } else {
    state.render[ij] = lerp(state.render[ij], {0, 0, 0, 0}, weight);
    state.albedo[ij] = lerp(state.albedo[ij], {0, 0, 0}, weight);
    state.normal[ij] = lerp(state.normal[ij], -ray.d, weight);
  }
}

// Init a sequence of random number generators.
trace_state make_trace_state(
    const scene_data& scene, const trace_params& params) {
  auto& camera     = scene.cameras[params.camera];
  auto  state      = trace_state{};
  auto  resolution = (camera.aspect >= 1)
                         ? vec2i{params.resolution,
                              (int)round(params.resolution / camera.aspect)}
                         : vec2i{(int)round(params.resolution * camera.aspect),
                              params.resolution};
  state.samples    = 0;
  state.render     = image<vec4f>{resolution};
  state.albedo     = image<vec3f>{resolution};
  state.normal     = image<vec3f>{resolution};
  state.hits       = image<int>{resolution};
  state.rngs       = image<rng_state>{resolution};
  auto rng_        = make_rng(1301081);
  for (auto& rng : state.rngs) {
    rng = make_rng(params.seed, rand1i(rng_, 1 << 31) / 2 + 1);
  }
  if (params.denoise) {
    state.denoised = image<vec4f>{resolution};
  }
  return state;
}

// Forward declaration
static trace_light& add_light(trace_lights& lights) {
  return lights.lights.emplace_back();
}

// Init trace lights
trace_lights make_trace_lights(
    const scene_data& scene, const trace_params& params) {
  auto lights = trace_lights{};

  for (auto handle : range(scene.instances.size())) {
    auto& instance = scene.instances[handle];
    auto& material = scene.materials[instance.material];
    if (material.emission == vec3f{0, 0, 0}) continue;
    auto& shape = scene.shapes[instance.shape];
    if (shape.triangles.empty() && shape.quads.empty()) continue;
    auto& light       = add_light(lights);
    light.instance    = (int)handle;
    light.environment = invalidid;
    if (!shape.triangles.empty()) {
      light.elements_cdf = vector<float>(shape.triangles.size());
      for (auto idx : range(light.elements_cdf.size())) {
        auto& t                 = shape.triangles[idx];
        light.elements_cdf[idx] = triangle_area(
            shape.positions[t.x], shape.positions[t.y], shape.positions[t.z]);
        if (idx != 0) light.elements_cdf[idx] += light.elements_cdf[idx - 1];
      }
    }
    if (!shape.quads.empty()) {
      light.elements_cdf = vector<float>(shape.quads.size());
      for (auto idx : range(light.elements_cdf.size())) {
        auto& t                 = shape.quads[idx];
        light.elements_cdf[idx] = quad_area(shape.positions[t.x],
            shape.positions[t.y], shape.positions[t.z], shape.positions[t.w]);
        if (idx != 0) light.elements_cdf[idx] += light.elements_cdf[idx - 1];
      }
    }
  }
  for (auto handle : range(scene.environments.size())) {
    auto& environment = scene.environments[handle];
    if (environment.emission == vec3f{0, 0, 0}) continue;
    auto& light       = add_light(lights);
    light.instance    = invalidid;
    light.environment = (int)handle;
    if (environment.emission_tex != invalidid) {
      auto& texture      = scene.textures[environment.emission_tex];
      auto  size         = max(texture.pixelsf.size(), texture.pixelsb.size());
      light.elements_cdf = vector<float>(size.x * size.y);
      for (auto idx : range(light.elements_cdf.size())) {
        auto ij                 = vec2i{(int)idx % size.x, (int)idx / size.x};
        auto th                 = (ij.y + 0.5f) * pif / size.y;
        auto value              = lookup_texture(texture, ij);
        light.elements_cdf[idx] = max(value) * sin(th);
        if (idx != 0) light.elements_cdf[idx] += light.elements_cdf[idx - 1];
      }
    }
  }

  // handle progress
  return lights;
}

// Convenience helper
image<vec4f> trace_image(const scene_data& scene, trace_sampler_type type,
    int resolution, int samples, int bounces) {
  auto params       = trace_params{};
  params.sampler    = type;
  params.resolution = resolution;
  params.samples    = samples;
  params.bounces    = bounces;
  auto bvh          = make_trace_bvh(scene, params);
  auto lights       = make_trace_lights(scene, params);
  auto state        = make_trace_state(scene, params);
  for (auto sample = 0; sample < params.samples; sample++) {
    trace_samples(state, scene, bvh, lights, params);
  }
  return get_image(state);
}

// Progressively computes an image.
image<vec4f> trace_image(const scene_data& scene, const trace_params& params) {
  auto bvh    = make_trace_bvh(scene, params);
  auto lights = make_trace_lights(scene, params);
  auto state  = make_trace_state(scene, params);
  for (auto sample = 0; sample < params.samples; sample++) {
    trace_samples(state, scene, bvh, lights, params);
  }
  return get_image(state);
}

// Progressively compute an image by calling trace_samples multiple times.
void trace_samples(trace_state& state, const scene_data& scene,
    const trace_bvh& bvh, const trace_lights& lights,
    const trace_params& params) {
  if (state.samples >= params.samples) return;
  if (params.noparallel) {
    for (auto ij : range(state.render.size())) {
      for (auto sample : range(state.samples, state.samples + params.batch)) {
        trace_sample(state, scene, bvh, lights, ij, sample, params);
      }
    }
  } else {
    parallel_for_batch(state.render.size(), [&](vec2i ij) {
      for (auto sample : range(state.samples, state.samples + params.batch)) {
        trace_sample(state, scene, bvh, lights, ij, sample, params);
      }
    });
  }
  state.samples += params.batch;
  if (params.denoise && !state.denoised.empty()) {
    denoise_image(state.denoised, state.render, state.albedo, state.normal);
  }
}

// Trace context
trace_context make_trace_context(const trace_params& params) {
  return {{}, false, false};
}

// Async start
void trace_start(trace_context& context, trace_state& state,
    const scene_data& scene, const trace_bvh& bvh, const trace_lights& lights,
    const trace_params& params) {
  if (state.samples >= params.samples) return;
  context.stop   = false;
  context.done   = false;
  context.worker = std::async(std::launch::async, [&]() {
    if (context.stop) return;
    parallel_for_batch(state.render.size(), [&](vec2i ij) {
      for (auto sample : range(state.samples, state.samples + params.batch)) {
        if (context.stop) return;
        trace_sample(state, scene, bvh, lights, ij, sample, params);
      }
    });
    state.samples += params.batch;
    if (context.stop) return;
    if (params.denoise && !state.denoised.empty()) {
      denoise_image(state.denoised, state.render, state.albedo, state.normal);
    }
    context.done = true;
  });
}

// Async cancel
void trace_cancel(trace_context& context) {
  context.stop = true;
  if (context.worker.valid()) context.worker.get();
}

// Async done
bool trace_done(const trace_context& context) { return context.done; }

void trace_preview(image<vec4f>& image, trace_context& context,
    trace_state& state, const scene_data& scene, const trace_bvh& bvh,
    const trace_lights& lights, const trace_params& params) {
  // preview
  auto pparams = params;
  pparams.resolution /= params.pratio;
  pparams.samples = 1;
  auto pstate     = make_trace_state(scene, pparams);
  trace_samples(pstate, scene, bvh, lights, pparams);
  auto preview = get_image(pstate);
  for (auto ij : range(state.size())) {
    auto pij  = clamp(ij / params.pratio, vec2i{0, 0}, preview.size() - 1);
    image[ij] = preview[pij];
  }
};

// Check image type
template <typename T>
static void check_image(const image<T>& image, vec2i size) {
  if (image.size() != size)
    throw std::invalid_argument{"image should have the same size"};
}

// Get resulting render, denoised if requested
image<vec4f> get_image(const trace_state& state) {
  auto render = image<vec4f>{state.render.size()};
  get_image(render, state);
  return render;
}
void get_image(image<vec4f>& render, const trace_state& state) {
  check_image(render, state.render.size());
  if (state.denoised.empty()) {
    render = state.render;
  } else {
    render = state.denoised;
  }
}

// Get resulting render
image<vec4f> get_rendered_image(const trace_state& state) {
  return state.render;
}
void get_rendered_image(image<vec4f>& image, const trace_state& state) {
  check_image(image, state.render.size());
  image = state.render;
}

// Get denoised render
image<vec4f> get_denoised_image(const trace_state& state) {
  auto image = state.render;
  get_denoised_image(image, state);
  return image;
}
void get_denoised_image(image<vec4f>& image, const trace_state& state) {
#if YOCTO_DENOISE
  // Create an Intel Open Image Denoise device
  oidn::DeviceRef device = oidn::newDevice();
  device.commit();

  // get image
  get_rendered_image(image, state);
  auto& albedo = state.albedo;
  auto& normal = state.normal;

  // Create a denoising filter
  oidn::FilterRef filter = device.newFilter("RT");  // ray tracing filter
  filter.setImage("color", (void*)image.data(), oidn::Format::Float3,
      state.size().x, state.size().y, 0, sizeof(vec4f),
      sizeof(vec4f) * state.size().x);
  filter.setImage("albedo", (void*)albedo.data(), oidn::Format::Float3,
      state.size().x, state.size().y);
  filter.setImage("normal", (void*)normal.data(), oidn::Format::Float3,
      state.size().x, state.size().y);
  filter.setImage("output", image.data(), oidn::Format::Float3, state.size().x,
      state.size().y, 0, sizeof(vec4f), sizeof(vec4f) * state.size().x);
  filter.set("inputScale", 1.0f);  // set scale as fixed
  filter.set("hdr", true);         // image is HDR
  filter.commit();

  // Filter the image
  filter.execute();
#else
  get_rendered_image(image, state);
#endif
}

// Get denoising buffers
image<vec3f> get_albedo_image(const trace_state& state) { return state.albedo; }
void         get_albedo_image(image<vec3f>& albedo, const trace_state& state) {
  albedo = state.albedo;
}
image<vec3f> get_normal_image(const trace_state& state) { return state.normal; }
void         get_normal_image(image<vec3f>& normal, const trace_state& state) {
  normal = state.normal;
}

// Denoise image
image<vec4f> denoise_image(const image<vec4f>& render,
    const image<vec3f>& albedo, const image<vec3f>& normal) {
  auto denoised = render;
  denoise_image(denoised, render, albedo, normal);
  return denoised;
}
void denoise_image(image<vec4f>& denoised, const image<vec4f>& render,
    const image<vec3f>& albedo, const image<vec3f>& normal) {
  check_image(denoised, render.size());
  check_image(albedo, render.size());
  check_image(normal, render.size());
#if YOCTO_DENOISE
  // set image
  denoised = render;

  // Create an Intel Open Image Denoise device
  oidn::DeviceRef device = oidn::newDevice(OIDN_DEVICE_TYPE_DEFAULT);
  device.commit();

  // check error
  const char* errorMessage;
  if (device.getError(errorMessage) != oidn::Error::None) {
    printf("Error: %s\n", errorMessage);
    return;
  }

  // width and height
  auto [width, height] = render.size();

  // Create a denoising filter
  oidn::FilterRef filter = device.newFilter("RT");  // ray tracing filter
  filter.setImage("color", (void*)render.data(), oidn::Format::Float3, width,
      height, 0, sizeof(vec4f), sizeof(vec4f) * render.size().x);
  filter.setImage("albedo", (void*)albedo.data(), oidn::Format::Float3, width,
      height, 0, sizeof(vec3f), sizeof(vec3f) * render.size().x);
  filter.setImage("normal", (void*)normal.data(), oidn::Format::Float3, width,
      height, 0, sizeof(vec3f), sizeof(vec3f) * render.size().x);
  filter.setImage("output", denoised.data(), oidn::Format::Float3, width,
      height, 0, sizeof(vec4f), sizeof(vec4f) * render.size().x);
  filter.set("inputScale", 1.0f);  // set scale as fixed
  filter.set("hdr", true);         // image is HDR
  filter.commit();

  // check error
  if (device.getError(errorMessage) != oidn::Error::None) {
    printf("Error: %s\n", errorMessage);
    return;
  }

  // Filter the image
  filter.execute();

  // check error
  if (device.getError(errorMessage) != oidn::Error::None) {
    printf("Error: %s\n", errorMessage);
    return;
  }

#else
  denoised = render;
#endif
}

}  // namespace yocto
