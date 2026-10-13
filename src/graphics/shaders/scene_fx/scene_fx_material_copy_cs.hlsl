// Copies the materials tagged in the guest's stencil (sample 0 of a host depth
// render target, SceneEffects::TagMaterial: the upper 7 bits) as numbers
// (TextureCache::Material, 0 = unknown). A non-empty fx_pass_rect (screen
// pixels) limits the copy to that rectangle, one tile of a predicated-tiled
// scene; the rest keeps what it was last given. FX_MSAA selects the
// multisampled source variant.

#include "scene_fx_common.hlsli"

#ifdef FX_MSAA
FX_BINDING(0) Texture2DMS<uint2> fx_stencil : register(t0);
#else
FX_BINDING(0) Texture2D<uint2> fx_stencil : register(t0);
#endif
FX_BINDING(1) FX_FORMAT_R16F RWTexture2D<float> fx_material : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= fx_scene_size)) {
    return;
  }
  if (fx_pass_rect.z > fx_pass_rect.x &&
      (any(id.xy < fx_pass_rect.xy) || any(id.xy >= fx_pass_rect.zw))) {
    return;
  }
  int2 source = int2(id.xy) - fx_screen_offset;
#ifdef FX_MSAA
  uint2 value = fx_stencil.Load(source, 0);
#else
  uint2 value = fx_stencil.Load(int3(source, 0));
#endif
#ifdef SCENE_FX_VULKAN
  // A stencil aspect view: the stencil alone.
  uint stencil = value.x;
#else
  // X24_TYPELESS_G8_UINT / X32_TYPELESS_G8X24_UINT: the stencil in G.
  uint stencil = value.y;
#endif
  fx_material[id.xy] = float((stencil & 0xFFu) >> 1);
}
