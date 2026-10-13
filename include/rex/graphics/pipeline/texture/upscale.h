/**
 * Texture upscaling (texture_upscale): an edge-directed 2x scaler in the
 * spirit of xBR (written from the published rule, not derived from the GPL
 * xBRZ sources), applied once or twice for 2x or 4x, plus a mip chain.
 *
 * Pixels are RGBA8, 4 bytes each, rows tightly packed.
 */

#pragma once

#include <cstdint>
#include <vector>

namespace rex::graphics::texture_upscale {

// Doubles `src` (width x height): flat areas stay as they were (each pixel
// becomes a 2x2 block), and where a diagonal edge runs through a corner of a
// pixel that corner takes half of the neighbor across the edge, which rounds
// staircases into slopes. Transparent pixels don't bleed their color.
std::vector<uint8_t> Scale2x(const uint8_t* src, uint32_t width, uint32_t height);

// `scale` = 2 or 4 (other values return the source unchanged).
std::vector<uint8_t> Scale(const uint8_t* src, uint32_t width, uint32_t height, uint32_t scale);

// Halves an image (2x2 box, alpha-weighted color); sizes are halved and
// rounded down, at least 1 (the DDS mip chain sizes).
std::vector<uint8_t> Downsample(const uint8_t* src, uint32_t width, uint32_t height,
                                uint32_t* out_width, uint32_t* out_height);

// A PNG file (8-bit RGBA) of the image: per-row filters and a deflate stream
// with LZ77 matches and the fixed Huffman codes. Upscaled textures repeat
// a lot, so this packs them several times smaller than raw pixels.
std::vector<uint8_t> EncodePng(const uint8_t* rgba, uint32_t width, uint32_t height);

}  // namespace rex::graphics::texture_upscale
