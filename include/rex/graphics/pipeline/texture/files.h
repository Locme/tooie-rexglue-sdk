/**
 * Texture dumping and replacement files.
 *
 * A guest texture is named by the format, the size and a hash of its base
 * level's guest bytes: "<format>_<width>x<height>_<hash>" (what the game
 * uploads, not where it lives in memory, so the names are stable between runs
 * and loads). Dumps are DDS files (DX10 header) of the untiled, byte-swapped
 * base level; replacements are DDS or PNG files of the same stem.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <rex/graphics/xenos.h>

namespace rex::graphics::texture_files {

// What the files need to know of a guest texture (from the texture key and
// the guest layout).
struct Description {
  xenos::TextureFormat format;
  xenos::DataDimension dimension;
  xenos::Endian endian;
  uint32_t width;
  uint32_t height;
  uint32_t depth_or_array_size;
  bool tiled;
  // Of the base level, in bytes.
  uint32_t row_pitch_bytes;
};

// DXGI format values the files use (DDS DX10 header); 0 = not supported.
enum FileFormat : uint32_t {
  kFileFormatNone = 0,
  kFileFormatR8G8B8A8Unorm = 28,
  kFileFormatR16G16B16A16Float = 10,
  kFileFormatR16G16B16A16Unorm = 11,
  kFileFormatR16G16Float = 34,
  kFileFormatR16Float = 54,
  kFileFormatR8G8Unorm = 49,
  kFileFormatR8Unorm = 61,
  kFileFormatB5G6R5Unorm = 85,
  kFileFormatB5G5R5A1Unorm = 86,
  kFileFormatB4G4R4A4Unorm = 115,
  kFileFormatBc1Unorm = 71,
  kFileFormatBc2Unorm = 74,
  kFileFormatBc3Unorm = 77,
  kFileFormatBc4Unorm = 80,
  kFileFormatBc5Unorm = 83,
};

// The file format a guest format is dumped as (and that its replacement may
// come in, besides R8G8B8A8 for the color formats).
FileFormat FileFormatFor(xenos::TextureFormat format);

// A texture loaded from a file in place of the guest's, in the host's layout:
// the levels tightly packed, rows of row_pitch bytes.
struct Replacement {
  FileFormat format;
  uint32_t width;
  uint32_t height;
  uint32_t mip_count;
  uint32_t block_width;
  uint32_t block_height;
  uint32_t bytes_per_block;
  struct Level {
    uint32_t width;
    uint32_t height;
    uint32_t offset;
    uint32_t row_pitch;
    uint32_t rows;
  };
  static constexpr uint32_t kMaxMips = 16;
  Level levels[kMaxMips];
  std::vector<uint8_t> data;
  // What the file replaced (the guest data the key's name came from).
  uint64_t guest_hash;
};

// Loads <directory>/<stem>.dds (DX10 or DXT1/3/5, ATI1/2, 32-bit RGBA/BGRA)
// or <directory>/<stem>.png (RGBA8) for a guest texture dumped in
// guest_format. Null if there's no such file, or it isn't usable for that
// format (the caller logs).
std::shared_ptr<Replacement> LoadReplacement(const std::filesystem::path& directory,
                                                   const std::string& stem,
                                                   FileFormat guest_format,
                                                   std::string* error_out);

// The 16-bit color formats are stored in the files in the DXGI channel order
// (what the Direct3D 12 load shaders produce). Converts untiled guest 16-bit
// pixels of a 5_6_5, 1_5_5_5 or 4_4_4_4 texture to that order in place; a no-op
// for the other formats.
void SwapGuest16BitChannelsToFileOrder(xenos::TextureFormat format, uint8_t* data, size_t size);
// Converts B4G4R4A4 (DXGI order) pixels to VK_FORMAT_B4G4R4A4_UNORM_PACK16's
// order in place.
void ConvertB4G4R4A4ToVulkanPack16(uint8_t* data, size_t size);

// Hash of the base level's guest data mixed with the format and size.
uint64_t HashGuestBase(const uint8_t* guest_base, uint32_t guest_base_size,
                       const Description& description);

// "<format>_<width>x<height>_<hash as 16 hex digits>".
std::string FileStem(const Description& description, uint64_t hash);

// Writes <directory>/<stem>.dds from the base level, untiled and byte-swapped.
// False if the format or dimension isn't dumpable (the caller logs once).
bool DumpBaseLevel(const std::filesystem::path& directory, const std::string& stem,
                   const uint8_t* guest_base, const Description& description);

}  // namespace rex::graphics::texture_files
