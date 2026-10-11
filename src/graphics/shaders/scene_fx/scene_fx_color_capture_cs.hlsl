// Captures a tile of a guest color render target (fx_pass_rect in render
// target pixels, at fx_screen_offset on the screen):
// - Default: the HDR scene before the effects at half resolution, for the
//   global illumination and the reflections (as the next frame's light
//   source) and as the surfaces' brightness when compositing. Each texel is the
//   average of its 2x2 scene pixels.
// - FX_FULL: the final image at full resolution, for the image effects.
// Sample 0 of multisampled sources (FX_MSAA), except for the final image,
// which is the average of all samples (sample 0 alone, written back by the
// image effects, would undo the antialiasing).

#include "scene_fx_common.hlsli"

#ifdef FX_MSAA
FX_BINDING(0) Texture2DMS<float4> fx_color : register(t0);
#else
FX_BINDING(0) Texture2D<float4> fx_color : register(t0);
#endif
FX_BINDING(1) FX_FORMAT_RGBA16F RWTexture2D<float4> fx_captured : register(u0);

float3 FxLoadColor(int2 pixel) {
  pixel = clamp(pixel, int2(fx_pass_rect.xy), int2(fx_pass_rect.zw) - 1);
#ifdef FX_MSAA
  float3 color = fx_color.Load(pixel, 0).rgb;
#else
  float3 color = fx_color.Load(int3(pixel, 0)).rgb;
#endif
  // No NaN / infinity from the guest's float formats.
  return (all(color == color) && all(color < 65504.0)) ? max(color, 0.0) : 0.0;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
#ifdef FX_FULL
  int2 pixel = int2(fx_pass_rect.xy) + int2(id.xy);
  if (any(pixel >= int2(fx_pass_rect.zw))) {
    return;
  }
#ifdef FX_MSAA
  uint width, height, sample_count;
  fx_color.GetDimensions(width, height, sample_count);
  float3 sum = 0.0;
  for (uint i = 0u; i < sample_count; ++i) {
    float3 color = fx_color.Load(pixel, int(i)).rgb;
    sum += (all(color == color) && all(color < 65504.0)) ? max(color, 0.0) : 0.0;
  }
  fx_captured[pixel + fx_screen_offset] = float4(sum / float(max(sample_count, 1u)), 1.0);
#else
  fx_captured[pixel + fx_screen_offset] = float4(FxLoadColor(pixel), 1.0);
#endif
#else
  int2 first_texel = (int2(fx_pass_rect.xy) + fx_screen_offset) >> 1;
  int2 texel = first_texel + int2(id.xy);
  int2 pixel = texel * 2 - fx_screen_offset;
  if (any(pixel >= int2(fx_pass_rect.zw)) || any(texel * 2 >= int2(fx_scene_size))) {
    return;
  }
  float3 sum = FxLoadColor(pixel) + FxLoadColor(pixel + int2(1, 0)) +
               FxLoadColor(pixel + int2(0, 1)) + FxLoadColor(pixel + int2(1, 1));
  fx_captured[texel] = float4(sum * 0.25, 1.0);
#endif
}
