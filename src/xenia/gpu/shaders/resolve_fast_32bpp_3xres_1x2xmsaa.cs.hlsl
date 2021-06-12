#include "endian.hlsli"
#define XE_RESOLVE_RESOLUTION_SCALED
#include "resolve.hlsli"

RWBuffer<uint2> xe_resolve_dest : register(u0);
Buffer<uint2> xe_resolve_source : register(t0);

[numthreads(8, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  // 1 thread = 2 guest pixels (a pair is independently addressable as uint2).
  uint2 pixel_index = xe_thread_id.xy << uint2(1u, 0u);
  // Group height is the same as resolve granularity, Y overflow check not
  // needed.
  [branch] if (pixel_index.x >= XeResolveSize().x) {
    return;
  }
  uint host_source_address_int2s =
      (XeEdramOffsetInts(pixel_index + XeResolveOffset(),
                         XeResolveEdramBaseTiles(), XeResolveEdramPitchTiles(),
                         XeResolveEdramMsaaSamples(), XeResolveEdramIsDepth(),
                         0u, XeResolveFirstSampleIndex()) * 9u) >> 1u;
  // abc jkl
  // def mno
  // ghi pqr
  // as:
  // ab|cd|ef|gh|ij|kl|mn|op|qr
  uint4 source_abcd = uint4(xe_resolve_source[host_source_address_int2s],
                            xe_resolve_source[host_source_address_int2s + 1u]);
  uint4 source_efgh = uint4(xe_resolve_source[host_source_address_int2s + 2u],
                            xe_resolve_source[host_source_address_int2s + 3u]);
  uint4 source_ijkl = uint4(xe_resolve_source[host_source_address_int2s + 4u],
                            xe_resolve_source[host_source_address_int2s + 5u]);
  uint4 source_mnop = uint4(xe_resolve_source[host_source_address_int2s + 6u],
                            xe_resolve_source[host_source_address_int2s + 7u]);
  uint2 source_qr = xe_resolve_source[host_source_address_int2s + 8u];
  XeResolveSwap18PixelsRedBlue32bpp(source_abcd, source_efgh, source_ijkl,
                                    source_mnop, source_qr);
  [branch] if (XeResolveEdramDuplicateSecondHostPixel()) {
    if (pixel_index.x == 0u) {
      source_abcd.x = source_abcd.y;
      source_abcd.w = source_efgh.x;
      source_efgh.z = source_efgh.w;
    }
    if (pixel_index.y == 0u) {
      source_abcd.x = source_abcd.w;
      source_abcd.yz = source_efgh.xy;
      source_ijkl.yzw = source_mnop.xyz;
    }
  }
  uint endian = XeResolveDestEndian128();
  uint dest_address = (XeResolveDestPixelAddress(pixel_index, 2u) * 9u) >> 3u;
  source_abcd = XeEndianSwap32(source_abcd, endian);
  source_efgh = XeEndianSwap32(source_efgh, endian);
  source_ijkl = XeEndianSwap32(source_ijkl, endian);
  source_mnop = XeEndianSwap32(source_mnop, endian);
  source_qr = XeEndianSwap32(source_qr, endian);
  xe_resolve_dest[dest_address] = source_abcd.xy;
  xe_resolve_dest[dest_address + 1u] = source_abcd.zw;
  xe_resolve_dest[dest_address + 2u] = source_efgh.xy;
  xe_resolve_dest[dest_address + 3u] = source_efgh.zw;
  xe_resolve_dest[dest_address + 4u] = source_ijkl.xy;
  xe_resolve_dest[dest_address + 5u] = source_ijkl.zw;
  xe_resolve_dest[dest_address + 6u] = source_mnop.xy;
  xe_resolve_dest[dest_address + 7u] = source_mnop.zw;
  xe_resolve_dest[dest_address + 8u] = source_qr;
}