// Screen-space reflections at full or half resolution (fx_effect_scale): the
// view ray is reflected about each surface's normal (from the depth) and
// marched through the depth buffer, refined by a binary search where it
// passes behind a surface. The reflected light is the previous frame's scene
// before the effects, reprojected - the guest renders the screen in tiles, so
// this frame's isn't complete yet - weighted by Fresnel (surfaces reflect most
// at grazing angles). Fades out toward the screen's edges and with distance,
// where nothing is known. Output: rgb = reflected light, a = confidence.
//
// Water (scene_fx_water_reflections): the game tells the height of the water
// around the camera, so wherever the view ray crosses that plane before
// reaching the surface in the depth buffer (the water may or may not write
// depth), it reflects about the plane's normal, rippled, and what it misses
// reflects the sky. Intensities are applied here: the composite adds as is.

#include "scene_fx_common.hlsli"

// Half resolution, 4 MIPs.
FX_BINDING(0) Texture2D<float> fx_view_depth : register(t0);
// Full resolution.
FX_BINDING(1) Texture2D<float> fx_depth : register(t1);
// Half resolution, the guest's scene before the effects, previous frame.
FX_BINDING(2) Texture2D<float4> fx_previous_color : register(t2);
// 1x1 sky color (a = valid).
FX_BINDING(3) Texture2D<float4> fx_sky : register(t3);
FX_BINDING(4) SamplerState fx_clamp : register(s0);
FX_EXTRA_CONSTANTS_BLOCK(5) {
  // View space position this frame to the previous frame's clip space.
  float4 fx_ssr_reproject[4];
  // xyz = world up in view space, w = 1 to reflect only on floors.
  float4 fx_ssr_up;
  // x = max distance, y = step count, z = thickness, w = Fresnel reflectance.
  float4 fx_ssr_params;
  // x = water level relative to the camera (world units along up), y = water
  // reflection strength (0 = no water), z = intensity of the other
  // reflections (0 = water only), w = ripple time (seconds).
  float4 fx_ssr_water;
  // The world's horizontal axes in view space (xyz) and the camera's
  // position along them (w), for the ripples.
  float4 fx_ssr_water_u;
  float4 fx_ssr_water_v;
};
FX_BINDING(6) FX_FORMAT_RGBA16F RWTexture2D<float4> fx_reflections : register(u0);

float FxEffectDistance(int2 texel) {
  texel = clamp(texel, int2(0, 0), FxEffectSize(fx_effect_scale) - 1);
  return fx_effect_scale == 1u ? fx_depth.Load(int3(texel, 0))
                               : fx_view_depth.Load(int3(texel, 0));
}

float3 FxEffectPosition(int2 texel) {
  texel = clamp(texel, int2(0, 0), FxEffectSize(fx_effect_scale) - 1);
  return FxViewPosition(FxEffectScreen(texel, fx_effect_scale), FxEffectDistance(texel));
}

// Screen position of a view space position (z > 0).
float2 FxProject(float3 position) {
  float2 ndc = position.xy / (position.z * float2(fx_inv_p00, fx_inv_p11));
  return (ndc * float2(0.5, -0.5) + 0.5) * float2(fx_scene_size);
}

float FxSceneDistance(float2 screen) {
  int2 pixel = clamp(int2(screen), int2(0, 0), int2(fx_scene_size) - 1);
  return fx_depth.Load(int3(pixel, 0));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(int2(id.xy) >= FxEffectSize(fx_effect_scale))) {
    return;
  }
  int2 texel = int2(id.xy);
  float3 position = FxEffectPosition(texel);
  bool is_sky = position.z >= fx_sky_distance;
  float3 view = normalize(position);
  float3 up_axis = fx_ssr_up.xyz;
  // Water: where the view ray crosses the water's plane, from above, before
  // it reaches the surface the depth has.
  bool water = false;
  float3 normal;
  float reflectance = fx_ssr_params.w;
  float strength = fx_ssr_water.z;
  if (fx_ssr_water.y > 0.0 && fx_ssr_water.x < 0.0) {
    float descent = dot(view, up_axis);
    float crossing = descent < -1.0e-3 ? fx_ssr_water.x / descent : -1.0;
    float surface = length(position);
    if (crossing > 0.0 && crossing <= surface * 1.01 + 1.0) {
      water = true;
      position = view * crossing;
      // Two small wave trains across the world's horizontal axes.
      float2 world = float2(dot(position, fx_ssr_water_u.xyz) + fx_ssr_water_u.w,
                            dot(position, fx_ssr_water_v.xyz) + fx_ssr_water_v.w);
      float time = fx_ssr_water.w;
      float2 slope = 0.012 * float2(cos(world.x * 0.031 + time * 1.7) +
                                        0.5 * cos(world.y * 0.047 - time * 1.3 + world.x * 0.02),
                                    cos(world.y * 0.029 + time * 1.5) +
                                        0.5 * cos(world.x * 0.043 + time * 1.1 - world.y * 0.017));
      normal = normalize(up_axis + slope.x * fx_ssr_water_u.xyz + slope.y * fx_ssr_water_v.xyz);
      // Water's Fresnel reflectance at normal incidence (0.02), raised: the
      // game's own water texture covers much of it.
      reflectance = 0.05;
      strength = fx_ssr_water.y;
    }
  }
  if (!water) {
    if (strength <= 0.0 || is_sky) {
      fx_reflections[texel] = 0.0;
      return;
    }
    // Normal from the depth, per axis from the side with the smaller depth
    // step.
    float3 right = FxEffectPosition(texel + int2(1, 0)) - position;
    float3 left = position - FxEffectPosition(texel - int2(1, 0));
    float3 down = FxEffectPosition(texel + int2(0, 1)) - position;
    float3 up = position - FxEffectPosition(texel - int2(0, 1));
    normal = normalize(cross(abs(down.z) < abs(up.z) ? down : up,
                             abs(right.z) < abs(left.z) ? right : left));
    if (dot(normal, view) > 0.0) {
      normal = -normal;
    }
    if (fx_ssr_up.w > 0.0 && dot(normal, up_axis) < 0.7) {
      fx_reflections[texel] = 0.0;
      return;
    }
  }
  float3 direction = reflect(view, normal);
  // Schlick's Fresnel.
  float fresnel = reflectance + (1.0 - reflectance) * pow(1.0 - saturate(dot(-view, normal)), 5.0);
  // The sky, for water rays that hit nothing (going up, nothing above).
  float4 sky = fx_sky.Load(int3(0, 0, 0));
  float3 sky_light = water && sky.a > 0.0 ? sky.rgb * fresnel * strength : 0.0;
  // Toward the camera: whatever it would hit is mostly off screen.
  if (direction.z < -0.2 || fresnel < 0.01) {
    fx_reflections[texel] = float4(sky_light, 0.0);
    return;
  }

  float max_distance = fx_ssr_params.x;
  uint step_count = max(uint(fx_ssr_params.y), 1u);
  float thickness = fx_ssr_params.z;
  float jitter = FxNoise(float2(texel));
  float previous_t = 0.0;
  float hit_t = -1.0;
  for (uint i = 0; i < step_count; ++i) {
    // Finer steps near the surface.
    float s = (float(i) + jitter) / float(step_count);
    float t = max_distance * s * s + 0.05;
    float3 sample_position = position + direction * t;
    if (sample_position.z <= 0.05) {
      break;
    }
    float2 screen = FxProject(sample_position);
    if (any(screen < 0.0) || any(screen >= float2(fx_scene_size))) {
      break;
    }
    float behind = sample_position.z - FxSceneDistance(screen);
    if (behind > 0.0 && behind < thickness + 0.02 * t) {
      // Refine between the last step in front and this one.
      float low = previous_t, high = t;
      [unroll] for (int refine = 0; refine < 5; ++refine) {
        float middle = 0.5 * (low + high);
        float3 middle_position = position + direction * middle;
        if (middle_position.z - FxSceneDistance(FxProject(middle_position)) > 0.0) {
          high = middle;
        } else {
          low = middle;
        }
      }
      hit_t = high;
      break;
    }
    previous_t = t;
  }
  if (hit_t < 0.0) {
    fx_reflections[texel] = float4(sky_light, 0.0);
    return;
  }

  // The hit's light last frame.
  float4 p = float4(position + direction * hit_t, 1.0);
  float4 clip = float4(dot(fx_ssr_reproject[0], p), dot(fx_ssr_reproject[1], p),
                       dot(fx_ssr_reproject[2], p), dot(fx_ssr_reproject[3], p));
  if (clip.w <= 1.0e-4) {
    fx_reflections[texel] = float4(sky_light, 0.0);
    return;
  }
  float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
  float2 edge = min(uv, 1.0 - uv);
  float confidence = saturate(min(edge.x, edge.y) * 10.0) * (1.0 - hit_t / max_distance);
  if (confidence <= 0.0) {
    fx_reflections[texel] = float4(sky_light, 0.0);
    return;
  }
  // The scene texture may be larger than the half-resolution scene.
  float2 color_size;
  fx_previous_color.GetDimensions(color_size.x, color_size.y);
  float3 light = fx_previous_color
                     .SampleLevel(fx_clamp, uv * float2(FxEffectSize(2u)) / color_size, 0.0)
                     .rgb;
  // Where the hit fades out (screen edges, distance), water shows the sky.
  fx_reflections[texel] = float4(light * (fresnel * confidence * strength) +
                                     sky_light * (1.0 - confidence),
                                 confidence);
}
