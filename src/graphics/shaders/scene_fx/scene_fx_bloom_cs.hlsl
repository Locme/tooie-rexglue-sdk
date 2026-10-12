// Subtle bloom for an 8-bit scene (scene_fx_bloom): the bright parts of the
// image, at a quarter of the screen's resolution, blurred and added back by
// the 16-bit resolve composite. The bright pass is kept in screen space over
// frames, so a tile's blur also sees the tiles around it (this frame's above,
// the previous frame's below) and no band edges show.
//
// - FX_BLOOM_PREFILTER: fx_pass_rect = the tile (render target pixels, of the
//   scene's 16-bit copy); each thread averages a 4x4 block and keeps what is
//   above the threshold (with a soft knee), written at screen / 4.
// - Otherwise (blur): a separable Gaussian along fx_blur_direction over
//   fx_pass_rect (quarter-resolution screen texels).

#include "scene_fx_common.hlsli"

#if defined(FX_BLOOM_PREFILTER)

FX_BINDING(0) Texture2D<float4> fx_image : register(t0);
FX_EXTRA_CONSTANTS_BLOCK(1) {
  // x = threshold, y = soft knee.
  float4 fx_bloom_threshold;
};
FX_BINDING(2) FX_FORMAT_RGBA16F RWTexture2D<float4> fx_bright : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  int2 base = int2(fx_pass_rect.xy) + int2(id.xy) * 4;
  if (any(base >= int2(fx_pass_rect.zw))) {
    return;
  }
  float3 sum = 0.0;
  [unroll] for (int y = 0; y < 4; ++y) {
    [unroll] for (int x = 0; x < 4; ++x) {
      int2 pixel = min(base + int2(x, y), int2(fx_pass_rect.zw) - 1);
      sum += fx_image.Load(int3(pixel, 0)).rgb;
    }
  }
  float3 color = sum / 16.0;
  float threshold = fx_bloom_threshold.x;
  float knee = max(fx_bloom_threshold.y, 1.0e-4);
  float brightness = max(max(color.r, color.g), color.b);
  float soft = clamp(brightness - threshold + knee, 0.0, 2.0 * knee);
  soft = soft * soft / (4.0 * knee);
  float weight = max(soft, brightness - threshold) / max(brightness, 1.0e-4);
  int2 texel = (base + fx_screen_offset) >> 2;
  fx_bright[texel] = float4(color * saturate(weight), 1.0);
}

#else  // blur

FX_BINDING(0) Texture2D<float4> fx_source : register(t0);
FX_BINDING(1) FX_FORMAT_RGBA16F RWTexture2D<float4> fx_dest : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  int2 texel = int2(fx_pass_rect.xy) + int2(id.xy);
  if (any(texel >= int2(fx_pass_rect.zw))) {
    return;
  }
  int2 size = int2((fx_scene_size + 3u) >> 2);
  int2 axis = int2(fx_blur_direction);
  // Sigma 4 texels (16 screen pixels), 12 texels each way.
  float4 sum = 0.0;
  float weight_sum = 0.0;
  [unroll] for (int i = -12; i <= 12; ++i) {
    float weight = exp(-float(i * i) / 32.0);
    int2 tap = clamp(texel + axis * i, int2(0, 0), size - 1);
    sum += fx_source.Load(int3(tap, 0)) * weight;
    weight_sum += weight;
  }
  fx_dest[texel] = sum / weight_sum;
}

#endif
