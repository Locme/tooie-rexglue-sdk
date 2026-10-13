// Image effects on the guest's final image (display-referred, after its
// tonemapping): contrast-adaptive sharpening - after AMD FidelityFX CAS: a
// cross-shaped sharpening kernel whose strength adapts to the local contrast,
// so edges get crisper without halos or noise amplification - then color
// grading, then the retro N64 16-bit color. Eye adaptation's exposure (t1,
// from scene_fx_exposure_cs) is applied first.

#include "scene_fx_common.hlsli"

// The final image (a copy), at render target pixels.
FX_BINDING(0) Texture2D<float4> fx_image : register(t0);
// 1x1: x = the adapted exposure's correction in stops.
FX_BINDING(1) Texture2D<float4> fx_exposure : register(t1);
FX_EXTRA_CONSTANTS_BLOCK(2) {
  // x = sharpness (0...1), y = 1 to sharpen.
  float4 fx_image_sharpen;
  // x = exposure (multiplier), y = contrast, z = saturation, w = vibrance.
  float4 fx_image_grade;
  // rgb = white balance (temperature and tint), w = gamma.
  float4 fx_image_white;
  // x = shadow lift, y = vignette, z = film grain, w = 1 to dither to 8 bits.
  float4 fx_image_finish;
  // x = 1 for the N64's 16-bit color (5 bits per channel, magic square dither),
  // y = CRT scanline strength (0 = off), z = CRT mask strength.
  float4 fx_image_retro;
  // x = colorblind correction (0 off, 1 deuteranopia, 2 protanopia, 3
  // tritanopia), y = its strength.
  float4 fx_image_colorblind;
  // The vignette's rectangle (the 3D's area on the screen), in render target
  // pixels.
  float4 fx_image_area;
  // xy = screen pixel - render target pixel (a tile of a tiled frame), zw =
  // the screen's size.
  float4 fx_image_screen;
  // x = 1 to apply the eye adaptation's exposure.
  float4 fx_image_exposure;
};

// An exposure change on display-referred color that keeps white white: in
// linear light, 1 - (1 - x)^e for a gain e, which has slope e in the darks
// (a plain exposure there) and eases into the highlights instead of clipping.
float3 FxAdaptExposure(float3 color, float stops) {
  float gain = exp2(stops);
  float3 linear_color = pow(saturate(color), 2.2);
  float3 adapted = gain >= 1.0 ? 1.0 - pow(1.0 - linear_color, gain)
                               : linear_color * gain;
  return pow(saturate(adapted), 1.0 / 2.2);
}

// Daltonization: the colors a dichromat sees (Machado et al. 2009, severity
// 1, linear RGB), and the difference they lose moved to channels they see.
float3 FxColorblindCorrect(float3 color, uint mode, float strength) {
  float3x3 simulate;
  if (mode == 2u) {  // protanopia
    simulate = float3x3(0.152286, 1.052583, -0.204868, 0.114503, 0.786281, 0.099216,
                        -0.003882, -0.048116, 1.051998);
  } else if (mode == 3u) {  // tritanopia
    simulate = float3x3(1.255528, -0.076749, -0.178779, -0.078411, 0.930809, 0.147602,
                        0.004733, 0.691367, 0.303900);
  } else {  // deuteranopia
    simulate = float3x3(0.367322, 0.860646, -0.227968, 0.280085, 0.672501, 0.047413,
                        -0.011820, 0.042940, 0.968881);
  }
  float3 linear_color = pow(saturate(color), 2.2);
  float3 error = linear_color - mul(simulate, linear_color);
  // Red-green losses go to blue and to the brightness, blue-yellow losses
  // to red and green.
  float3 shift = mode == 3u ? float3(error.b * 0.7 + error.r, error.b * 0.7 + error.g, 0.0)
                            : float3(0.0, error.r * 0.7 + error.g, error.r * 0.7 + error.b);
  return pow(saturate(linear_color + shift * strength), 1.0 / 2.2);
}

float3 FxLoadImage(int2 pixel) {
  pixel = clamp(pixel, int2(fx_pass_rect.xy), int2(fx_pass_rect.zw) - 1);
  return fx_image.Load(int3(pixel, 0)).rgb;
}

float4 main(float4 position : SV_Position) : SV_Target {
  int2 pixel = int2(position.xy);
  float3 color = FxLoadImage(pixel);
  // The split view: the effects on the left half only.
  if (fx_debug_mode == FX_DEBUG_SPLIT && pixel.x >= int(fx_pass_rect.x + fx_pass_rect.z) / 2) {
    return float4(color, 1.0);
  }
  if (fx_image_sharpen.y > 0.0) {
    //   a b c
    //   d e f
    //   g h i
    float3 a = FxLoadImage(pixel + int2(-1, -1));
    float3 b = FxLoadImage(pixel + int2(0, -1));
    float3 c = FxLoadImage(pixel + int2(1, -1));
    float3 d = FxLoadImage(pixel + int2(-1, 0));
    float3 e = color;
    float3 f = FxLoadImage(pixel + int2(1, 0));
    float3 g = FxLoadImage(pixel + int2(-1, 1));
    float3 h = FxLoadImage(pixel + int2(0, 1));
    float3 i = FxLoadImage(pixel + int2(1, 1));
    // Soft minimum and maximum: the cross plus the whole 3x3 neighborhood.
    float3 cross_min = min(min(min(d, e), min(f, b)), h);
    float3 cross_max = max(max(max(d, e), max(f, b)), h);
    float3 soft_min = cross_min + min(cross_min, min(min(a, c), min(g, i)));
    float3 soft_max = cross_max + max(cross_max, max(max(a, c), max(g, i)));
    // Less sharpening where the contrast is already high (or near clipping).
    float3 amplitude = sqrt(saturate(min(soft_min, 2.0 - soft_max) / max(soft_max, 1.0e-4)));
    float peak = -1.0 / lerp(8.0, 5.0, saturate(fx_image_sharpen.x));
    float3 weight = amplitude * peak;
    color = saturate(((b + d + f + h) * weight + e) / (1.0 + 4.0 * weight));
  }

  if (fx_image_exposure.x > 0.0) {
    color = FxAdaptExposure(color, fx_exposure.Load(int3(0, 0, 0)).x);
  }
  color *= fx_image_grade.x * fx_image_white.rgb;
  float luminance = FxLuminance(color);
  // Vibrance: saturates muted colors more than already vivid ones.
  float chroma = max(max(color.r, color.g), color.b) - min(min(color.r, color.g), color.b);
  float saturation = fx_image_grade.z * (1.0 + fx_image_grade.w * (1.0 - saturate(chroma)));
  color = lerp(luminance.xxx, color, saturation);
  color = (color - 0.5) * fx_image_grade.y + 0.5;
  // Shadow lift: raises the darks, leaving the whites.
  color = saturate(color);
  color += fx_image_finish.x * (1.0 - color) * (1.0 - color) * (1.0 - color);
  color = pow(saturate(color), fx_image_white.w);

  // Vignette, over the 3D's area (all of the tiles of a tiled frame).
  float2 area_size = max(fx_image_area.zw - fx_image_area.xy, 1.0);
  float2 centered = (position.xy - fx_image_area.xy) / area_size * 2.0 - 1.0;
  centered.x *= area_size.x / area_size.y;
  color *= saturate(1.0 - fx_image_finish.y * dot(centered, centered) * 0.25);
  // Film grain, changing every frame, stronger in the midtones.
  float grain = FxNoise(position.xy + fx_image_screen.xy + float2(17.0, 59.0) * float(fx_frame & 255)) - 0.5;
  float midtones = 4.0 * FxLuminance(color) * (1.0 - FxLuminance(color));
  color += grain * fx_image_finish.z * 0.15 * midtones;
  if (fx_image_colorblind.x > 0.0) {
    color = FxColorblindCorrect(color, uint(fx_image_colorblind.x), fx_image_colorblind.y);
  }
  if (fx_image_retro.y > 0.0 || fx_image_retro.z > 0.0) {
    // A CRT showing the N64's 240 lines: each line's beam brightest at its
    // center, darker between lines (brightened back to keep the average), and
    // an aperture grille of red, green and blue stripes.
    float scanline = (position.y + fx_image_screen.y) / max(fx_image_screen.w, 1.0) * 240.0;
    float beam = 0.5 + 0.5 * cos(6.2831853 * (frac(scanline) - 0.5));
    color *= lerp(1.0, (0.35 + 0.65 * beam) * 1.25, fx_image_retro.y);
    uint stripe = uint(position.x) % 3u;
    float3 mask = float3(stripe == 0u, stripe == 1u, stripe == 2u);
    // The other two channels dimmed, the average kept.
    float dim = 0.6 * fx_image_retro.z;
    color *= lerp((1.0 - dim).xxx, 1.0.xxx, mask) / (1.0 - dim * (2.0 / 3.0));
  }
  if (fx_image_retro.x > 0.0) {
    // The RDP's 16-bit framebuffer: 8-bit color + the magic square's 0...7
    // (an eighth of a 5-bit step each), truncated to 5 bits, read back by the
    // VI as 5 bits repeated into 8.
    const uint kMagicSquare[16] = {0u, 6u, 1u, 7u, 4u, 2u, 5u, 3u,
                                   3u, 5u, 2u, 4u, 7u, 1u, 6u, 0u};
    uint2 cell = uint2(position.xy) & 3u;
    float dither = float(kMagicSquare[cell.y * 4u + cell.x]);
    float3 bits5 = min(floor((floor(saturate(color) * 255.0 + 0.5) + dither) / 8.0), 31.0);
    return float4((bits5 * 8.0 + floor(bits5 / 4.0)) / 255.0, 1.0);
  }
  if (fx_image_finish.w > 0.0) {
    color += FxDither8(position.xy);
  }
  return float4(saturate(color), 1.0);
}
