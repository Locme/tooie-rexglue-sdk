// Eye adaptation (scene_fx_eye_adaptation): the final image's brightness
// measured per tile, and an exposure correction that follows it over time,
// applied by the image pass (scene_fx_image_ps).
//
// - FX_EXPOSURE_ADAPT: at a frame's first image, the previous frame's tiles
//   (fx_exposure_params.y of them) averaged by their weights into the log
//   average brightness, and the correction (stops, x of the 1x1 target)
//   moved toward what brings it to the key, at the rate for brightening (z)
//   or darkening (w). y of the target = 1 once valid.
// - Otherwise (measure): one 16x16 group samples a 32x32 grid over
//   fx_exposure_rect of the image (display-referred, so taken to linear),
//   writing x = the log2 average brightness, y = the weight (the rectangle's
//   area) to texel fx_exposure_params.x.

#include "scene_fx_common.hlsli"

#if defined(FX_EXPOSURE_ADAPT)

FX_BINDING(0) Texture2D<float4> fx_exposure_tiles : register(t0);
FX_BINDING(1) Texture2D<float4> fx_previous_exposure : register(t1);  // 1x1
#define FX_EXPOSURE_CONSTANTS_BINDING 2
#define FX_EXPOSURE_TARGET_BINDING 3

#else

FX_BINDING(0) Texture2D<float4> fx_image : register(t0);
#define FX_EXPOSURE_CONSTANTS_BINDING 1
#define FX_EXPOSURE_TARGET_BINDING 2

#endif

FX_EXTRA_CONSTANTS_BLOCK(FX_EXPOSURE_CONSTANTS_BINDING) {
  // Measure: left, top, right, bottom in the image's pixels.
  int4 fx_exposure_rect;
  // x = tile texel (measure), y = tiles to average (adapt), z = brightening
  // rate, w = darkening rate.
  float4 fx_exposure_params;
  // x = log2 key, y = strength, z = darkest, w = brightest correction (stops).
  float4 fx_exposure_range;
};

#if defined(FX_EXPOSURE_ADAPT)

FX_BINDING(FX_EXPOSURE_TARGET_BINDING) FX_FORMAT_RGBA16F RWTexture2D<float4> fx_exposure
    : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy != 0u)) {
    return;
  }
  float4 previous = fx_previous_exposure.Load(int3(0, 0, 0));
  float log_sum = 0.0, weight_sum = 0.0;
  uint count = uint(fx_exposure_params.y);
  for (uint i = 0u; i < count; ++i) {
    float4 tile = fx_exposure_tiles.Load(int3(i, 0, 0));
    log_sum += tile.x * tile.y;
    weight_sum += tile.y;
  }
  float4 result = previous;
  if (weight_sum > 0.0) {
    float average = log_sum / weight_sum;
    float target = clamp((fx_exposure_range.x - average) * fx_exposure_range.y,
                         fx_exposure_range.z, fx_exposure_range.w);
    if (previous.y > 0.0) {
      float rate = target < previous.x ? fx_exposure_params.w : fx_exposure_params.z;
      result = float4(lerp(previous.x, target, rate), 1.0, average, 0.0);
    } else {
      result = float4(target, 1.0, average, 0.0);
    }
  }
  fx_exposure[uint2(0, 0)] = result;
}

#else

FX_BINDING(FX_EXPOSURE_TARGET_BINDING) FX_FORMAT_RGBA16F RWTexture2D<float4> fx_exposure_tiles
    : register(u0);

groupshared float fx_log_sums[256];

[numthreads(16, 16, 1)]
void main(uint3 local : SV_GroupThreadID, uint index : SV_GroupIndex) {
  float2 rect_size = float2(fx_exposure_rect.zw - fx_exposure_rect.xy);
  float log_sum = 0.0;
  [unroll] for (uint i = 0u; i < 4u; ++i) {
    float2 grid = float2(local.xy * 2u + uint2(i & 1u, i >> 1u)) + 0.5;
    int2 pixel = fx_exposure_rect.xy + int2(grid / 32.0 * rect_size);
    float3 color = saturate(fx_image.Load(int3(pixel, 0)).rgb);
    // Black isn't infinitely dark: a floor, so a few black pixels don't
    // dominate the log average.
    log_sum += log2(max(FxLuminance(pow(color, 2.2)), 0.002));
  }
  fx_log_sums[index] = log_sum;
  GroupMemoryBarrierWithGroupSync();
  [unroll] for (uint stride = 128u; stride > 0u; stride >>= 1u) {
    if (index < stride) {
      fx_log_sums[index] += fx_log_sums[index + stride];
    }
    GroupMemoryBarrierWithGroupSync();
  }
  if (index == 0u) {
    // Weight: the area in units of 64x64 pixels (fits half floats).
    fx_exposure_tiles[uint2(uint(fx_exposure_params.x), 0u)] =
        float4(fx_log_sums[0] / 1024.0, rect_size.x * rect_size.y / 4096.0, 0.0, 1.0);
  }
}

#endif
