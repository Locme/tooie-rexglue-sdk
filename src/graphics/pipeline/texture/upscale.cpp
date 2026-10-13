#include <rex/graphics/pipeline/texture/upscale.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace rex::graphics::texture_upscale {

namespace {

struct Pixel {
  float r, g, b, a;
  float y, u, v;
};

Pixel Load(const uint8_t* p) {
  Pixel out;
  out.r = p[0];
  out.g = p[1];
  out.b = p[2];
  out.a = p[3];
  out.y = 0.299f * out.r + 0.587f * out.g + 0.114f * out.b;
  out.u = -0.169f * out.r - 0.331f * out.g + 0.5f * out.b;
  out.v = 0.5f * out.r - 0.419f * out.g - 0.081f * out.b;
  return out;
}

// Perceptual distance: brightness counts most, alpha as much as brightness
// (so cut-out edges count as edges).
float Distance(const Pixel& p, const Pixel& q) {
  return 48.0f * std::fabs(p.y - q.y) + 7.0f * std::fabs(p.u - q.u) +
         6.0f * std::fabs(p.v - q.v) + 48.0f * std::fabs(p.a - q.a);
}

// Below this the two colors count as the same.
constexpr float kSame = 48.0f * 2.0f;

// Half of p and half of q, with each color weighted by its alpha.
void Blend(uint8_t* out, const uint8_t* p, const uint8_t* q) {
  const uint32_t ap = p[3], aq = q[3];
  const uint32_t sum = ap + aq;
  for (int c = 0; c < 3; ++c) {
    out[c] = sum ? uint8_t((p[c] * ap + q[c] * aq + sum / 2) / sum)
                 : uint8_t((p[c] + q[c] + 1) / 2);
  }
  out[3] = uint8_t((ap + aq + 1) / 2);
}

}  // namespace

std::vector<uint8_t> Scale2x(const uint8_t* src, uint32_t width, uint32_t height) {
  const uint32_t ow = width * 2, oh = height * 2;
  std::vector<uint8_t> out(size_t(ow) * oh * 4);
  if (!width || !height) {
    return out;
  }
  std::vector<Pixel> pixels(size_t(width) * height);
  for (size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = Load(src + i * 4);
  }
  auto at = [&](int x, int y) -> size_t {
    x = std::clamp(x, 0, int(width) - 1);
    y = std::clamp(y, 0, int(height) - 1);
    return size_t(y) * width + x;
  };
  // The bottom-right corner's neighbors as (dx, dy); the other corners use
  // them rotated by 90 degrees each.
  enum { E, F, H, I, C, G, F4, H5, D, I5, I4, B, kCount };
  static constexpr int kOffsets[kCount][2] = {
      {0, 0}, {1, 0}, {0, 1}, {1, 1}, {1, -1}, {-1, 1},
      {2, 0}, {0, 2}, {-1, 0}, {1, 2}, {2, 1}, {0, -1}};
  // Output subpixel of each rotation: bottom right, bottom left, top left,
  // top right.
  static constexpr int kCorner[4][2] = {{1, 1}, {0, 1}, {0, 0}, {1, 0}};
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* e = src + at(int(x), int(y)) * 4;
      for (int sy = 0; sy < 2; ++sy) {
        for (int sx = 0; sx < 2; ++sx) {
          std::memcpy(&out[(size_t(y * 2 + sy) * ow + x * 2 + sx) * 4], e, 4);
        }
      }
      for (int rot = 0; rot < 4; ++rot) {
        size_t idx[kCount];
        for (int k = 0; k < kCount; ++k) {
          int dx = kOffsets[k][0], dy = kOffsets[k][1];
          for (int r = 0; r < rot; ++r) {
            const int t = dx;
            dx = -dy;
            dy = t;
          }
          idx[k] = at(int(x) + dx, int(y) + dy);
        }
        const Pixel& pe = pixels[idx[E]];
        const Pixel& pf = pixels[idx[F]];
        const Pixel& ph = pixels[idx[H]];
        const float d_ef = Distance(pe, pf), d_eh = Distance(pe, ph);
        if (d_ef < kSame || d_eh < kSame) {
          continue;
        }
        auto d = [&](int a, int b) { return Distance(pixels[idx[a]], pixels[idx[b]]); };
        // An edge from F to H cuts off this corner when the colors along it
        // match better than across it.
        const float along = d(E, C) + d(E, G) + d(I, F4) + d(I, H5) + 4.0f * d(H, F);
        const float across = d(H, D) + d(H, I5) + d(F, I4) + d(F, B) + 4.0f * d(E, I);
        if (along >= across) {
          continue;
        }
        const size_t neighbor = d_ef <= d_eh ? idx[F] : idx[H];
        uint8_t* o = &out[(size_t(y * 2 + kCorner[rot][1]) * ow + x * 2 + kCorner[rot][0]) * 4];
        Blend(o, e, src + neighbor * 4);
      }
    }
  }
  return out;
}

std::vector<uint8_t> Scale(const uint8_t* src, uint32_t width, uint32_t height, uint32_t scale) {
  if (scale == 2) {
    return Scale2x(src, width, height);
  }
  if (scale == 4) {
    std::vector<uint8_t> twice = Scale2x(src, width, height);
    return Scale2x(twice.data(), width * 2, height * 2);
  }
  return std::vector<uint8_t>(src, src + size_t(width) * height * 4);
}

std::vector<uint8_t> Downsample(const uint8_t* src, uint32_t width, uint32_t height,
                                uint32_t* out_width, uint32_t* out_height) {
  // The same level sizes as DDS mip chains (and LayOutLevels): halved,
  // rounded down, at least 1.
  const uint32_t ow = std::max(1u, width / 2), oh = std::max(1u, height / 2);
  std::vector<uint8_t> out(size_t(ow) * oh * 4);
  for (uint32_t y = 0; y < oh; ++y) {
    for (uint32_t x = 0; x < ow; ++x) {
      uint32_t color[3] = {}, alpha = 0, plain[3] = {}, n = 0;
      for (uint32_t sy = 0; sy < 2; ++sy) {
        for (uint32_t sx = 0; sx < 2; ++sx) {
          const uint32_t px = std::min(x * 2 + sx, width - 1);
          const uint32_t py = std::min(y * 2 + sy, height - 1);
          const uint8_t* p = src + (size_t(py) * width + px) * 4;
          for (int c = 0; c < 3; ++c) {
            color[c] += p[c] * p[3];
            plain[c] += p[c];
          }
          alpha += p[3];
          ++n;
        }
      }
      uint8_t* o = &out[(size_t(y) * ow + x) * 4];
      for (int c = 0; c < 3; ++c) {
        o[c] = alpha ? uint8_t((color[c] + alpha / 2) / alpha) : uint8_t((plain[c] + n / 2) / n);
      }
      o[3] = uint8_t((alpha + n / 2) / n);
    }
  }
  *out_width = ow;
  *out_height = oh;
  return out;
}

namespace {

// Deflate with the fixed Huffman codes (RFC 1951 3.2.6) and LZ77 matches
// found through hash chains, wrapped in zlib (RFC 1950).
class BitWriter {
 public:
  explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}
  // `count` bits of `value`, least significant first.
  void Bits(uint32_t value, int count) {
    acc_ |= uint64_t(value) << fill_;
    fill_ += count;
    while (fill_ >= 8) {
      out_.push_back(uint8_t(acc_));
      acc_ >>= 8;
      fill_ -= 8;
    }
  }
  // A Huffman code: most significant bit first.
  void Code(uint32_t code, int count) {
    uint32_t reversed = 0;
    for (int i = 0; i < count; ++i) {
      reversed |= ((code >> i) & 1) << (count - 1 - i);
    }
    Bits(reversed, count);
  }
  void Flush() {
    if (fill_ > 0) {
      out_.push_back(uint8_t(acc_));
    }
    acc_ = 0;
    fill_ = 0;
  }

 private:
  std::vector<uint8_t>& out_;
  uint64_t acc_ = 0;
  int fill_ = 0;
};

void Literal(BitWriter& w, uint32_t symbol) {
  if (symbol < 144) {
    w.Code(0x30 + symbol, 8);
  } else if (symbol < 256) {
    w.Code(0x190 + (symbol - 144), 9);
  } else if (symbol < 280) {
    w.Code(symbol - 256, 7);
  } else {
    w.Code(0xC0 + (symbol - 280), 8);
  }
}

constexpr uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                    33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                    1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void Match(BitWriter& w, uint32_t length, uint32_t distance) {
  int l = 28;
  while (kLengthBase[l] > length) --l;
  Literal(w, 257 + l);
  w.Bits(length - kLengthBase[l], kLengthExtra[l]);
  int d = 29;
  while (kDistBase[d] > distance) --d;
  w.Code(d, 5);
  w.Bits(distance - kDistBase[d], kDistExtra[d]);
}

std::vector<uint8_t> Zlib(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> out = {0x78, 0x01};
  BitWriter w(out);
  w.Bits(1, 1);  // final block
  w.Bits(1, 2);  // fixed Huffman codes
  constexpr uint32_t kWindow = 32768, kHashBits = 15, kMaxChain = 48;
  constexpr uint32_t kMinMatch = 3, kMaxMatch = 258;
  std::vector<int32_t> head(size_t(1) << kHashBits, -1);
  std::vector<int32_t> prev(data.size(), -1);
  auto hash = [&](size_t i) {
    return ((uint32_t(data[i]) << 10) ^ (uint32_t(data[i + 1]) << 5) ^ data[i + 2]) &
           ((1u << kHashBits) - 1);
  };
  auto insert = [&](size_t i) {
    if (i + 2 < data.size()) {
      const uint32_t h = hash(i);
      prev[i] = head[h];
      head[h] = int32_t(i);
    }
  };
  size_t i = 0;
  while (i < data.size()) {
    uint32_t best_length = 0, best_distance = 0;
    if (i + kMinMatch <= data.size()) {
      int32_t candidate = head[hash(i)];
      const size_t max_length = std::min<size_t>(kMaxMatch, data.size() - i);
      for (uint32_t chain = 0; candidate >= 0 && chain < kMaxChain; ++chain) {
        if (i - size_t(candidate) > kWindow) break;
        uint32_t length = 0;
        while (length < max_length && data[candidate + length] == data[i + length]) ++length;
        if (length > best_length) {
          best_length = length;
          best_distance = uint32_t(i - candidate);
          if (length == max_length) break;
        }
        candidate = prev[candidate];
      }
    }
    if (best_length >= kMinMatch) {
      Match(w, best_length, best_distance);
      for (uint32_t k = 0; k < best_length; ++k) insert(i + k);
      i += best_length;
    } else {
      Literal(w, data[i]);
      insert(i);
      ++i;
    }
  }
  Literal(w, 256);  // end of block
  w.Flush();
  uint32_t a = 1, b = 0;
  for (uint8_t v : data) {
    a = (a + v) % 65521;
    b = (b + a) % 65521;
  }
  const uint32_t adler = (b << 16) | a;
  for (int k = 3; k >= 0; --k) out.push_back(uint8_t(adler >> (8 * k)));
  return out;
}

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[n] = c;
    }
    ready = true;
  }
  crc = ~crc;
  for (size_t k = 0; k < size; ++k) crc = table[(crc ^ data[k]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void Chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& body) {
  const uint32_t size = uint32_t(body.size());
  for (int k = 3; k >= 0; --k) png.push_back(uint8_t(size >> (8 * k)));
  const size_t start = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), body.begin(), body.end());
  const uint32_t crc = Crc32(png.data() + start, png.size() - start);
  for (int k = 3; k >= 0; --k) png.push_back(uint8_t(crc >> (8 * k)));
}

uint8_t Paeth(int a, int b, int c) {
  const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
  return uint8_t(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

}  // namespace

std::vector<uint8_t> EncodePng(const uint8_t* rgba, uint32_t width, uint32_t height) {
  const size_t stride = size_t(width) * 4;
  // Each row with the filter (none, sub, up, average, Paeth) whose output
  // has the smallest sum of absolute values.
  std::vector<uint8_t> raw;
  raw.reserve((stride + 1) * height);
  std::vector<uint8_t> candidate(stride), best(stride);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* row = rgba + y * stride;
    const uint8_t* up = y ? row - stride : nullptr;
    uint64_t best_cost = UINT64_MAX;
    uint8_t best_filter = 0;
    for (uint8_t filter = 0; filter < 5; ++filter) {
      uint64_t cost = 0;
      for (size_t x = 0; x < stride; ++x) {
        const int a = x >= 4 ? row[x - 4] : 0, b = up ? up[x] : 0,
                  c = (up && x >= 4) ? up[x - 4] : 0;
        int predicted = 0;
        switch (filter) {
          case 1: predicted = a; break;
          case 2: predicted = b; break;
          case 3: predicted = (a + b) / 2; break;
          case 4: predicted = Paeth(a, b, c); break;
          default: break;
        }
        candidate[x] = uint8_t(row[x] - predicted);
        cost += candidate[x] < 128 ? candidate[x] : 256 - candidate[x];
      }
      if (cost < best_cost) {
        best_cost = cost;
        best_filter = filter;
        best.swap(candidate);
      }
    }
    raw.push_back(best_filter);
    raw.insert(raw.end(), best.begin(), best.end());
  }
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  for (uint32_t v : {width, height}) {
    for (int k = 3; k >= 0; --k) ihdr.push_back(uint8_t(v >> (8 * k)));
  }
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA, deflate, adaptive, no interlace
  Chunk(png, "IHDR", ihdr);
  Chunk(png, "IDAT", Zlib(raw));
  Chunk(png, "IEND", {});
  return png;
}

}  // namespace rex::graphics::texture_upscale
