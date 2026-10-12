#include <rex/graphics/pipeline/texture/files.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
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

bool DumpBaseLevel(const std::filesystem::path& directory, const std::string& stem,
                   const uint8_t* guest_base, const Description& key) {
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
  std::vector<uint8_t> pixels(size_t(out_pitch) * blocks_y);
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

  // DDS: magic, 124-byte header, 20-byte DX10 header, the base level.
  std::vector<uint8_t> file;
  file.reserve(148 + pixels.size());
  WriteU32(file, 0x20534444);  // "DDS "
  WriteU32(file, 124);
  bool compressed = format_info->type == FormatType::kCompressed;
  // CAPS | HEIGHT | WIDTH | PIXELFORMAT | (PITCH or LINEARSIZE)
  WriteU32(file, 0x1 | 0x2 | 0x4 | 0x1000 | (compressed ? 0x80000 : 0x8));
  WriteU32(file, height);
  WriteU32(file, width);
  WriteU32(file, compressed ? uint32_t(pixels.size()) : out_pitch);
  WriteU32(file, 0);  // depth
  WriteU32(file, 1);  // mip count
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
  WriteU32(file, 0x1000);  // caps: TEXTURE
  for (int i = 0; i < 4; ++i) {
    WriteU32(file, 0);  // caps2..4, reserved2
  }
  // DX10 header.
  WriteU32(file, dxgi_format);
  WriteU32(file, 3);  // D3D10_RESOURCE_DIMENSION_TEXTURE2D
  WriteU32(file, 0);  // misc flags
  WriteU32(file, 1);  // array size
  WriteU32(file, 0);  // misc flags 2 (alpha mode unknown)
  file.insert(file.end(), pixels.begin(), pixels.end());

  std::error_code error;
  std::filesystem::create_directories(directory, error);
  std::filesystem::path path = directory / (stem + ".dds");
  FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) {
    return false;
  }
  bool ok = std::fwrite(file.data(), 1, file.size(), f) == file.size();
  std::fclose(f);
  return ok;
}

FileFormat FileFormatFor(xenos::TextureFormat format) { return FileFormatForImpl(format); }

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
  replacement.format = kFileFormatR8G8B8A8Unorm;
  replacement.width = uint32_t(width);
  replacement.height = uint32_t(height);
  replacement.mip_count = 1;
  FileFormatBlockInfo(replacement.format, replacement.block_width, replacement.block_height,
                      replacement.bytes_per_block);
  LayOutLevels(replacement);
  replacement.data = std::move(pixels);
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

}  // namespace rex::graphics::texture_files
