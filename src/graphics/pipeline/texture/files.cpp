#include <rex/graphics/pipeline/texture/files.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <mutex>
#include <deque>
#include <condition_variable>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/upscale.h>
#include <rex/hash.h>
#include <rex/ui/image_decode.h>

namespace rex::graphics::texture_files {

namespace {

FileFormat FileFormatForImpl(xenos::TextureFormat format) {
  switch (format) {
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_8_8_8_8_A:
    case xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16:
      return kFileFormatR8G8B8A8Unorm;
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT1_AS_16_16_16_16:
      return kFileFormatBc1Unorm;
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16:
      return kFileFormatBc2Unorm;
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16:
      return kFileFormatBc3Unorm;
    case xenos::TextureFormat::k_DXT5A:
      return kFileFormatBc4Unorm;
    case xenos::TextureFormat::k_DXN:
      return kFileFormatBc5Unorm;
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_B:
      return kFileFormatR8Unorm;
    case xenos::TextureFormat::k_8_8:
      return kFileFormatR8G8Unorm;
    case xenos::TextureFormat::k_5_6_5:
      return kFileFormatB5G6R5Unorm;
    case xenos::TextureFormat::k_1_5_5_5:
      return kFileFormatB5G5R5A1Unorm;
    case xenos::TextureFormat::k_4_4_4_4:
      return kFileFormatB4G4R4A4Unorm;
    case xenos::TextureFormat::k_16_16_16_16:
      return kFileFormatR16G16B16A16Unorm;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      return kFileFormatR16G16B16A16Float;
    case xenos::TextureFormat::k_16_16_FLOAT:
      return kFileFormatR16G16Float;
    case xenos::TextureFormat::k_16_FLOAT:
      return kFileFormatR16Float;
    default:
      return kFileFormatNone;
  }
}

const char* FormatName(xenos::TextureFormat format) {
  const FormatInfo* info = FormatInfo::Get(format);
  return info && info->name ? info->name : "unknown";
}

void WriteU32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(uint8_t(value));
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value >> 16));
  out.push_back(uint8_t(value >> 24));
}

}  // namespace

uint64_t HashGuestBase(const uint8_t* guest_base, uint32_t guest_base_size,
                       const Description& key) {
  XXH3_state_t* state = XXH3_createState();
  XXH3_64bits_reset(state);
  XXH3_64bits_update(state, guest_base, guest_base_size);
  uint32_t salt[3] = {uint32_t(key.format), key.width, key.height};
  XXH3_64bits_update(state, salt, sizeof(salt));
  uint64_t hash = XXH3_64bits_digest(state);
  XXH3_freeState(state);
  return hash;
}

std::string FileStem(const Description& key, uint64_t hash) {
  char buffer[128];
  std::snprintf(buffer, sizeof(buffer), "%s_%ux%u_%016llx", FormatName(key.format), key.width,
                key.height, static_cast<unsigned long long>(hash));
  return buffer;
}

namespace {

// The base level untiled and byte-swapped, rows of *pitch_out bytes (blocks
// for the compressed formats), in the file's channel order.
bool ReadBaseLevel(const uint8_t* guest_base, const Description& key, uint32_t swizzle,
                   std::vector<uint8_t>& pixels, uint32_t* pitch_out) {
  if (key.dimension != xenos::DataDimension::k2DOrStacked || key.depth_or_array_size != 1) {
    return false;
  }
  uint32_t dxgi_format = FileFormatFor(key.format);
  const FormatInfo* format_info = FormatInfo::Get(key.format);
  if (!dxgi_format || !format_info) {
    return false;
  }
  uint32_t width = key.width, height = key.height;
  uint32_t bytes_per_block = format_info->bytes_per_block();
  uint32_t blocks_x = (width + format_info->block_width - 1) / format_info->block_width;
  uint32_t blocks_y = (height + format_info->block_height - 1) / format_info->block_height;
  uint32_t out_pitch = blocks_x * bytes_per_block;
  pixels.assign(size_t(out_pitch) * blocks_y, 0);
  xenos::Endian endian = key.endian;
  if (key.tiled) {
    texture_conversion::UntileInfo untile = {};
    untile.width = blocks_x;
    untile.height = blocks_y;
    untile.input_pitch = key.row_pitch_bytes / bytes_per_block;
    untile.output_pitch = blocks_x;
    untile.input_format_info = format_info;
    untile.output_format_info = format_info;
    untile.copy_callback = [endian](void* output, const void* input, size_t length) {
      texture_conversion::CopySwapBlock(endian, output, input, length);
    };
    texture_conversion::Untile(pixels.data(), guest_base, &untile);
  } else {
    for (uint32_t y = 0; y < blocks_y; ++y) {
      texture_conversion::CopySwapBlock(endian, pixels.data() + size_t(y) * out_pitch,
                                        guest_base + size_t(y) * key.row_pitch_bytes, out_pitch);
    }
  }
  SwapGuest16BitChannelsToFileOrder(key.format, pixels.data(), pixels.size());
  if (dxgi_format == kFileFormatR8G8B8A8Unorm) {
    SwizzleRGBA8ToFileOrder(pixels.data(), pixels.size(), swizzle);
  }
  *pitch_out = out_pitch;
  return true;
}

// DDS: magic, 124-byte header, 20-byte DX10 header, then the levels (data
// holds them back to back; `pitch` and `linear_size` describe the first).
bool WriteDds(const std::filesystem::path& directory, const std::string& stem,
              uint32_t dxgi_format, bool compressed, uint32_t width, uint32_t height,
              uint32_t mip_count, uint32_t pitch, uint32_t linear_size,
              const std::vector<uint8_t>& data) {
  std::vector<uint8_t> file;
  file.reserve(148 + data.size());
  WriteU32(file, 0x20534444);  // "DDS "
  WriteU32(file, 124);
  // CAPS | HEIGHT | WIDTH | PIXELFORMAT | (PITCH or LINEARSIZE) | MIPMAPCOUNT
  WriteU32(file, 0x1 | 0x2 | 0x4 | 0x1000 | (compressed ? 0x80000 : 0x8) |
                     (mip_count > 1 ? 0x20000 : 0));
  WriteU32(file, height);
  WriteU32(file, width);
  WriteU32(file, compressed ? linear_size : pitch);
  WriteU32(file, 0);  // depth
  WriteU32(file, mip_count);
  for (int i = 0; i < 11; ++i) {
    WriteU32(file, 0);  // reserved
  }
  // Pixel format: size, FOURCC flag, "DX10", then zeros.
  WriteU32(file, 32);
  WriteU32(file, 0x4);
  WriteU32(file, 0x30315844);
  for (int i = 0; i < 5; ++i) {
    WriteU32(file, 0);
  }
  // caps: TEXTURE, plus COMPLEX | MIPMAP with mips.
  WriteU32(file, 0x1000 | (mip_count > 1 ? 0x400008 : 0));
  for (int i = 0; i < 4; ++i) {
    WriteU32(file, 0);  // caps2..4, reserved2
  }
  // DX10 header.
  WriteU32(file, dxgi_format);
  WriteU32(file, 3);  // D3D10_RESOURCE_DIMENSION_TEXTURE2D
  WriteU32(file, 0);  // misc flags
  WriteU32(file, 1);  // array size
  WriteU32(file, 0);  // misc flags 2 (alpha mode unknown)
  file.insert(file.end(), data.begin(), data.end());

  std::error_code error;
  std::filesystem::create_directories(directory, error);
  // Written under a temporary name and renamed, so a reader never sees half
  // a file.
  std::filesystem::path path = directory / (stem + ".dds");
  std::filesystem::path temp = directory / (stem + ".dds.tmp");
  FILE* f = std::fopen(temp.string().c_str(), "wb");
  if (!f) {
    return false;
  }
  bool ok = std::fwrite(file.data(), 1, file.size(), f) == file.size();
  ok = std::fclose(f) == 0 && ok;
  if (ok) {
    std::filesystem::rename(temp, path, error);
    ok = !error;
  }
  if (!ok) {
    std::filesystem::remove(temp, error);
  }
  return ok;
}

}  // namespace

bool DumpBaseLevel(const std::filesystem::path& directory, const std::string& stem,
                   const uint8_t* guest_base, const Description& key, uint32_t swizzle) {
  std::vector<uint8_t> pixels;
  uint32_t pitch = 0;
  if (!ReadBaseLevel(guest_base, key, swizzle, pixels, &pitch)) {
    return false;
  }
  const FormatInfo* format_info = FormatInfo::Get(key.format);
  return WriteDds(directory, stem, FileFormatFor(key.format),
                  format_info->type == FormatType::kCompressed, key.width, key.height, 1, pitch,
                  uint32_t(pixels.size()), pixels);
}

bool CanUpscale(const Description& key) {
  return key.dimension == xenos::DataDimension::k2DOrStacked && key.depth_or_array_size == 1 &&
         FileFormatFor(key.format) == kFileFormatR8G8B8A8Unorm;
}

namespace {

uint32_t LayOutLevels(Replacement& replacement);
void FileFormatBlockInfo(FileFormat format, uint32_t& block_width, uint32_t& block_height,
                         uint32_t& bytes_per_block);

// An RGBA8 replacement of `base` (width x height) with a full mip chain.
std::shared_ptr<Replacement> RGBA8WithMips(std::vector<uint8_t> base, uint32_t width,
                                           uint32_t height) {
  auto replacement = std::make_shared<Replacement>();
  replacement->format = kFileFormatR8G8B8A8Unorm;
  replacement->width = width;
  replacement->height = height;
  FileFormatBlockInfo(replacement->format, replacement->block_width, replacement->block_height,
                      replacement->bytes_per_block);
  std::vector<uint8_t> data = base;
  std::vector<uint8_t> level = std::move(base);
  uint32_t mip_count = 1, w = width, h = height;
  while ((w > 1 || h > 1) && mip_count < Replacement::kMaxMips) {
    uint32_t nw, nh;
    level = texture_upscale::Downsample(level.data(), w, h, &nw, &nh);
    data.insert(data.end(), level.begin(), level.end());
    w = nw;
    h = nh;
    ++mip_count;
  }
  replacement->mip_count = mip_count;
  LayOutLevels(*replacement);
  replacement->data = std::move(data);
  return replacement;
}

// One background thread writes the upscaled textures' PNG files, so the GPU
// thread only pays for the upscale itself.
class PngWriter {
 public:
  void Add(std::filesystem::path path, std::vector<uint8_t> rgba, uint32_t width,
           uint32_t height) {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.push_back({std::move(path), std::move(rgba), width, height});
    if (!thread_.joinable()) {
      thread_ = std::thread([this] { Run(); });
      thread_.detach();
    }
    wake_.notify_one();
  }

 private:
  struct Job {
    std::filesystem::path path;
    std::vector<uint8_t> rgba;
    uint32_t width, height;
  };
  void Run() {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [this] { return !jobs_.empty(); });
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      const std::vector<uint8_t> png =
          texture_upscale::EncodePng(job.rgba.data(), job.width, job.height);
      std::error_code error;
      std::filesystem::create_directories(job.path.parent_path(), error);
      // Written under a temporary name and renamed, so a reader never sees
      // half a file.
      std::filesystem::path temp = job.path;
      temp += ".tmp";
      FILE* f = std::fopen(temp.string().c_str(), "wb");
      if (!f) {
        continue;
      }
      bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
      ok = std::fclose(f) == 0 && ok;
      if (ok) {
        std::filesystem::rename(temp, job.path, error);
      } else {
        std::filesystem::remove(temp, error);
      }
    }
  }
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Job> jobs_;
  std::thread thread_;
};

PngWriter& GetPngWriter() {
  static PngWriter* writer = new PngWriter();  // never destroyed: its thread may still run
  return *writer;
}

}  // namespace

std::shared_ptr<Replacement> UpscaleBaseLevel(const uint8_t* guest_base, const Description& key,
                                              uint32_t swizzle, uint32_t scale,
                                              const std::filesystem::path& save_path) {
  if (!CanUpscale(key)) {
    return nullptr;
  }
  std::vector<uint8_t> pixels;
  uint32_t pitch = 0;
  if (!ReadBaseLevel(guest_base, key, swizzle, pixels, &pitch) || pitch != key.width * 4) {
    return nullptr;
  }
  const uint32_t width = key.width * scale, height = key.height * scale;
  std::vector<uint8_t> base = texture_upscale::Scale(pixels.data(), key.width, key.height, scale);
  if (!save_path.empty()) {
    GetPngWriter().Add(save_path, base, width, height);
  }
  return RGBA8WithMips(std::move(base), width, height);
}

namespace {

uint8_t Expand(uint32_t v, uint32_t bits) {
  return uint8_t(bits >= 8 ? v : (v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1));
}

uint16_t LoadLE16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

// A BC1 color block's four colors (RGBA8); `bc1` allows the transparent
// fourth color.
void Bc1Palette(const uint8_t* block, bool bc1, uint8_t palette[4][4]) {
  const uint16_t c0 = LoadLE16(block), c1 = LoadLE16(block + 2);
  for (int i = 0; i < 2; ++i) {
    const uint16_t c = i ? c1 : c0;
    palette[i][0] = Expand(c >> 11, 5);
    palette[i][1] = Expand((c >> 5) & 63, 6);
    palette[i][2] = Expand(c & 31, 5);
    palette[i][3] = 255;
  }
  for (int ch = 0; ch < 3; ++ch) {
    if (!bc1 || c0 > c1) {
      palette[2][ch] = uint8_t((2 * palette[0][ch] + palette[1][ch] + 1) / 3);
      palette[3][ch] = uint8_t((palette[0][ch] + 2 * palette[1][ch] + 1) / 3);
    } else {
      palette[2][ch] = uint8_t((palette[0][ch] + palette[1][ch] + 1) / 2);
      palette[3][ch] = 0;
    }
  }
  palette[2][3] = 255;
  palette[3][3] = (!bc1 || c0 > c1) ? 255 : 0;
}

// The 8 alphas of a BC3 / BC4 alpha block.
void AlphaPalette(const uint8_t* block, uint8_t alpha[8]) {
  alpha[0] = block[0];
  alpha[1] = block[1];
  if (alpha[0] > alpha[1]) {
    for (int i = 1; i < 7; ++i) alpha[i + 1] = uint8_t(((7 - i) * alpha[0] + i * alpha[1] + 3) / 7);
  } else {
    for (int i = 1; i < 5; ++i) alpha[i + 1] = uint8_t(((5 - i) * alpha[0] + i * alpha[1] + 2) / 5);
    alpha[6] = 0;
    alpha[7] = 255;
  }
}

uint32_t AlphaIndex(const uint8_t* block, int texel) {
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(block[2 + i]) << (8 * i);
  return uint32_t(bits >> (3 * texel)) & 7;
}

}  // namespace

bool DecodeToRGBA8(FileFormat format, const uint8_t* data, uint32_t pitch, uint32_t width,
                   uint32_t height, std::vector<uint8_t>& rgba) {
  rgba.assign(size_t(width) * height * 4, 0);
  auto put = [&](uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x >= width || y >= height) return;
    uint8_t* o = rgba.data() + (size_t(y) * width + x) * 4;
    o[0] = r;
    o[1] = g;
    o[2] = b;
    o[3] = a;
  };
  switch (format) {
    case kFileFormatR8G8B8A8Unorm:
      for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(rgba.data() + size_t(y) * width * 4, data + size_t(y) * pitch, width * 4);
      }
      return true;
    case kFileFormatR8Unorm:
      for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
          const uint8_t v = data[size_t(y) * pitch + x];
          put(x, y, v, v, v, 255);
        }
      return true;
    case kFileFormatR8G8Unorm:
      for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
          const uint8_t* p = data + size_t(y) * pitch + x * 2;
          put(x, y, p[0], p[1], 0, 255);
        }
      return true;
    case kFileFormatB5G6R5Unorm:
    case kFileFormatB5G5R5A1Unorm:
    case kFileFormatB4G4R4A4Unorm:
      for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
          const uint16_t v = LoadLE16(data + size_t(y) * pitch + x * 2);
          if (format == kFileFormatB5G6R5Unorm) {
            put(x, y, Expand(v >> 11, 5), Expand((v >> 5) & 63, 6), Expand(v & 31, 5), 255);
          } else if (format == kFileFormatB5G5R5A1Unorm) {
            put(x, y, Expand((v >> 10) & 31, 5), Expand((v >> 5) & 31, 5), Expand(v & 31, 5),
                (v & 0x8000) ? 255 : 0);
          } else {
            put(x, y, Expand((v >> 8) & 15, 4), Expand((v >> 4) & 15, 4), Expand(v & 15, 4),
                Expand(v >> 12, 4));
          }
        }
      return true;
    case kFileFormatBc1Unorm:
    case kFileFormatBc2Unorm:
    case kFileFormatBc3Unorm:
    case kFileFormatBc4Unorm: {
      const uint32_t block_bytes =
          (format == kFileFormatBc1Unorm || format == kFileFormatBc4Unorm) ? 8 : 16;
      for (uint32_t by = 0; by < (height + 3) / 4; ++by)
        for (uint32_t bx = 0; bx < (width + 3) / 4; ++bx) {
          const uint8_t* block = data + size_t(by) * pitch + size_t(bx) * block_bytes;
          uint8_t palette[4][4] = {};
          uint8_t alpha[8] = {};
          const uint8_t* color = block;
          if (format == kFileFormatBc4Unorm) {
            AlphaPalette(block, alpha);
          } else if (format != kFileFormatBc1Unorm) {
            color = block + 8;
            if (format == kFileFormatBc3Unorm) AlphaPalette(block, alpha);
          }
          if (format != kFileFormatBc4Unorm) {
            Bc1Palette(color, format == kFileFormatBc1Unorm, palette);
          }
          const uint32_t indices = uint32_t(color[4]) | uint32_t(color[5]) << 8 |
                                   uint32_t(color[6]) << 16 | uint32_t(color[7]) << 24;
          for (int t = 0; t < 16; ++t) {
            const uint32_t x = bx * 4 + t % 4, y = by * 4 + t / 4;
            if (format == kFileFormatBc4Unorm) {
              const uint8_t v = alpha[AlphaIndex(block, t)];
              put(x, y, v, v, v, 255);
              continue;
            }
            const uint8_t* c = palette[(indices >> (2 * t)) & 3];
            uint8_t a = c[3];
            if (format == kFileFormatBc2Unorm) {
              a = Expand((block[t / 2] >> (4 * (t & 1))) & 15, 4);
            } else if (format == kFileFormatBc3Unorm) {
              a = alpha[AlphaIndex(block, t)];
            }
            put(x, y, c[0], c[1], c[2], a);
          }
        }
      return true;
    }
    default:
      return false;
  }
}

bool DumpBaseLevelPng(const std::filesystem::path& directory, const std::string& stem,
                      const uint8_t* guest_base, const Description& key, uint32_t swizzle) {
  std::vector<uint8_t> pixels;
  uint32_t pitch = 0;
  if (!ReadBaseLevel(guest_base, key, swizzle, pixels, &pitch)) {
    return false;
  }
  std::vector<uint8_t> rgba;
  if (!DecodeToRGBA8(FileFormatFor(key.format), pixels.data(), pitch, key.width, key.height,
                     rgba)) {
    return false;
  }
  GetPngWriter().Add(directory / (stem + ".png"), std::move(rgba), key.width, key.height);
  return true;
}

FileFormat FileFormatFor(xenos::TextureFormat format) { return FileFormatForImpl(format); }

namespace {
// The swizzle's four source channels when it reorders all four, else false.
bool SwizzlePermutation(uint32_t swizzle, uint32_t source[4]) {
  uint32_t seen = 0;
  for (uint32_t c = 0; c < 4; ++c) {
    source[c] = (swizzle >> (3 * c)) & 0b111;
    if (source[c] > 3 || (seen & (1u << source[c]))) {
      return false;
    }
    seen |= 1u << source[c];
  }
  return true;
}
}  // namespace

void SwizzleRGBA8ToFileOrder(uint8_t* data, size_t size, uint32_t swizzle) {
  uint32_t source[4];
  if (swizzle == kIdentitySwizzle || !SwizzlePermutation(swizzle, source)) {
    return;
  }
  for (size_t i = 0; i + 3 < size; i += 4) {
    const uint8_t in[4] = {data[i], data[i + 1], data[i + 2], data[i + 3]};
    for (uint32_t c = 0; c < 4; ++c) {
      data[i + c] = in[source[c]];
    }
  }
}

void SwizzleRGBA8FromFileOrder(uint8_t* data, size_t size, uint32_t swizzle) {
  uint32_t source[4];
  if (swizzle == kIdentitySwizzle || !SwizzlePermutation(swizzle, source)) {
    return;
  }
  for (size_t i = 0; i + 3 < size; i += 4) {
    const uint8_t in[4] = {data[i], data[i + 1], data[i + 2], data[i + 3]};
    for (uint32_t c = 0; c < 4; ++c) {
      data[i + source[c]] = in[c];
    }
  }
}

void SwapGuest16BitChannelsToFileOrder(xenos::TextureFormat format, uint8_t* data, size_t size) {
  // The same swaps as the texture_load_r5g6b5_b5g6r5, r5g5b5a1_b5g5r5a1 and
  // rgba4_bgra4 load shaders.
  uint32_t low_mask, shift;
  switch (format) {
    case xenos::TextureFormat::k_5_6_5:
      low_mask = 0x1F;
      shift = 11;
      break;
    case xenos::TextureFormat::k_1_5_5_5:
      low_mask = 0x1F;
      shift = 10;
      break;
    case xenos::TextureFormat::k_4_4_4_4:
      low_mask = 0xF;
      shift = 8;
      break;
    default:
      return;
  }
  uint32_t high_mask = low_mask << shift;
  for (size_t i = 0; i + 1 < size; i += 2) {
    uint32_t v = uint32_t(data[i]) | (uint32_t(data[i + 1]) << 8);
    v = (v & ~(low_mask | high_mask)) | ((v & low_mask) << shift) | ((v & high_mask) >> shift);
    data[i] = uint8_t(v);
    data[i + 1] = uint8_t(v >> 8);
  }
}

void ConvertB4G4R4A4ToVulkanPack16(uint8_t* data, size_t size) {
  // DXGI B4G4R4A4: B 0-3, G 4-7, R 8-11, A 12-15.
  // VK_FORMAT_B4G4R4A4_UNORM_PACK16: A 0-3, R 4-7, G 8-11, B 12-15.
  for (size_t i = 0; i + 1 < size; i += 2) {
    uint32_t v = uint32_t(data[i]) | (uint32_t(data[i + 1]) << 8);
    v = ((v & 0x0FFF) << 4) | (v >> 12);
    data[i] = uint8_t(v);
    data[i + 1] = uint8_t(v >> 8);
  }
}

namespace {

void FileFormatBlockInfo(FileFormat format, uint32_t& block_width, uint32_t& block_height,
                         uint32_t& bytes_per_block) {
  block_width = block_height = 1;
  switch (format) {
    case kFileFormatBc1Unorm:
    case kFileFormatBc4Unorm:
      block_width = block_height = 4;
      bytes_per_block = 8;
      break;
    case kFileFormatBc2Unorm:
    case kFileFormatBc3Unorm:
    case kFileFormatBc5Unorm:
      block_width = block_height = 4;
      bytes_per_block = 16;
      break;
    case kFileFormatR8G8B8A8Unorm:
    case kFileFormatR16G16Float:
      bytes_per_block = 4;
      break;
    case kFileFormatR16G16B16A16Float:
    case kFileFormatR16G16B16A16Unorm:
      bytes_per_block = 8;
      break;
    case kFileFormatR8G8Unorm:
    case kFileFormatR16Float:
    case kFileFormatB5G6R5Unorm:
    case kFileFormatB5G5R5A1Unorm:
    case kFileFormatB4G4R4A4Unorm:
      bytes_per_block = 2;
      break;
    default:
      bytes_per_block = 1;
      break;
  }
}

bool ReplacementUsableFor(FileFormat replacement, FileFormat guest) {
  if (replacement == guest) {
    return true;
  }
  // Any color texture may be replaced by a plain RGBA8 image (the host
  // formats share the channel order).
  if (replacement == kFileFormatR8G8B8A8Unorm) {
    return guest == kFileFormatBc1Unorm || guest == kFileFormatBc2Unorm ||
           guest == kFileFormatBc3Unorm || guest == kFileFormatB5G6R5Unorm ||
           guest == kFileFormatB5G5R5A1Unorm || guest == kFileFormatB4G4R4A4Unorm;
  }
  return false;
}

uint32_t ReadU32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f) {
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    std::fclose(f);
    return false;
  }
  out.resize(size_t(size));
  bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

// Fills the levels (tightly packed) for the format and size; the data size.
uint32_t LayOutLevels(Replacement& replacement) {
  uint32_t offset = 0;
  for (uint32_t level = 0; level < replacement.mip_count; ++level) {
    Replacement::Level& l = replacement.levels[level];
    l.width = std::max(replacement.width >> level, 1u);
    l.height = std::max(replacement.height >> level, 1u);
    uint32_t blocks_x = (l.width + replacement.block_width - 1) / replacement.block_width;
    uint32_t blocks_y = (l.height + replacement.block_height - 1) / replacement.block_height;
    l.row_pitch = blocks_x * replacement.bytes_per_block;
    l.rows = blocks_y;
    l.offset = offset;
    offset += l.row_pitch * l.rows;
  }
  return offset;
}

bool ParseDds(const std::vector<uint8_t>& file, Replacement& replacement, std::string& error) {
  if (file.size() < 128 || ReadU32(file.data()) != 0x20534444 || ReadU32(file.data() + 4) != 124) {
    error = "not a DDS file";
    return false;
  }
  const uint8_t* h = file.data() + 4;
  uint32_t flags = ReadU32(h + 4);
  uint32_t height = ReadU32(h + 8), width = ReadU32(h + 12);
  uint32_t mip_count = (flags & 0x20000) ? std::max(ReadU32(h + 24), 1u) : 1;
  uint32_t pf_flags = ReadU32(h + 76);
  uint32_t fourcc = ReadU32(h + 80);
  uint32_t bit_count = ReadU32(h + 84);
  uint32_t mask_r = ReadU32(h + 88), mask_g = ReadU32(h + 92), mask_b = ReadU32(h + 96);
  size_t data_offset = 128;
  FileFormat format = kFileFormatNone;
  bool bgra = false;
  if ((pf_flags & 0x4) && fourcc == 0x30315844) {  // "DX10"
    if (file.size() < 148) {
      error = "truncated DX10 header";
      return false;
    }
    uint32_t dxgi = ReadU32(file.data() + 128);
    uint32_t dimension = ReadU32(file.data() + 132);
    uint32_t array_size = ReadU32(file.data() + 140);
    if (dimension != 3 || array_size > 1) {
      error = "only 2D, non-array DDS files are supported";
      return false;
    }
    switch (dxgi) {
      case kFileFormatR8G8B8A8Unorm:
      case 29:  // R8G8B8A8_UNORM_SRGB: taken as linear.
        format = kFileFormatR8G8B8A8Unorm;
        break;
      case 87:  // B8G8R8A8_UNORM
      case 91:
        format = kFileFormatR8G8B8A8Unorm;
        bgra = true;
        break;
      case kFileFormatBc1Unorm:
      case 72:
        format = kFileFormatBc1Unorm;
        break;
      case kFileFormatBc2Unorm:
      case 75:
        format = kFileFormatBc2Unorm;
        break;
      case kFileFormatBc3Unorm:
      case 78:
        format = kFileFormatBc3Unorm;
        break;
      case kFileFormatBc4Unorm:
      case kFileFormatBc5Unorm:
      case kFileFormatR8Unorm:
      case kFileFormatR8G8Unorm:
      case kFileFormatB5G6R5Unorm:
      case kFileFormatB5G5R5A1Unorm:
      case kFileFormatB4G4R4A4Unorm:
      case kFileFormatR16G16B16A16Float:
      case kFileFormatR16G16B16A16Unorm:
      case kFileFormatR16G16Float:
      case kFileFormatR16Float:
        format = FileFormat(dxgi);
        break;
      default:
        error = "unsupported DXGI format " + std::to_string(dxgi);
        return false;
    }
    data_offset = 148;
  } else if (pf_flags & 0x4) {
    switch (fourcc) {
      case 0x31545844:  // DXT1
        format = kFileFormatBc1Unorm;
        break;
      case 0x33545844:  // DXT3
      case 0x32545844:  // DXT2
        format = kFileFormatBc2Unorm;
        break;
      case 0x35545844:  // DXT5
      case 0x34545844:  // DXT4
        format = kFileFormatBc3Unorm;
        break;
      case 0x31495441:  // ATI1
      case 0x55344342:  // BC4U
        format = kFileFormatBc4Unorm;
        break;
      case 0x32495441:  // ATI2
      case 0x55354342:  // BC5U
        format = kFileFormatBc5Unorm;
        break;
      default:
        error = "unsupported DDS FourCC";
        return false;
    }
  } else if ((pf_flags & 0x40) && bit_count == 32 && mask_g == 0x0000FF00) {
    if (mask_r == 0x000000FF && mask_b == 0x00FF0000) {
      format = kFileFormatR8G8B8A8Unorm;
    } else if (mask_r == 0x00FF0000 && mask_b == 0x000000FF) {
      format = kFileFormatR8G8B8A8Unorm;
      bgra = true;
    }
  }
  if (format == kFileFormatNone) {
    error = "unsupported DDS pixel format";
    return false;
  }
  if (!width || !height || width > 16384 || height > 16384) {
    error = "bad size";
    return false;
  }
  replacement.format = format;
  replacement.width = width;
  replacement.height = height;
  uint32_t max_mips = 1;
  while ((std::max(width, height) >> max_mips) > 0 && max_mips < Replacement::kMaxMips) {
    ++max_mips;
  }
  replacement.mip_count = std::min(mip_count, max_mips);
  FileFormatBlockInfo(format, replacement.block_width, replacement.block_height,
                      replacement.bytes_per_block);
  uint32_t data_size = LayOutLevels(replacement);
  if (file.size() < data_offset + data_size) {
    error = "truncated pixel data";
    return false;
  }
  replacement.data.assign(file.begin() + data_offset, file.begin() + data_offset + data_size);
  if (bgra) {
    for (size_t i = 0; i + 3 < replacement.data.size(); i += 4) {
      std::swap(replacement.data[i], replacement.data[i + 2]);
    }
  }
  return true;
}

bool ParsePng(const std::vector<uint8_t>& file, Replacement& replacement, std::string& error) {
  int width = 0, height = 0;
  std::vector<uint8_t> pixels = rex::ui::DecodeImageRGBA(file.data(), file.size(), width, height);
  if (pixels.empty() || width <= 0 || height <= 0) {
    error = "PNG decode failed";
    return false;
  }
  // PNG files hold one level; the mip chain is made here (2x2 boxes), so a
  // replacement doesn't shimmer in the distance.
  replacement = *RGBA8WithMips(std::move(pixels), uint32_t(width), uint32_t(height));
  return true;
}

}  // namespace

std::shared_ptr<Replacement> LoadReplacement(const std::filesystem::path& directory,
                                                   const std::string& stem,
                                                   FileFormat guest_format,
                                                   std::string* error_out) {
  std::string error;
  std::vector<uint8_t> file;
  auto replacement = std::make_shared<Replacement>();
  bool parsed = false;
  std::error_code ec;
  std::filesystem::path dds = directory / (stem + ".dds");
  std::filesystem::path png = directory / (stem + ".png");
  if (std::filesystem::exists(dds, ec)) {
    parsed = ReadFile(dds, file) && ParseDds(file, *replacement, error);
  } else if (std::filesystem::exists(png, ec)) {
    parsed = ReadFile(png, file) && ParsePng(file, *replacement, error);
  } else {
    return nullptr;
  }
  if (parsed && !ReplacementUsableFor(replacement->format, guest_format)) {
    error = "file format " + std::to_string(replacement->format) +
            " can't replace a texture dumped as format " + std::to_string(guest_format);
    parsed = false;
  }
  if (!parsed) {
    if (error_out) {
      *error_out = error.empty() ? "unreadable" : error;
    }
    return nullptr;
  }
  return replacement;
}

namespace {
std::atomic<uint64_t> g_reload_generation{0};
}  // namespace

void RequestReload() {
  g_reload_generation.fetch_add(1);
}

uint64_t ReloadGeneration() {
  return g_reload_generation.load();
}

}  // namespace rex::graphics::texture_files
