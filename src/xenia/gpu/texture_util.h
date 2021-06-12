/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2018 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_TEXTURE_UTIL_H_
#define XENIA_GPU_TEXTURE_UTIL_H_

#include <algorithm>

#include "xenia/base/math.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {
namespace texture_util {

// This namespace replaces texture_extent and most of texture_info for
// simplicity.

// Extracts the size from the fetch constant, and also cleans up addresses and
// mip range based on real presence of the base level and mips. Returns 6 faces
// for cube textures.
void GetSubresourcesFromFetchConstant(
    const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t* width_out,
    uint32_t* height_out, uint32_t* depth_or_faces_out, uint32_t* base_page_out,
    uint32_t* mip_page_out, uint32_t* mip_min_level_out,
    uint32_t* mip_max_level_out,
    xenos::TextureFilter sampler_mip_filter =
        xenos::TextureFilter::kUseFetchConst);

// Gets the number of the mipmap level where the packed mips are stored.
inline uint32_t GetPackedMipLevel(uint32_t width, uint32_t height) {
  uint32_t log2_size = xe::log2_ceil(std::min(width, height));
  return log2_size > 4 ? log2_size - 4 : 0;
}

// Gets the offset of the mipmap within the tail in blocks, or zeros (and
// returns false) if the mip level is not packed. Width, height and depth are in
// texels. For non-3D textures, set depth to 1.
// The offset is always within the dimensions of the image rounded to 32.
bool GetPackedMipOffset(uint32_t width, uint32_t height, uint32_t depth,
                        xenos::TextureFormat format, uint32_t mip,
                        uint32_t& x_blocks, uint32_t& y_blocks,
                        uint32_t& z_blocks);

// Both tiled and linear textures, as it appears from Direct3D 9 texture
// alignment disassembly (where the parameter indicating whether the texture is
// tiled only has effect on aligning the width to max(256 / block size, 32)
// rather than 32), are stored as tiles of 32x1x1 (for 1D), 32x32x1 (for 2D), or
// 32x32x4 (for 3D) texels (or compression blocks for compressed textures) for
// the purpose of calculation of the distance between subresources like array
// slices, and between depth slices (especially for linear textures).
//
// Textures have the base level (level 0) stored under their base_address, and
// mip levels (starting from 1) stored under their mip_address. There are
// differences in how texture data is stored under base_address and mip_address:
// - The base level uses the row pitch (specified in texels divided by 32 - thus
//   implies 32-block alignment for both uncompressed and compressed textures)
//   stored in the fetch constant, and height aligned to 32 blocks for Z slice
//   and array layer stride calculation purposes. The pitch can be different
//   from the actual width - an example is Plants vs. Zombies, using 1408 pitch
//   for a 1280x menu background).
// - The mip levels use `max(next_pow2(width or height in texels) >> level, 1)`
//   aligned to 32 blocks for the same purpose, likely disregarding the pitch
//   from the fetch constant.
//
// There is also mip tail packing if the fetch constant specifies that packed
// mips are enabled, for both tiled and linear textures (Prey uses linear
// DXT-compressed textures with packed mips very extensively for the game world
// materials). In this case, mips with width or height of 16 or smaller are
// stored not individually, but instead, in 32-texel (note: not 32-block - mip
// tail calculations are done with texel units; but 32-block padding can only be
// bigger than 32-texel padding for compressed textures) padding of the last
// level before the packed one.
//
// Note that the mip tail can be used both for the base level and mips (1...) if
// the entire texture has width or height of 16 or smaller. Therefore, both the
// base and the mips would be loaded from a mip tail that would be stored like
// the level 0 of the texture. But, in this case, under base_address and
// mip_address there are two separate mip tails, and the former likely uses the
// pitch from the fetch constant and no power of two size rounding, while for
// the latter the strides are likely calculated like for usual mips. The same
// applies to 17...32 texture sizes, though in this case the base is not packed
// tail, but the mips are still packed within an image that's stored like the
// level 0 of the texture. So, "storage level 0" is an ambiguous concept - host
// texture loading code should distinguish between "base level 0" and "mip tail
// for the mips 1... stored like level 0" and load the actual host level 0 from
// base_address, with all the base addressing properties, and host levels 1...
// from mip_address, with all the mips addressing properties. The base level
// being packed is evident from the function that tiles textures in game
// disassembly, which only checks the flag whether the data is packed passed to
// it, not the level, to see if it needs to calculate the offset in the mip
// tail, and the offset calculation function doesn't have level == 0 checks in
// it, only early-out if level < packed tail level (which can be 0). There are
// examples of textures with packed base, for example, in the intro level of
// Prey (8x8 linear DXT1 - pairs of orange lights in the bottom of gambling
// machines).
//
// Linear texture rows are aligned to 256 bytes, for both the base and the mips
// (for the base, Direct3D 9 writes an already 256-byte-aligned pitch to the
// fetch constant).
//
// However, all the 32x32x4 padding, being just padding, is not necessarily
// being actually accessed, especially for linear textures. Ridge Racer
// Unbounded has a 1280x720 k_8_8_8_8 linear texture, and allocates memory for
// exactly 1280x720, so aligning the height to 32 to 1280x736 results in access
// violations. So, while for stride calculations all the padding must be
// respected, for actual memory loads it's better to avoid trying to access it
// when possible:
// - If the pitch is bigger than the width, it's better to calculate the last
//   row's length from the width rather than the pitch (this also possibly works
//   in the other direction though - pitch < width is a weird situation, but
//   probably legal, and may lead to reading data from beyond the calculated
//   subresource stride).
// - For linear textures (like that 1280x720 example from Ridge Racer
//   Unbounded), it's easy to calculate the exact memory extent that may be
//   accessed knowing the dimensions (unlike for tiled textures with complex
//   addressing within 32x32x4-block tiles), so there's no need to align them to
//   32x32x4 for memory extent calculation.
//   - For the linear packed mip tail, the extent can be calculated as max of
//     (block offsets + block extents) of all levels stored in it.
//
// 1D textures are always linear and likely can't have packed mips (for `width >
// height` textures, mip offset calculation may result in packing along Y).
//
// Array slices are stored within levels (this is different than how Direct3D
// 10+ builds subresource indices, for instance). Each array slice or level is
// aligned to 4 KB (but this doesn't apply to 3D texture slices within one
// level).

struct TextureGuestLayout {
  struct Level {
    // Distance between each row of blocks in bytes, including all the needed
    // power of two (for mips) and 256-byte (for linear textures) alignment.
    uint32_t row_pitch_bytes;
    // Distance between Z slices in block rows, aligned to power of two for
    // mips, and to tile height.
    uint32_t z_slice_stride_block_rows;
    // Distance between each array slice within the level in bytes, aligned to
    // kTextureSubresourceAlignmentBytes. The distance to the next level is this
    // multiplied by the array slice count.
    uint32_t array_slice_stride_bytes;

    // Estimated amount of memory this level occupies, and variables involved in
    // its calculation. Not aligned to kTextureSubresourceAlignmentBytes. For
    // tiled textures, this will be rounded to 32x32x4 blocks (or 32x32x1
    // depending on the dimension), but for the linear subresources, this may be
    // significantly (including less 4 KB pages) smaller than the aligned size
    // (like for Ridge Racer Unbounded where aligning the height of a 1280x720
    // linear texture results in access violations). For the linear mip tail,
    // this includes all the mip levels stored in it. If the width is bigger
    // than the pitch, this will also be taken into account for the last row so
    // all memory actually used by the texture will be loaded, and may be bigger
    // than the distance between array slices or levels. The purpose of this
    // parameter is to make the memory amount that needs to be resident as close
    // to the real amount as possible, to make sure all the needed data will be
    // read, but also, if possible, unneeded memory pages won't be accessed
    // (since that may trigger an access violation on the CPU).
    uint32_t x_extent_blocks;
    uint32_t y_extent_blocks;
    uint32_t z_extent;
    uint32_t array_slice_data_extent_bytes;
    // Including all array slices.
    uint32_t level_data_extent_bytes;
  };

  Level base;
  // If mip_max_level specified at calculation time is at least 1, the stored
  // mips are min(1, packed_mip_level) through min(mip_max_level,
  // packed_mip_level).
  Level mips[xenos::kTexture2DCubeMaxWidthHeightLog2 + 1];
  uint32_t mip_offsets_bytes[xenos::kTexture2DCubeMaxWidthHeightLog2 + 1];
  uint32_t mips_total_extent_bytes;
  uint32_t max_level;
  // UINT32_MAX if there's no packed mip tail.
  uint32_t packed_level;
  uint32_t array_size;
};

TextureGuestLayout GetGuestTextureLayout(
    xenos::DataDimension dimension, uint32_t base_pitch_texels_div_32,
    uint32_t width_texels, uint32_t height_texels, uint32_t depth_or_array_size,
    bool is_tiled, xenos::TextureFormat format, bool has_packed_levels,
    bool has_base, uint32_t max_level);

// Returns the total size of memory the texture uses starting from its base and
// mip addresses, in bytes (both are optional).
void GetTextureTotalSize(xenos::DataDimension dimension,
                         uint32_t base_pitch_texels_div_32,
                         uint32_t width_texels, uint32_t height_texels,
                         uint32_t depth_or_array_size, bool is_tiled,
                         xenos::TextureFormat format, uint32_t mip_max_level,
                         bool has_packed_mips, uint32_t* base_size_out,
                         uint32_t* mip_size_out);

// Notes about tiled addresses that can be useful for simplifying and optimizing
// tiling/untiling:
// - Offset2D(X * 32 + x, Y * 32 + y) ==
//       Offset2D(X * 32, Y * 32) + Offset2D(x, y)
//   (true for negative offsets too).
// - Offset3D(X * 32 + x, Y * 32 + y, Z * 8 + z) ==
//       Offset3D(X * 32, Y * 32, Z * 8) + Offset3D(x, y, z)
//   (true for negative offsets too).
// - 2D 32x32 tiles are laid out linearly.
// - 3D tiled texture slices 0:3 and 4:7 are stored separately in memory, in
//   non-overlapping ranges, but addressing in 4:7 is different than in 0:3.
// - Addressing of blocks that are contiguous along X (for tiling/untiling of
//   larger portions at once):
//   - 1bpb - each 8 blocks are laid out sequentially, odd 8 blocks =
//     even 8 blocks + 64 bytes (two R32G32_UINT tiled accesses for one
//     R32G32B32A32_UINT linear access).
//   - 2bpb, 4bpb, 8bpb, 16bpb - each 16 bytes contain blocks laid out
//     sequentially (can tile/untile in R32G32B32A32_UINT portions).
//   - 2bpb - odd 8 blocks = even 8 blocks + 64 bytes.
//   - 4bpb - odd 4 blocks = even 4 blocks + 32 bytes.
//   - 8bpb - odd 2 blocks = even 2 blocks + 32 bytes.
//   - 16bpb - odd block = even block + 32 bytes.
// - Resolve granularity for both offset and size is 8x8 pixels - see
//   xenos::kResolveAlignmentPixels. So, multiple pixels can still be loaded and
//   stored when resolving, taking the contiguous storage patterns described
//   above into account.

// bytes_per_block_log2 is log2_floor according to how Direct3D 9 calculates it,
// but k_32_32_32 textures are never tiled anyway likely.

int32_t GetTiledOffset2D(int32_t x, int32_t y, uint32_t pitch,
                         uint32_t bytes_per_block_log2);
int32_t GetTiledOffset3D(int32_t x, int32_t y, int32_t z, uint32_t pitch,
                         uint32_t height, uint32_t bytes_per_block_log2);

// Returns four packed TextureSign values swizzled according to the swizzle in
// the fetch constant, so the shader can apply TextureSigns after reading a
// pre-swizzled texture. 0/1 elements are considered unsigned (and not biased),
// however, if all non-constant components are signed, 0/1 are considered signed
// too (because in backends, unsigned and signed textures may use separate host
// textures with different formats, so just one is used for both signed and
// constant components).
uint8_t SwizzleSigns(const xenos::xe_gpu_texture_fetch_t& fetch);
constexpr bool IsAnySignNotSigned(uint8_t packed_signs) {
  return packed_signs != uint32_t(xenos::TextureSign::kSigned) * 0b01010101;
}
constexpr bool IsAnySignSigned(uint8_t packed_signs) {
  // Make signed 00 - check if all are 01, 10 or 11.
  uint32_t xor_signed =
      packed_signs ^ (uint32_t(xenos::TextureSign::kSigned) * 0b01010101);
  return ((xor_signed | (xor_signed >> 1)) & 0b01010101) != 0b01010101;
}

}  // namespace texture_util
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_TEXTURE_UTIL_H_
