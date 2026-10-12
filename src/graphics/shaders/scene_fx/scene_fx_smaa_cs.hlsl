// SMAA 1x (Jimenez et al., Enhanced Subpixel Morphological Antialiasing) on
// the final image, before the image effects. Orthogonal patterns with corner
// detection (no diagonals), luma edges with local contrast adaptation. One
// source per pass, all at render target pixels; fx_pass_rect is the tile.
//
// - FX_SMAA_TABLES: writes the area (orthogonal, subsample 0: the top-left
//   80x80 of SMAA's 160x560 AreaTex) and search (64x16) lookup textures from
//   the extra constants, once.
// - FX_SMAA_EDGES: luma edges (r = left, g = top) of the image copy. Run over
//   the tile plus a margin; outside the tile it writes no edges, so the later
//   passes never see another tile's (or frame's) pixels.
// - FX_SMAA_WEIGHTS: blending weights from the edges (same margin).
// - FX_SMAA_BLEND: neighborhood blending of the image copy into the output.

#include "scene_fx_common.hlsli"

#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_HIGH 1
#define SMAA_DISABLE_DIAG_DETECTION 1
#define SMAA_INCLUDE_VS 0
#define SMAATexture2D(tex) Texture2D<float4> tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(fx_sampler, coord, 0.0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(fx_sampler, coord, 0.0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) \
  tex.SampleLevel(fx_sampler, coord, 0.0, offset)
#define SMAASample(tex, coord) tex.SampleLevel(fx_sampler, coord, 0.0)
#define SMAASamplePoint(tex, coord) tex.SampleLevel(fx_sampler, coord, 0.0)
#define SMAASampleOffset(tex, coord, offset) tex.SampleLevel(fx_sampler, coord, 0.0, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
// 1 / size, size of the textures the pass works on (all at render target
// pixels), set first thing in main.
static float4 fx_smaa_metrics;
#define SMAA_RT_METRICS fx_smaa_metrics

// Margin (render target pixels) around the tile the edges and weights cover:
// more than the search distance (2 * SMAA_MAX_SEARCH_STEPS) plus the corner
// and crossing taps.
#define FX_SMAA_MARGIN 40

#if defined(FX_SMAA_TABLES)

// 80x80 area texels (r, g bytes, 2 texels per uint), then 64x16 search bytes
// (4 per uint).
FX_EXTRA_CONSTANTS_BLOCK(0) {
  uint4 fx_smaa_tables[864];
};
FX_BINDING(1) FX_FORMAT_RGBA16F RWTexture2D<float4> fx_smaa_area : register(u0);
FX_BINDING(2) FX_FORMAT_RGBA16F RWTexture2D<float4> fx_smaa_search : register(u1);

uint FxTableWord(uint index) {
  uint4 words = fx_smaa_tables[index >> 2u];
  uint lane = index & 3u;
  return lane == 0u ? words.x : (lane == 1u ? words.y : (lane == 2u ? words.z : words.w));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= 80u || id.y >= 80u) {
    return;
  }
  uint texel = id.y * 80u + id.x;
  uint area = (FxTableWord(texel >> 1u) >> ((texel & 1u) * 16u)) & 0xFFFFu;
  fx_smaa_area[id.xy] = float4(float(area & 0xFFu), float(area >> 8u), 0.0, 0.0) / 255.0;
  if (id.x < 64u && id.y < 16u) {
    uint index = id.y * 64u + id.x;
    uint search = (FxTableWord(3200u + (index >> 2u)) >> ((index & 3u) * 8u)) & 0xFFu;
    fx_smaa_search[id.xy] = float4(float(search) / 255.0, 0.0, 0.0, 0.0);
  }
}

#elif defined(FX_SMAA_EDGES)

FX_BINDING(0) Texture2D<float4> fx_image : register(t0);
FX_BINDING(1) FX_FORMAT_RGBA16F RWTexture2D<float4> fx_edges : register(u0);

float FxLuma(int2 pixel) {
  pixel = clamp(pixel, int2(fx_pass_rect.xy), int2(fx_pass_rect.zw) - 1);
  return dot(fx_image.Load(int3(pixel, 0)).rgb, float3(0.2126, 0.7152, 0.0722));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  int2 pixel = int2(fx_pass_rect.xy) - FX_SMAA_MARGIN + int2(id.xy);
  uint width, height;
  fx_edges.GetDimensions(width, height);
  if (any(pixel < 0) || any(pixel >= int2(width, height)) ||
      any(pixel >= int2(fx_pass_rect.zw) + FX_SMAA_MARGIN)) {
    return;
  }
  float2 edges = 0.0;
  if (all(pixel >= int2(fx_pass_rect.xy)) && all(pixel < int2(fx_pass_rect.zw))) {
    // SMAALumaEdgeDetectionPS (SMAA_PRESET_HIGH: threshold 0.1, local
    // contrast adaptation factor 2), with loads clamped to the tile.
    float l = FxLuma(pixel);
    float l_left = FxLuma(pixel + int2(-1, 0));
    float l_top = FxLuma(pixel + int2(0, -1));
    float4 delta;
    delta.xy = abs(l - float2(l_left, l_top));
    edges = step(0.1, delta.xy);
    if (dot(edges, float2(1.0, 1.0)) != 0.0) {
      float l_right = FxLuma(pixel + int2(1, 0));
      float l_bottom = FxLuma(pixel + int2(0, 1));
      delta.zw = abs(l - float2(l_right, l_bottom));
      float2 max_delta = max(delta.xy, delta.zw);
      float l_left_left = FxLuma(pixel + int2(-2, 0));
      float l_top_top = FxLuma(pixel + int2(0, -2));
      delta.zw = abs(float2(l_left, l_top) - float2(l_left_left, l_top_top));
      max_delta = max(max_delta.xy, delta.zw);
      float final_delta = max(max_delta.x, max_delta.y);
      edges *= step(final_delta, 2.0 * delta.xy);
    }
  }
  fx_edges[pixel] = float4(edges, 0.0, 0.0);
}

#elif defined(FX_SMAA_WEIGHTS)

FX_BINDING(0) Texture2D<float4> fx_edges : register(t0);
FX_BINDING(1) Texture2D<float4> fx_area : register(t1);
FX_BINDING(2) Texture2D<float4> fx_search : register(t2);
FX_BINDING(3) SamplerState fx_sampler : register(s0);
FX_BINDING(4) FX_FORMAT_RGBA16F RWTexture2D<float4> fx_weights : register(u0);

#include "smaa.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  int2 pixel = int2(fx_pass_rect.xy) - FX_SMAA_MARGIN + int2(id.xy);
  uint width, height;
  fx_weights.GetDimensions(width, height);
  if (any(pixel < 0) || any(pixel >= int2(width, height)) ||
      any(pixel >= int2(fx_pass_rect.zw) + FX_SMAA_MARGIN)) {
    return;
  }
  float4 weights = 0.0;
  if (all(pixel >= int2(fx_pass_rect.xy)) && all(pixel < int2(fx_pass_rect.zw))) {
    fx_smaa_metrics = float4(1.0 / float2(width, height), float2(width, height));
    float2 texcoord = (float2(pixel) + 0.5) * fx_smaa_metrics.xy;
    // SMAABlendingWeightCalculationVS.
    float4 offset[3];
    offset[0] = mad(SMAA_RT_METRICS.xyxy, float4(-0.25, -0.125, 1.25, -0.125), texcoord.xyxy);
    offset[1] = mad(SMAA_RT_METRICS.xyxy, float4(-0.125, -0.25, -0.125, 1.25), texcoord.xyxy);
    offset[2] = mad(SMAA_RT_METRICS.xxyy,
                    float4(-2.0, 2.0, -2.0, 2.0) * float(SMAA_MAX_SEARCH_STEPS),
                    float4(offset[0].xz, offset[1].yw));
    weights = SMAABlendingWeightCalculationPS(texcoord, texcoord * SMAA_RT_METRICS.zw, offset,
                                              fx_edges, fx_area, fx_search, float4(0.0, 0.0, 0.0, 0.0));
  }
  fx_weights[pixel] = weights;
}

#else  // FX_SMAA_BLEND

FX_BINDING(0) Texture2D<float4> fx_image : register(t0);
FX_BINDING(1) Texture2D<float4> fx_weights : register(t1);
FX_BINDING(2) SamplerState fx_sampler : register(s0);
FX_BINDING(3) FX_FORMAT_RGBA16F RWTexture2D<float4> fx_blended : register(u0);

#include "smaa.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  int2 pixel = int2(fx_pass_rect.xy) + int2(id.xy);
  if (any(pixel >= int2(fx_pass_rect.zw))) {
    return;
  }
  uint width, height;
  fx_blended.GetDimensions(width, height);
  fx_smaa_metrics = float4(1.0 / float2(width, height), float2(width, height));
  float2 texcoord = (float2(pixel) + 0.5) * fx_smaa_metrics.xy;
  // The split view: SMAA on the left half only.
  if (fx_debug_mode == FX_DEBUG_SPLIT && pixel.x >= int(fx_pass_rect.x + fx_pass_rect.z) / 2) {
    fx_blended[pixel] = float4(fx_image.Load(int3(pixel, 0)).rgb, 1.0);
    return;
  }
  // SMAANeighborhoodBlendingVS.
  float4 offset = mad(SMAA_RT_METRICS.xyxy, float4(1.0, 0.0, 0.0, 1.0), texcoord.xyxy);
  fx_blended[pixel] = float4(SMAANeighborhoodBlendingPS(texcoord, offset, fx_image, fx_weights).rgb,
                             1.0);
}

#endif
