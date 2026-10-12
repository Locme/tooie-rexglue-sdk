// Copies the guest's depth (sample 0 of a host depth render target):
// - Default: the scene depth as linear view distance over the whole scene.
// - FX_RAW: a shadow map's stored depth as is (orthographic, already linear).
// FX_MSAA selects the multisampled source variant.
// A non-empty fx_pass_rect (screen pixels) limits the copy to that rectangle:
// one tile of a predicated-tiled scene, keeping the tiles above (copied
// before it) and, below it (not drawn yet), reprojecting the previous frame's
// depth (fx_depth_params.x) or emptying.

#include "scene_fx_common.hlsli"

#ifdef FX_MSAA
FX_BINDING(0) Texture2DMS<float> fx_source : register(t0);
#else
FX_BINDING(0) Texture2D<float> fx_source : register(t0);
#endif
#ifdef FX_RAW
FX_BINDING(1) FX_FORMAT_R32F RWTexture2D<float> fx_depth : register(u0);
#else
FX_BINDING(1) Texture2D<float> fx_depth_previous : register(t1);
FX_EXTRA_CONSTANTS_BLOCK(2) {
  // Previous frame's view space to this frame's clip space.
  float4 fx_depth_reproject[4];
  // x = 1 to fill the rows not drawn yet from the previous depth.
  float4 fx_depth_params;
};
FX_BINDING(3) FX_FORMAT_R32F RWTexture2D<float> fx_depth : register(u0);
#endif

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= fx_scene_size)) {
    return;
  }
  if (fx_pass_rect.z > fx_pass_rect.x) {
    if (any(id.xy < fx_pass_rect.xy) || id.x >= fx_pass_rect.z) {
      return;
    }
#ifndef FX_RAW
    if (id.y >= fx_pass_rect.w) {
      // The tiles below aren't drawn yet this frame: the previous frame's
      // depth where the camera has moved to (as the surface seen there was
      // roughly where it is now), else sky. Not the previous depth as is:
      // it no longer lines up with this tile's (dark lines along the tile
      // edges), and not sky alone either (bright lines, nothing occluding
      // from below).
      float filled = 1.0e6;
      if (fx_depth_params.x > 0.0) {
        float previous = fx_depth_previous.Load(int3(id.xy, 0));
        if (previous > 0.0 && previous < 1.0e5) {
          float4 p = float4(FxViewPosition(float2(id.xy) + 0.5, previous), 1.0);
          // The view space distance is the clip space w.
          float distance = dot(fx_depth_reproject[3], p);
          if (distance > 0.0) {
            filled = min(distance, 1.0e6);
          }
        }
      }
      fx_depth[id.xy] = filled;
      return;
    }
#endif
  }
  int2 source = int2(id.xy) - fx_screen_offset;
#ifdef FX_MSAA
  float device_depth = fx_source.Load(source, 0);
#else
  float device_depth = fx_source.Load(int3(source, 0));
#endif
#ifdef FX_RAW
  fx_depth[id.xy] = device_depth;
#else
  float distance = FxViewDistance(device_depth);
  // Past the far plane (or numerically behind the camera): treat as sky.
  fx_depth[id.xy] = distance > 0.0 ? min(distance, 1.0e6) : 1.0e6;
#endif
}
