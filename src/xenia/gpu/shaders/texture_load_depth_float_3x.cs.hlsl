#include "pixel_formats.hlsli"
#define XE_TEXTURE_LOAD_32BPB_TRANSFORM(blocks) \
  (XeFloat20e4To32((blocks) >> 8u))
#include "texture_load_32bpb_3x.hlsli"
