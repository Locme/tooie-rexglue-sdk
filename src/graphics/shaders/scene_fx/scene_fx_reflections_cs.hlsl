// Screen-space reflections at full or half resolution (fx_effect_scale): the
// view ray is reflected about each surface's normal (from the depth) and
// marched through the depth buffer, refined by a binary search where it
// passes behind a surface. The reflected light is the previous frame's scene
// before the effects, reprojected - the guest renders the screen in tiles, so
// this frame's isn't complete yet - weighted by Fresnel (surfaces reflect most
// at grazing angles). Fades out toward the screen's edges and with distance,
// where nothing is known. With the materials tagged (texture_materials), only
// those reflect, each as much as it should (water and ice a lot, stone not at
// all); without, every surface a little. Output: rgb = reflected light, a =
// confidence.

#include "scene_fx_common.hlsli"

// Half resolution, 4 MIPs.
FX_BINDING(0) Texture2D<float> fx_view_depth : register(t0);
// Full resolution.
FX_BINDING(1) Texture2D<float> fx_depth : register(t1);
// Half resolution, the guest's scene before the effects, previous frame.
FX_BINDING(2) Texture2D<float4> fx_previous_color : register(t2);
// Full resolution, TextureCache::Material (0 = unknown).
FX_BINDING(3) Texture2D<float> fx_material : register(t3);
// 1x1, the sky's color.
FX_BINDING(4) Texture2D<float4> fx_sky : register(t4);
FX_BINDING(5) SamplerState fx_clamp : register(s0);
FX_EXTRA_CONSTANTS_BLOCK(6) {
  // View space position this frame to the previous frame's clip space.
  float4 fx_ssr_reproject[4];
  // xyz = world up in view space, w = 1 to reflect only on floors.
  float4 fx_ssr_up;
  // x = max distance, y = step count, z = thickness, w = Fresnel reflectance.
  float4 fx_ssr_params;
  // x = 1 for only the tagged materials to reflect, y = 1 to show the
  // materials instead.
  float4 fx_ssr_materials;
};
FX_BINDING(7) FX_FORMAT_RGBA16F RWTexture2D<float4> fx_reflections : register(u0);

// Per TextureCache::Material: x = reflectance facing it (Fresnel F0), y =
// strength; and its color in the debug view.
static const float2 kFxMaterialReflection[6] = {
    float2(0.0, 0.0),    // unknown
    // Far more than real water's 0.02: added over the game's own color (the
    // composite only adds light), a real-world amount barely shows.
    float2(0.35, 1.0),   // water
    float2(0.25, 1.0),   // ice
    float2(0.6, 0.8),    // metal
    float2(0.2, 1.0),    // glass
    float2(0.1, 0.8),    // polished
};
static const float3 kFxMaterialColor[6] = {
    float3(0.0, 0.0, 0.0), float3(0.1, 0.3, 1.0), float3(0.4, 1.0, 1.0),
    float3(1.0, 0.8, 0.1), float3(0.2, 1.0, 0.3), float3(1.0, 0.2, 1.0),
};

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
  float reflectance = fx_ssr_params.w;
  float strength = 1.0;
  if (fx_ssr_materials.x > 0.0) {
    int2 pixel = min(int2(FxEffectScreen(texel, fx_effect_scale)), int2(fx_scene_size) - 1);
    uint material = min(uint(fx_material.Load(int3(pixel, 0)) + 0.5), 5u);
    if (fx_ssr_materials.y > 0.0) {
      fx_reflections[texel] = float4(kFxMaterialColor[material], 1.0);
      return;
    }
    reflectance = kFxMaterialReflection[material].x;
    strength = kFxMaterialReflection[material].y;
    if (strength <= 0.0) {
      fx_reflections[texel] = 0.0;
      return;
    }
  }
  float3 position = FxEffectPosition(texel);
  if (position.z >= fx_sky_distance) {
    fx_reflections[texel] = 0.0;
    return;
  }
  // Normal from the depth, per axis from the side with the smaller depth step.
  float3 right = FxEffectPosition(texel + int2(1, 0)) - position;
  float3 left = position - FxEffectPosition(texel - int2(1, 0));
  float3 down = FxEffectPosition(texel + int2(0, 1)) - position;
  float3 up = position - FxEffectPosition(texel - int2(0, 1));
  float3 normal = normalize(cross(abs(down.z) < abs(up.z) ? down : up,
                                  abs(right.z) < abs(left.z) ? right : left));
  float3 view = normalize(position);
  if (dot(normal, view) > 0.0) {
    normal = -normal;
  }
  if (fx_ssr_up.w > 0.0 && dot(normal, fx_ssr_up.xyz) < 0.7) {
    fx_reflections[texel] = 0.0;
    return;
  }
  float3 direction = reflect(view, normal);
  // Schlick's Fresnel.
  float fresnel = strength * (reflectance + (1.0 - reflectance) *
                                             pow(1.0 - saturate(dot(-view, normal)), 5.0));
  // Toward the camera: whatever it would hit is mostly off screen.
  if (direction.z < -0.2 || fresnel < 0.01) {
    fx_reflections[texel] = 0.0;
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
    // Nothing on the screen: a tagged surface (water, ice) facing up sees the
    // sky, faded in from the horizon.
    float sky_view = fx_ssr_materials.x > 0.0 ? saturate(dot(direction, fx_ssr_up.xyz) * 4.0)
                                              : 0.0;
    float3 sky = fx_sky.Load(int3(0, 0, 0)).rgb;
    fx_reflections[texel] = float4(sky * (fresnel * sky_view), sky_view);
    return;
  }

  // The hit's light last frame.
  float4 p = float4(position + direction * hit_t, 1.0);
  float4 clip = float4(dot(fx_ssr_reproject[0], p), dot(fx_ssr_reproject[1], p),
                       dot(fx_ssr_reproject[2], p), dot(fx_ssr_reproject[3], p));
  if (clip.w <= 1.0e-4) {
    fx_reflections[texel] = 0.0;
    return;
  }
  float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
  float2 edge = min(uv, 1.0 - uv);
  float confidence = saturate(min(edge.x, edge.y) * 10.0) * (1.0 - hit_t / max_distance);
  if (confidence <= 0.0) {
    fx_reflections[texel] = 0.0;
    return;
  }
  // The scene texture may be larger than the half-resolution scene.
  float2 color_size;
  fx_previous_color.GetDimensions(color_size.x, color_size.y);
  float3 light = fx_previous_color
                     .SampleLevel(fx_clamp, uv * float2(FxEffectSize(2u)) / color_size, 0.0)
                     .rgb;
  fx_reflections[texel] = float4(light * (fresnel * confidence), confidence);
}
