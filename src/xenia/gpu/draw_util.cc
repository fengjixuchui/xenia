/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/draw_util.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/xenos.h"

DEFINE_bool(
    resolve_resolution_scale_duplicate_second_pixel, true,
    "When using resolution scale, apply the hack that duplicates the "
    "right/lower host pixel in the left and top sides of render target resolve "
    "areas to eliminate the gap caused by half-pixel offset (this is necessary "
    "for certain games like GTA IV to work).",
    "GPU");

DEFINE_bool(
    present_rescale, true,
    "Whether to rescale the image, instead of maintaining the original pixel "
    "size, when presenting to the window. When this is disabled, other "
    "positioning options are ignored.",
    "GPU");
DEFINE_bool(
    present_letterbox, true,
    "Maintain aspect ratio when stretching by displaying bars around the image "
    "when there's no more overscan area to crop out.",
    "GPU");
// https://github.com/MonoGame/MonoGame/issues/4697#issuecomment-217779403
// Using the value from DirectXTK (5% cropped out from each side, thus 90%),
// which is not exactly the Xbox One title-safe area, but close, and within the
// action-safe area:
// https://github.com/microsoft/DirectXTK/blob/1e80a465c6960b457ef9ab6716672c1443a45024/Src/SimpleMath.cpp#L144
// XNA TitleSafeArea is 80%, but it's very conservative, designed for CRT, and
// is the title-safe area rather than the action-safe area.
// 90% is also exactly the fraction of 16:9 height in 16:10.
DEFINE_int32(
    present_safe_area_x, 90,
    "Percentage of the image width that can be kept when presenting to "
    "maintain aspect ratio without letterboxing or stretching.",
    "GPU");
DEFINE_int32(
    present_safe_area_y, 90,
    "Percentage of the image height that can be kept when presenting to "
    "maintain aspect ratio without letterboxing or stretching.",
    "GPU");

namespace xe {
namespace gpu {
namespace draw_util {

int32_t FloatToD3D11Fixed16p8(float f32) {
  // https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#3.2.4.1%20FLOAT%20-%3E%20Fixed%20Point%20Integer
  // Early exit tests.
  // n == NaN || n.unbiasedExponent < -f-1 -> 0 . 0
  if (!(std::abs(f32) >= 1.0f / 512.0f)) {
    return 0;
  }
  // n >= (2^(i-1)-2^-f) -> 2^(i-1)-1 . 2^f-1
  if (f32 >= 32768.0f - 1.0f / 256.0f) {
    return (1 << 23) - 1;
  }
  // n <= -2^(i-1) -> -2^(i-1) . 0
  if (f32 <= -32768.0f) {
    return -32768 * 256;
  }
  uint32_t f32_bits = *reinterpret_cast<const uint32_t*>(&f32);
  // Copy float32 mantissa bits [22:0] into corresponding bits [22:0] of a
  // result buffer that has at least 24 bits total storage (before reaching
  // rounding step further below). This includes one bit for the hidden 1.
  // Set bit [23] (float32 hidden bit).
  // Clear bits [31:24].
  union {
    int32_t s;
    uint32_t u;
  } result;
  result.u = (f32_bits & ((1 << 23) - 1)) | (1 << 23);
  // If the sign bit is set in the float32 number (negative), then take the 2's
  // component of the entire set of bits.
  if ((f32_bits >> 31) != 0) {
    result.s = -result.s;
  }
  // Final calculation: extraBits = (mantissa - f) - n.unbiasedExponent
  // (guaranteed to be >= 0).
  int32_t exponent = int32_t((f32_bits >> 23) & 255) - 127;
  uint32_t extra_bits = uint32_t(15 - exponent);
  if (extra_bits) {
    // Round the 32-bit value to a decimal that is extraBits to the left of
    // the LSB end, using nearest-even.
    result.u += (1 << (extra_bits - 1)) - 1 + ((result.u >> extra_bits) & 1);
    // Shift right by extraBits (sign extending).
    result.s >>= extra_bits;
  }
  return result.s;
}

bool IsRasterizationPotentiallyDone(const RegisterFile& regs,
                                    bool primitive_polygonal) {
  // TODO(Triang3l): Investigate ModeControl::kIgnore better, with respect to
  // sample counting. Let's assume sample counting is a part of depth / stencil,
  // thus disabled too.
  xenos::ModeControl edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::ModeControl::kColorDepth &&
      edram_mode != xenos::ModeControl::kDepth) {
    return false;
  }
  if (regs.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode ==
          xenos::VertexShaderExportMode::kMultipass ||
      !regs.Get<reg::RB_SURFACE_INFO>().surface_pitch) {
    return false;
  }
  if (primitive_polygonal) {
    auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    if (pa_su_sc_mode_cntl.cull_front && pa_su_sc_mode_cntl.cull_back) {
      // Both faces are culled.
      return false;
    }
  }
  return true;
}

// https://docs.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_standard_multisample_quality_levels
const int8_t kD3D10StandardSamplePositions2x[2][2] = {{4, 4}, {-4, -4}};
const int8_t kD3D10StandardSamplePositions4x[4][2] = {
    {-2, -6}, {6, -2}, {-6, 2}, {2, 6}};

bool IsPixelShaderNeededWithRasterization(const Shader& shader,
                                          const RegisterFile& regs) {
  assert_true(shader.type() == xenos::ShaderType::kPixel);
  assert_true(shader.is_ucode_analyzed());

  // See xenos::ModeControl for explanation why the pixel shader is only used
  // when it's kColorDepth here.
  if (regs.Get<reg::RB_MODECONTROL>().edram_mode !=
      xenos::ModeControl::kColorDepth) {
    return false;
  }

  // Discarding (explicitly or through alphatest or alpha to coverage) has side
  // effects on pixel counting.
  //
  // Depth output only really matters if depth test is active, but it's used
  // extremely rarely, and pretty much always intentionally - for simplicity,
  // consider it as always mattering.
  //
  // Memory export is an obvious intentional side effect.
  if (shader.kills_pixels() || shader.writes_depth() ||
      shader.is_valid_memexport_used() ||
      (shader.writes_color_target(0) &&
       DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))) {
    return true;
  }

  // Check if a color target is actually written.
  uint32_t rb_color_mask = regs[XE_GPU_REG_RB_COLOR_MASK].u32;
  uint32_t rts_remaining = shader.writes_color_targets();
  uint32_t rt_index;
  while (xe::bit_scan_forward(rts_remaining, &rt_index)) {
    rts_remaining &= ~(uint32_t(1) << rt_index);
    uint32_t format_component_count = GetColorRenderTargetFormatComponentCount(
        regs.Get<reg::RB_COLOR_INFO>(
                reg::RB_COLOR_INFO::rt_register_indices[rt_index])
            .color_format);
    if ((rb_color_mask >> (rt_index * 4)) &
        ((uint32_t(1) << format_component_count) - 1)) {
      return true;
    }
  }

  // Only depth / stencil passthrough potentially.
  return false;
}

void GetHostViewportInfo(const RegisterFile& regs, uint32_t resolution_scale,
                         bool origin_bottom_left, uint32_t x_max,
                         uint32_t y_max, bool allow_reverse_z,
                         bool convert_z_to_float24, bool full_float24_in_0_to_1,
                         bool pixel_shader_writes_depth,
                         ViewportInfo& viewport_info_out) {
  assert_not_zero(resolution_scale);

  // A vertex position goes the following path:
  //
  // = Vertex shader output in clip space, (-w, -w, 0) ... (w, w, w) for
  //   Direct3D or (-w, -w, -w) ... (w, w, w) for OpenGL.
  // > Clipping to the boundaries of the clip space if enabled.
  // > Division by W if not pre-divided.
  // = Normalized device coordinates, (-1, -1, 0) ... (1, 1, 1) for Direct3D or
  //   (-1, -1, -1) ... (1, 1, 1) for OpenGL.
  // > Viewport scaling.
  // > Viewport, window and half-pixel offsetting.
  // = Actual position in render target pixels used for rasterization and depth
  //   buffer coordinates.
  //
  // On modern PC graphics APIs, all drawing is done with clipping enabled (only
  // Z clipping can be replaced with viewport depth range clamping).
  //
  // On the Xbox 360, however, there are two cases:
  //
  // - Clipping is enabled:
  //
  //   Drawing "as normal", primarily for the game world. Draws are clipped to
  //   the (-w, -w, 0) ... (w, w, w) or (-w, -w, -w) ... (w, w, w) clip space.
  //
  //   Ideally all offsets in pixels (window offset, half-pixel offset) are
  //   post-clip, and thus they would need to be applied via the host viewport
  //   (also the Direct3D 11.3 specification defines this as the correct way of
  //   reproducing the original Direct3D 9 half-pixel offset behavior).
  //
  //   However, in reality, only WARP actually truly clips to -W...W, with the
  //   viewport fractional offset actually accurately making samples outside the
  //   fractional rectangle unable to be covered. AMD, Intel and Nvidia, in
  //   Direct3D 12, all don't truly clip even a really huge primitive to -W...W.
  //   Instead, primitives still overflow the fractional rectangle and cover
  //   samples outside of it. The actual viewport scissor is floor(TopLeftX,
  //   TopLeftY) ... floor(TopLeftX + Width, TopLeftY + Height), with flooring
  //   and addition in float32 (with 0x3F7FFFFF TopLeftXY, or 1.0f - ULP, all
  //   the samples in the top row / left column can be covered, while with
  //   0x3F800000, or 1.0f, none of them can be).
  //
  //   We are reproducing the same behavior here - what would happen if we'd be
  //   passing the guest values directly to Direct3D 12. Also, for consistency
  //   across hardware and APIs (especially Vulkan with viewportSubPixelBits
  //   being 0 rather than at least 8 on some devices - Arm Mali, Imagination
  //   PowerVR), and for simplicity of math, and also for exact calculations in
  //   bounds checking in validation layers of the host APIs, we are returning
  //   integer viewport coordinates, handling the fractional offset in the
  //   vertex shaders instead, via ndc_scale and ndc_offset - it shouldn't
  //   significantly affect precision that we will be doing the offsetting in
  //   W-scaled rather than W-divided units, the ratios of exponents involved in
  //   the calculations stay the same, and everything ends up being 16.8 anyway
  //   on most hardware, so small precision differences are very unlikely to
  //   affect coverage.
  //
  // FIXME(Triang3l): Overestimate or more properly round the viewport scissor
  // boundaries if this flooring causes gaps on the bottom / right side in real
  // games if any are found using fractional viewport coordinates. Viewport
  // scissoring is not an inherent result of the viewport scale / offset, these
  // are used merely for transformation of coordinates; rather, it's done by
  // intersecting the viewport and scissor rectangles in the guest driver and
  // writing the common portion to PA_SC_WINDOW_SCISSOR, so how the scissor is
  // computed for a fractional viewport is entirely up to the guest.
  //
  //   Even though Xbox 360 games are designed for Direct3D, with 0...W range of
  //   Z in clip space, the GPU also allows -W...W. Since Xenia is not targeting
  //   OpenGL (where it would be toggled via glClipControl - or, on ES, it would
  //   always be -W...W), this function always remaps it to 0...W, though
  //   numerically not precisely (0 is moved to 0.5, locking the exponent near
  //   what was the truly floating-point 0 originally). It is the guest
  //   viewport's responsibility (haven't checked, but it's logical) to remap
  //   from -1...1 in the NDC to glDepthRange within the 0...1 range. Also -Z
  //   pointing forward in OpenGL doesn't matter here (the -W...W clip space is
  //   symmetric).
  //
  // - Clipping is disabled:
  //
  //   The most common case of drawing without clipping in games is screen-space
  //   draws, most prominently clears, directly in render target coordinates.
  //
  //   In this particular case (though all the general case arithmetic still
  //   applies), the vertex shader returns a position in pixels, pre-divided by
  //   W (though this doesn't matter if W is 1).
  //
  //   Because clipping is disabled, this huge polygon with, for example,
  //   a (1280, 720, 0, 1) vertex, is not clipped to (-w, -w) ... (w, w), so the
  //   vertex becomes (1280, 720) in the NDC as well (even though in regular 3D
  //   draws with clipping, disregarding the guard band for simplicity, it can't
  //   be bigger than (1, 1) after clipping and the division by W).
  //
  //   For these draws, the viewport is also usually disabled (though, again, it
  //   doesn't have to be - an enabled viewport would likely still work as
  //   usual) by disabling PA_CL_VTE_CNTL::VPORT_X/Y/Z_SCALE/OFFSET_ENA - which
  //   equals to having a viewport scale of (1, 1, 1) and offset of (0, 0, 0).
  //   This results in the NDC being treated directly as pixel coordinates.
  //   Normally, with clipping, this would make only a tiny 1x1 area in the
  //   corner of the render target being possible to cover (and 3 unreachable
  //   pixels outside of the render target). The window offset is then applied,
  //   if needed, as well as the half-pixel offset.
  //
  //   It's also possible (though not verified) that without clipping, Z (as a
  //   result of, for instance, polygon offset, or explicit calculations in the
  //   vertex shader) may end up outside the viewport Z range. Direct3D 10
  //   requires clamping to the viewport Z bounds in all cases in the
  //   output-merger according to the Direct3D 11.3 functional specification. A
  //   different behavior is likely on the Xbox 360, however, because while
  //   Direct3D 10-compatible AMD GPUs such as the R600 have
  //   PA_SC_VPORT_ZMIN/ZMAX registers, the Adreno 200 doesn't seem to have any
  //   equivalents, neither in PA nor in RB. This probably also applies to
  //   shader depth output - possibly doesn't need to be clamped as well.
  //
  //   On the PC, we need to emulate disabled clipping by using a viewport at
  //   least as large as the scissor region within the render target, as well as
  //   the full viewport depth range (plus changing Z clipping to Z clamping on
  //   the host if possible), and rescale from the guest clip space to the host
  //   "no clip" clip space, as well as apply the viewport, the window offset,
  //   and the half-pixel offset, in the vertex shader. Ideally, the host
  //   viewport should have a power of 2 size - so scaling doesn't affect
  //   precision, and is merely an exponent bias.
  //
  // NDC XY point towards +XY on the render target - the viewport scale sign
  // handles the remapping from Direct3D 9 -Y towards +U to a generic
  // transformation from the NDC to pixel coordinates.
  //
  // TODO(Triang3l): Investigate the need for clamping of oDepth to 0...1 for
  // D24FS8 as well.

  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto pa_su_vtx_cntl = regs.Get<reg::PA_SU_VTX_CNTL>();

  // Obtain the original viewport values in a normalized way.
  float scale_xy[] = {
      pa_cl_vte_cntl.vport_x_scale_ena ? regs[XE_GPU_REG_PA_CL_VPORT_XSCALE].f32
                                       : 1.0f,
      pa_cl_vte_cntl.vport_y_scale_ena ? regs[XE_GPU_REG_PA_CL_VPORT_YSCALE].f32
                                       : 1.0f,
  };
  float scale_z = pa_cl_vte_cntl.vport_z_scale_ena
                      ? regs[XE_GPU_REG_PA_CL_VPORT_ZSCALE].f32
                      : 1.0f;
  float offset_base_xy[] = {
      pa_cl_vte_cntl.vport_x_offset_ena
          ? regs[XE_GPU_REG_PA_CL_VPORT_XOFFSET].f32
          : 0.0f,
      pa_cl_vte_cntl.vport_y_offset_ena
          ? regs[XE_GPU_REG_PA_CL_VPORT_YOFFSET].f32
          : 0.0f,
  };
  float offset_z = pa_cl_vte_cntl.vport_z_offset_ena
                       ? regs[XE_GPU_REG_PA_CL_VPORT_ZOFFSET].f32
                       : 0.0f;
  // Calculate all the integer.0 or integer.5 offsetting exactly at full
  // precision, separately so it can be used in other integer calculations
  // without double rounding if needed.
  float offset_add_xy[2] = {};
  if (pa_su_sc_mode_cntl.vtx_window_offset_enable) {
    auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    offset_add_xy[0] += float(pa_sc_window_offset.window_x_offset);
    offset_add_xy[1] += float(pa_sc_window_offset.window_y_offset);
  }
  if (cvars::half_pixel_offset && !pa_su_vtx_cntl.pix_center) {
    offset_add_xy[0] += 0.5f;
    offset_add_xy[1] += 0.5f;
  }

  // The maximum value is at least the maximum host render target size anyway -
  // and a guest pixel is always treated as a whole with resolution scaling.
  uint32_t xy_max_unscaled[] = {x_max / resolution_scale,
                                y_max / resolution_scale};
  assert_not_zero(xy_max_unscaled[0]);
  assert_not_zero(xy_max_unscaled[1]);

  float z_min;
  float z_max;
  float ndc_scale[3];
  float ndc_offset[3];

  if (pa_cl_clip_cntl.clip_disable) {
    // Clipping is disabled - use a huge host viewport, perform pixel and depth
    // offsetting in the vertex shader.

    // XY.
    for (uint32_t i = 0; i < 2; ++i) {
      viewport_info_out.xy_offset[i] = 0;
      uint32_t extent_axis_unscaled =
          std::min(xenos::kTexture2DCubeMaxWidthHeight, xy_max_unscaled[i]);
      viewport_info_out.xy_extent[i] = extent_axis_unscaled * resolution_scale;
      float extent_axis_unscaled_float = float(extent_axis_unscaled);
      float pixels_to_ndc_axis = 2.0f / extent_axis_unscaled_float;
      ndc_scale[i] = scale_xy[i] * pixels_to_ndc_axis;
      ndc_offset[i] = (offset_base_xy[i] - extent_axis_unscaled_float * 0.5f +
                       offset_add_xy[i]) *
                      pixels_to_ndc_axis;
    }

    // Z.
    z_min = 0.0f;
    z_max = 1.0f;
    ndc_scale[2] = scale_z;
    ndc_offset[2] = offset_z;
  } else {
    // Clipping is enabled - perform pixel and depth offsetting via the host
    // viewport.

    // XY.
    for (uint32_t i = 0; i < 2; ++i) {
      // With resolution scaling, do all viewport XY scissoring in guest pixels
      // if fractional and for the half-pixel offset - we treat guest pixels as
      // a whole, and also the half-pixel offset would be irreversible in guest
      // vertices if we did flooring in host pixels. Instead of flooring, also
      // doing truncation for simplicity - since maxing with 0 is done anyway
      // (we only return viewports in the positive quarter-plane).
      float offset_axis = offset_base_xy[i] + offset_add_xy[i];
      float scale_axis = scale_xy[i];
      float scale_axis_abs = std::abs(scale_xy[i]);
      float axis_0 = offset_axis - scale_axis_abs;
      float axis_1 = offset_axis + scale_axis_abs;
      float axis_max_unscaled_float = float(xy_max_unscaled[i]);
      // max(0.0f, xy) drops NaN and < 0 - max picks the first argument in the
      // !(a < b) case (always for NaN), min as float (axis_max_unscaled_float
      // is well below 2^24) to safely drop very large values.
      uint32_t axis_0_int =
          uint32_t(std::min(axis_max_unscaled_float, std::max(0.0f, axis_0)));
      uint32_t axis_1_int =
          uint32_t(std::min(axis_max_unscaled_float, std::max(0.0f, axis_1)));
      uint32_t axis_extent_int = axis_1_int - axis_0_int;
      viewport_info_out.xy_offset[i] = axis_0_int * resolution_scale;
      viewport_info_out.xy_extent[i] = axis_extent_int * resolution_scale;
      float ndc_scale_axis;
      float ndc_offset_axis;
      if (axis_extent_int) {
        // Rescale from the old bounds to the new ones, and also apply the sign.
        // If the new bounds are smaller than the old, for instance, we're
        // cropping - the new -W...W clip space is a subregion of the old one -
        // the scale should be > 1 so the area being cut off ends up outside
        // -W...W. If the new region should include more than the original clip
        // space, a region previously outside -W...W should end up within it, so
        // the scale should be < 1.
        float axis_extent_rounded = float(axis_extent_int);
        ndc_scale_axis = scale_axis * 2.0f / axis_extent_rounded;
        // Move the origin of the snapped coordinates back to the original one.
        ndc_offset_axis = (float(offset_axis) -
                           (float(axis_0_int) + axis_extent_rounded * 0.5f)) *
                          2.0f / axis_extent_rounded;
      } else {
        // Empty viewport (everything outside the viewport scissor).
        ndc_scale_axis = 1.0f;
        ndc_offset_axis = 0.0f;
      }
      ndc_scale[i] = ndc_scale_axis;
      ndc_offset[i] = ndc_offset_axis;
    }

    // Z.
    float host_clip_offset_z;
    float host_clip_scale_z;
    if (pa_cl_clip_cntl.dx_clip_space_def) {
      host_clip_offset_z = offset_z;
      host_clip_scale_z = scale_z;
      ndc_scale[2] = 1.0f;
      ndc_offset[2] = 0.0f;
    } else {
      // Normalizing both Direct3D / Vulkan 0...W and OpenGL -W...W clip spaces
      // to 0...W. We are not targeting OpenGL, but there we could accept the
      // wanted clip space (Direct3D, OpenGL, or any) and return the actual one
      // (Direct3D or OpenGL).
      //
      // If the guest wants to use -W...W clip space (-1...1 NDC) and a 0...1
      // depth range in the end, it's expected to use ZSCALE of 0.5 and ZOFFSET
      // of 0.5.
      //
      // We are providing the near and the far (or offset and offset + scale)
      // plane distances to the host API in a way that the near maps to Z = 0
      // and the far maps to Z = W in clip space (or Z = 1 in NDC).
      //
      // With D3D offset and scale that we want, assuming D3D clip space input,
      // the formula for the depth would be:
      //
      // depth = offset_d3d + scale_d3d * ndc_z_d3d
      //
      // We are remapping the incoming OpenGL Z from -W...W to 0...W by scaling
      // it by 0.5 and adding 0.5 * W to the result. So, our depth formula would
      // be:
      //
      // depth = offset_d3d + scale_d3d * (ndc_z_gl * 0.5 + 0.5)
      //
      // The guest registers, however, contain the offset and the scale for
      // remapping not from 0...W to near...far, but from -W...W to near...far,
      // or:
      //
      // depth = offset_gl + scale_gl * ndc_z_gl
      //
      // Knowing offset_gl, scale_gl and how ndc_z_d3d can be obtained from
      // ndc_z_gl, we need to derive the formulas for the needed offset_d3d and
      // scale_d3d to apply them to the incoming ndc_z_d3d.
      //
      // depth = offset_gl + scale_gl * (ndc_z_d3d * 2 - 1)
      //
      // Expanding:
      //
      // depth = offset_gl + (scale_gl * ndc_z_d3d * 2 - scale_gl)
      //
      // Reordering:
      //
      // depth = (offset_gl - scale_gl) + (scale_gl * 2) * ndc_z_d3d
      // offset_d3d = offset_gl - scale_gl
      // scale_d3d = scale_gl * 2
      host_clip_offset_z = offset_z - scale_z;
      host_clip_scale_z = scale_z * 2.0f;
      // Need to remap -W...W clip space to 0...W via ndc_scale and ndc_offset -
      // by scaling Z by 0.5 and adding 0.5 * W to it.
      ndc_scale[2] = 0.5f;
      ndc_offset[2] = 0.5f;
    }
    if (pixel_shader_writes_depth) {
      // Allow the pixel shader to write any depth value since
      // PA_SC_VPORT_ZMIN/ZMAX isn't present on the Adreno 200; guest pixel
      // shaders don't have access to the original Z in the viewport space
      // anyway and likely must write the depth on all execution paths.
      z_min = 0.0f;
      z_max = 1.0f;
    } else {
      // This clamping is not very correct, but just for safety. Direct3D
      // doesn't allow an unrestricted depth range. Vulkan does, as an
      // extension. But cases when this really matters are yet to be found -
      // trying to fix this will result in more correct depth values, but
      // incorrect clipping.
      z_min = xe::saturate_unsigned(host_clip_offset_z);
      z_max = xe::saturate_unsigned(host_clip_offset_z + host_clip_scale_z);
      // Direct3D 12 doesn't allow reverse depth range - on some drivers it
      // works, on some drivers it doesn't, actually, but it was never
      // explicitly allowed by the specification.
      if (!allow_reverse_z && z_min > z_max) {
        std::swap(z_min, z_max);
        ndc_scale[2] = -ndc_scale[2];
        ndc_offset[2] = 1.0f - ndc_offset[2];
      }
    }
  }

  if (GetDepthControlForCurrentEdramMode(regs).z_enable &&
      regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
          xenos::DepthRenderTargetFormat::kD24FS8) {
    if (convert_z_to_float24) {
      // Need to adjust the bounds that the resulting depth values will be
      // clamped to after the pixel shader. Preferring adding some error to
      // interpolated Z instead if conversion can't be done exactly, without
      // modifying clipping bounds by adjusting Z in vertex shaders, as that
      // may cause polygons placed explicitly at Z = 0 or Z = W to be clipped.
      z_min = xenos::Float20e4To32(xenos::Float32To20e4(z_min));
      z_max = xenos::Float20e4To32(xenos::Float32To20e4(z_max));
    }
    if (full_float24_in_0_to_1) {
      // Remap the full [0...2) float24 range to [0...1) support data round-trip
      // during render target ownership transfer of EDRAM tiles through depth
      // input without unrestricted depth range.
      z_min *= 0.5f;
      z_max *= 0.5f;
    }
  }
  viewport_info_out.z_min = z_min;
  viewport_info_out.z_max = z_max;

  if (origin_bottom_left) {
    ndc_scale[1] = -ndc_scale[1];
    ndc_offset[1] = -ndc_offset[1];
  }
  for (uint32_t i = 0; i < 3; ++i) {
    viewport_info_out.ndc_scale[i] = ndc_scale[i];
    viewport_info_out.ndc_offset[i] = ndc_offset[i];
  }
}

void GetScissor(const RegisterFile& regs, Scissor& scissor_out,
                bool clamp_to_surface_pitch) {
  auto pa_sc_window_scissor_tl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
  int32_t tl_x = int32_t(pa_sc_window_scissor_tl.tl_x);
  int32_t tl_y = int32_t(pa_sc_window_scissor_tl.tl_y);
  auto pa_sc_window_scissor_br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  int32_t br_x = int32_t(pa_sc_window_scissor_br.br_x);
  int32_t br_y = int32_t(pa_sc_window_scissor_br.br_y);
  if (!pa_sc_window_scissor_tl.window_offset_disable) {
    auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    tl_x += pa_sc_window_offset.window_x_offset;
    tl_y += pa_sc_window_offset.window_y_offset;
    br_x += pa_sc_window_offset.window_x_offset;
    br_y += pa_sc_window_offset.window_y_offset;
  }
  // Screen scissor is not used by Direct3D 9 (always 0, 0 to 8192, 8192), but
  // still handled here for completeness.
  auto pa_sc_screen_scissor_tl = regs.Get<reg::PA_SC_SCREEN_SCISSOR_TL>();
  tl_x = std::max(tl_x, pa_sc_screen_scissor_tl.tl_x);
  tl_y = std::max(tl_y, pa_sc_screen_scissor_tl.tl_y);
  auto pa_sc_screen_scissor_br = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();
  br_x = std::min(br_x, pa_sc_screen_scissor_br.br_x);
  br_y = std::min(br_y, pa_sc_screen_scissor_br.br_y);
  if (clamp_to_surface_pitch) {
    // Clamp the horizontal scissor to surface_pitch for safety, in case that's
    // not done by the guest for some reason (it's not when doing draws without
    // clipping in Direct3D 9, for instance), to prevent overflow - this is
    // important for host implementations, both based on target-indepedent
    // rasterization without render target width at all (pixel shader
    // interlock-based custom RB implementations) and using conventional render
    // targets, but padded to EDRAM tiles.
    uint32_t surface_pitch = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch;
    tl_x = std::min(tl_x, int32_t(surface_pitch));
    br_x = std::min(br_x, int32_t(surface_pitch));
  }
  // Ensure the rectangle is non-negative, by collapsing it into a 0-sized one
  // (not by reordering the bounds preserving the width / height, which would
  // reveal samples not meant to be covered, unless TL > BR does that on a real
  // console, but no evidence of such has ever been seen), and also drop
  // negative offsets.
  tl_x = std::max(tl_x, int32_t(0));
  tl_y = std::max(tl_y, int32_t(0));
  br_x = std::max(br_x, tl_x);
  br_y = std::max(br_y, tl_y);
  scissor_out.offset[0] = uint32_t(tl_x);
  scissor_out.offset[1] = uint32_t(tl_y);
  scissor_out.extent[0] = uint32_t(br_x - tl_x);
  scissor_out.extent[1] = uint32_t(br_y - tl_y);
}

xenos::CopySampleSelect SanitizeCopySampleSelect(
    xenos::CopySampleSelect copy_sample_select, xenos::MsaaSamples msaa_samples,
    bool is_depth) {
  // Depth can't be averaged.
  if (msaa_samples >= xenos::MsaaSamples::k4X) {
    if (copy_sample_select > xenos::CopySampleSelect::k0123) {
      copy_sample_select = xenos::CopySampleSelect::k0123;
    }
    if (is_depth) {
      switch (copy_sample_select) {
        case xenos::CopySampleSelect::k01:
        case xenos::CopySampleSelect::k0123:
          copy_sample_select = xenos::CopySampleSelect::k0;
          break;
        case xenos::CopySampleSelect::k23:
          copy_sample_select = xenos::CopySampleSelect::k2;
          break;
        default:
          break;
      }
    }
  } else if (msaa_samples >= xenos::MsaaSamples::k2X) {
    switch (copy_sample_select) {
      case xenos::CopySampleSelect::k2:
        copy_sample_select = xenos::CopySampleSelect::k0;
        break;
      case xenos::CopySampleSelect::k3:
        copy_sample_select = xenos::CopySampleSelect::k1;
        break;
      default:
        if (copy_sample_select > xenos::CopySampleSelect::k01) {
          copy_sample_select = xenos::CopySampleSelect::k01;
        }
    }
    if (is_depth && copy_sample_select == xenos::CopySampleSelect::k01) {
      copy_sample_select = xenos::CopySampleSelect::k0;
    }
  } else {
    copy_sample_select = xenos::CopySampleSelect::k0;
  }
  return copy_sample_select;
}

void GetResolveEdramTileSpan(ResolveEdramPackedInfo edram_info,
                             ResolveAddressPackedInfo address_info,
                             uint32_t& base_out, uint32_t& row_length_used_out,
                             uint32_t& rows_out) {
  uint32_t x_scale_log2 =
      3 + uint32_t(edram_info.msaa_samples >= xenos::MsaaSamples::k4X) +
      edram_info.format_is_64bpp;
  uint32_t x0 = (address_info.local_x_div_8 << x_scale_log2) /
                xenos::kEdramTileWidthSamples;
  uint32_t x1 = (((address_info.local_x_div_8 + address_info.width_div_8)
                  << x_scale_log2) +
                 (xenos::kEdramTileWidthSamples - 1)) /
                xenos::kEdramTileWidthSamples;
  uint32_t y_scale_log2 =
      3 + uint32_t(edram_info.msaa_samples >= xenos::MsaaSamples::k2X);
  uint32_t y0 = (address_info.local_y_div_8 << y_scale_log2) /
                xenos::kEdramTileHeightSamples;
  uint32_t y1 = (((address_info.local_y_div_8 + address_info.height_div_8)
                  << y_scale_log2) +
                 (xenos::kEdramTileHeightSamples - 1)) /
                xenos::kEdramTileHeightSamples;
  base_out = edram_info.base_tiles + y0 * edram_info.pitch_tiles + x0;
  row_length_used_out = x1 - x0;
  rows_out = y1 - y0;
}

const ResolveCopyShaderInfo
    resolve_copy_shader_info[size_t(ResolveCopyShaderIndex::kCount)] = {
        {"Resolve Copy Fast 32bpp 1x/2xMSAA", 1, false, 4, 4, 6, 3},
        {"Resolve Copy Fast 32bpp 4xMSAA", 1, false, 4, 4, 6, 3},
        {"Resolve Copy Fast 32bpp 2xRes", 2, false, 4, 4, 4, 3},
        {"Resolve Copy Fast 32bpp 3xRes 1x/2xMSAA", 3, false, 3, 3, 4, 3},
        {"Resolve Copy Fast 32bpp 3xRes 4xMSAA", 3, false, 3, 3, 4, 3},
        {"Resolve Copy Fast 64bpp 1x/2xMSAA", 1, false, 4, 4, 5, 3},
        {"Resolve Copy Fast 64bpp 4xMSAA", 1, false, 3, 4, 5, 3},
        {"Resolve Copy Fast 64bpp 2xRes", 2, false, 4, 4, 3, 3},
        {"Resolve Copy Fast 64bpp 3xRes", 3, false, 3, 3, 3, 3},
        {"Resolve Copy Full 8bpp", 1, true, 2, 3, 6, 3},
        {"Resolve Copy Full 8bpp 2xRes", 2, false, 4, 3, 4, 3},
        {"Resolve Copy Full 8bpp 3xRes", 3, true, 2, 3, 6, 3},
        {"Resolve Copy Full 16bpp", 1, true, 2, 3, 5, 3},
        {"Resolve Copy Full 16bpp 2xRes", 2, false, 4, 3, 3, 3},
        {"Resolve Copy Full 16bpp from 32bpp 3xRes", 3, true, 2, 3, 5, 3},
        {"Resolve Copy Full 16bpp from 64bpp 3xRes", 3, false, 3, 3, 5, 3},
        {"Resolve Copy Full 32bpp", 1, true, 2, 4, 5, 3},
        {"Resolve Copy Full 32bpp 2xRes", 2, false, 4, 4, 3, 3},
        {"Resolve Copy Full 32bpp from 32bpp 3xRes", 3, true, 2, 3, 4, 3},
        {"Resolve Copy Full 32bpp from 64bpp 3xRes", 3, false, 3, 3, 4, 3},
        {"Resolve Copy Full 64bpp", 1, true, 2, 4, 5, 3},
        {"Resolve Copy Full 64bpp 2xRes", 2, false, 4, 4, 3, 3},
        {"Resolve Copy Full 64bpp from 32bpp 3xRes", 3, true, 2, 3, 3, 3},
        {"Resolve Copy Full 64bpp from 64bpp 3xRes", 3, false, 3, 3, 3, 3},
        {"Resolve Copy Full 128bpp", 1, true, 2, 4, 4, 3},
        {"Resolve Copy Full 128bpp 2xRes", 2, false, 4, 4, 3, 3},
        {"Resolve Copy Full 128bpp from 32bpp 3xRes", 3, true, 2, 4, 3, 3},
        {"Resolve Copy Full 128bpp from 64bpp 3xRes", 3, false, 3, 4, 3, 3},
};

bool GetResolveInfo(const RegisterFile& regs, const Memory& memory,
                    TraceWriter& trace_writer, uint32_t resolution_scale,
                    bool fixed_16_truncated_to_minus_1_to_1,
                    ResolveInfo& info_out) {
  auto rb_copy_control = regs.Get<reg::RB_COPY_CONTROL>();
  info_out.rb_copy_control = rb_copy_control;

  if (rb_copy_control.copy_command != xenos::CopyCommand::kRaw &&
      rb_copy_control.copy_command != xenos::CopyCommand::kConvert) {
    XELOGE(
        "Unsupported resolve copy command {}. Report the game to Xenia "
        "developers",
        uint32_t(rb_copy_control.copy_command));
    assert_always();
    return false;
  }

  // Don't pass uninitialized values to shaders, not to leak data to frame
  // captures.
  info_out.address.packed = 0;

  // Get the extent of pixels covered by the resolve rectangle, according to the
  // top-left rasterization rule.
  // D3D9 HACK: Vertices to use are always in vf0, and are written by the CPU.
  auto fetch = regs.Get<xenos::xe_gpu_vertex_fetch_t>(
      XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0);
  if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 3 * 2) {
    XELOGE("Unsupported resolve vertex buffer format");
    assert_always();
    return false;
  }
  trace_writer.WriteMemoryRead(fetch.address * sizeof(uint32_t),
                               fetch.size * sizeof(uint32_t));
  const float* vertices_guest = reinterpret_cast<const float*>(
      memory.TranslatePhysical(fetch.address * sizeof(uint32_t)));
  // Most vertices have a negative half-pixel offset applied, which we reverse.
  float half_pixel_offset =
      regs.Get<reg::PA_SU_VTX_CNTL>().pix_center ? 0.0f : 0.5f;
  int32_t vertices_fixed[6];
  for (size_t i = 0; i < xe::countof(vertices_fixed); ++i) {
    vertices_fixed[i] = FloatToD3D11Fixed16p8(
        xenos::GpuSwap(vertices_guest[i], fetch.endian) + half_pixel_offset);
  }
  // Inclusive.
  int32_t x0 = std::min(std::min(vertices_fixed[0], vertices_fixed[2]),
                        vertices_fixed[4]);
  int32_t y0 = std::min(std::min(vertices_fixed[1], vertices_fixed[3]),
                        vertices_fixed[5]);
  // Exclusive.
  int32_t x1 = std::max(std::max(vertices_fixed[0], vertices_fixed[2]),
                        vertices_fixed[4]);
  int32_t y1 = std::max(std::max(vertices_fixed[1], vertices_fixed[3]),
                        vertices_fixed[5]);
  // Top-left - include .5 (0.128 treated as 0 covered, 0.129 as 0 not covered).
  x0 = (x0 + 127) >> 8;
  y0 = (y0 + 127) >> 8;
  // Bottom-right - exclude .5.
  x1 = (x1 + 127) >> 8;
  y1 = (y1 + 127) >> 8;

  auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();

  // Apply the window offset to the vertices.
  if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    x0 += pa_sc_window_offset.window_x_offset;
    y0 += pa_sc_window_offset.window_y_offset;
    x1 += pa_sc_window_offset.window_x_offset;
    y1 += pa_sc_window_offset.window_y_offset;
  }

  // Apply the scissor and prevent negative origin (behind the EDRAM base).
  Scissor scissor;
  // False because clamping to the surface pitch will be done later (it will be
  // aligned to the resolve alignment here, for resolving from render targets
  // with a pitch that is not a multiple of 8).
  GetScissor(regs, scissor, false);
  int32_t scissor_right = int32_t(scissor.offset[0] + scissor.extent[0]);
  int32_t scissor_bottom = int32_t(scissor.offset[1] + scissor.extent[1]);
  x0 = xe::clamp(x0, int32_t(scissor.offset[0]), scissor_right);
  y0 = xe::clamp(y0, int32_t(scissor.offset[1]), scissor_bottom);
  x1 = xe::clamp(x1, int32_t(scissor.offset[0]), scissor_right);
  y1 = xe::clamp(y1, int32_t(scissor.offset[1]), scissor_bottom);

  assert_true(x0 <= x1 && y0 <= y1);

  // Direct3D 9's D3DDevice_Resolve internally rounds the right/bottom of the
  // rectangle internally to 8. While all the alignment should have already been
  // done by Direct3D 9, just for safety of host implementation of resolve,
  // force-align the rectangle by expanding (D3D9 expands to the right/bottom
  // for some reason, haven't found how left/top is rounded, but logically it
  // would make sense to expand to the left/top too).
  x0 &= ~int32_t(xenos::kResolveAlignmentPixels - 1);
  y0 &= ~int32_t(xenos::kResolveAlignmentPixels - 1);
  x1 = xe::align(x1, int32_t(xenos::kResolveAlignmentPixels));
  y1 = xe::align(y1, int32_t(xenos::kResolveAlignmentPixels));

  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  if (rb_surface_info.msaa_samples > xenos::MsaaSamples::k4X) {
    // Safety check because a lot of code assumes up to 4x.
    assert_always();
    XELOGE(
        "{}x MSAA requested by the guest in a resolve, Xenos only supports up "
        "to 4x",
        uint32_t(1) << uint32_t(rb_surface_info.msaa_samples));
    return false;
  }

  // Clamp to the EDRAM surface pitch (maximum possible surface pitch is also
  // assumed to be the largest resolvable size).
  int32_t surface_pitch_aligned =
      int32_t(rb_surface_info.surface_pitch &
              ~uint32_t(xenos::kResolveAlignmentPixels - 1));
  if (x1 > surface_pitch_aligned) {
    XELOGE("Resolve region {} <= x < {} is outside the surface pitch {}", x0,
           x1, surface_pitch_aligned);
    x0 = std::min(x0, surface_pitch_aligned);
    x1 = std::min(x1, surface_pitch_aligned);
  }
  assert_true(x1 - x0 <= int32_t(xenos::kMaxResolveSize));

  // Clamp the height to a sane value (to make sure it can fit in the packed
  // shader constant).
  if (y1 - y0 > int32_t(xenos::kMaxResolveSize)) {
    XELOGE("Resolve region {} <= y < {} is taller than {}", y0, y1,
           xenos::kMaxResolveSize);
    y1 = y0 + int32_t(xenos::kMaxResolveSize);
  }

  if (x0 >= x1 || y0 >= y1) {
    XELOGE("Resolve region is empty");
  }

  assert_true(x0 <= x1 && y0 <= y1);
  info_out.address.width_div_8 =
      uint32_t(x1 - x0) >> xenos::kResolveAlignmentPixelsLog2;
  info_out.address.height_div_8 =
      uint32_t(y1 - y0) >> xenos::kResolveAlignmentPixelsLog2;

  // Handle the destination.
  bool is_depth =
      rb_copy_control.copy_src_select >= xenos::kMaxColorRenderTargets;
  // Get the sample selection to safely pass to the shader.
  xenos::CopySampleSelect sample_select =
      SanitizeCopySampleSelect(rb_copy_control.copy_sample_select,
                               rb_surface_info.msaa_samples, is_depth);
  if (rb_copy_control.copy_sample_select != sample_select) {
    XELOGW(
        "Incorrect resolve sample selected for {}-sample {}: {}, treating like "
        "{}",
        1 << uint32_t(rb_surface_info.msaa_samples),
        is_depth ? "depth" : "color", rb_copy_control.copy_sample_select,
        sample_select);
  }
  info_out.address.copy_sample_select = sample_select;
  // Get the format to pass to the shader in a unified way - for depth (for
  // which Direct3D 9 specifies the k_8_8_8_8 destination format), make sure the
  // shader won't try to do conversion - pass proper k_24_8 or k_24_8_FLOAT.
  auto rb_copy_dest_info = regs.Get<reg::RB_COPY_DEST_INFO>();
  xenos::TextureFormat dest_format;
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  if (is_depth) {
    dest_format = DepthRenderTargetToTextureFormat(rb_depth_info.depth_format);
  } else {
    dest_format = xenos::TextureFormat(rb_copy_dest_info.copy_dest_format);
    // For development feedback - not much known about these formats currently.
    xenos::TextureFormat dest_closest_format;
    switch (dest_format) {
      case xenos::TextureFormat::k_8_A:
      case xenos::TextureFormat::k_8_B:
        dest_closest_format = xenos::TextureFormat::k_8;
        break;
      case xenos::TextureFormat::k_8_8_8_8_A:
        dest_closest_format = xenos::TextureFormat::k_8_8_8_8;
        break;
      default:
        dest_closest_format = dest_format;
    }
    if (dest_format != dest_closest_format) {
      XELOGW(
          "Resolving to format {}, which is untested - treating like {}. "
          "Report the game to Xenia developers!",
          FormatInfo::Get(dest_format)->name,
          FormatInfo::Get(dest_closest_format)->name);
    }
  }

  // Calculate the destination memory extent.
  uint32_t rb_copy_dest_base = regs[XE_GPU_REG_RB_COPY_DEST_BASE].u32;
  uint32_t copy_dest_base_adjusted = rb_copy_dest_base;
  uint32_t copy_dest_length;
  auto rb_copy_dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
  uint32_t copy_dest_pitch_aligned_div_32 =
      (rb_copy_dest_pitch.copy_dest_pitch +
       (xenos::kTextureTileWidthHeight - 1)) >>
      xenos::kTextureTileWidthHeightLog2;
  info_out.copy_dest_pitch_aligned.pitch_aligned_div_32 =
      copy_dest_pitch_aligned_div_32;
  info_out.copy_dest_pitch_aligned.height_aligned_div_32 =
      (rb_copy_dest_pitch.copy_dest_height +
       (xenos::kTextureTileWidthHeight - 1)) >>
      xenos::kTextureTileWidthHeightLog2;
  const FormatInfo& dest_format_info = *FormatInfo::Get(dest_format);
  if (is_depth || dest_format_info.type == FormatType::kResolvable) {
    uint32_t bpp_log2 = xe::log2_floor(dest_format_info.bits_per_pixel >> 3);
    xenos::DataDimension dest_dimension;
    uint32_t dest_height, dest_depth;
    if (rb_copy_dest_info.copy_dest_array) {
      // The pointer is already adjusted to the Z / 8 (copy_dest_slice is
      // 3-bit).
      copy_dest_base_adjusted += texture_util::GetTiledOffset3D(
          x0 & ~int32_t(xenos::kTextureTileWidthHeight - 1),
          y0 & ~int32_t(xenos::kTextureTileWidthHeight - 1), 0,
          rb_copy_dest_pitch.copy_dest_pitch,
          rb_copy_dest_pitch.copy_dest_height, bpp_log2);
      dest_dimension = xenos::DataDimension::k3D;
      dest_height = rb_copy_dest_pitch.copy_dest_height;
      // The pointer is only adjusted to Z / 8, but the texture may have a depth
      // of (N % 8) <= 4, like 4, 12, 20 when rounded up to 4
      // (xenos::kTextureTiledDepthGranularity), so provide Z + 1 to measure the
      // size of the texture conservatively, but without going out of the upper
      // bound (though this still may go out of bounds a bit probably if
      // resolving to non-zero XY, but not sure if that really happens and
      // actually causes issues).
      dest_depth = rb_copy_dest_info.copy_dest_slice + 1;
    } else {
      copy_dest_base_adjusted += texture_util::GetTiledOffset2D(
          x0 & ~int32_t(xenos::kTextureTileWidthHeight - 1),
          y0 & ~int32_t(xenos::kTextureTileWidthHeight - 1),
          rb_copy_dest_pitch.copy_dest_pitch, bpp_log2);
      dest_dimension = xenos::DataDimension::k2DOrStacked;
      // RB_COPY_DEST_PITCH::copy_dest_height is the real texture height used
      // for 3D texture pitch, it's not relative to 0,0 of the coordinate space
      // (in Halo 3, the sniper rifle scope has copy_dest_height of 192, but the
      // rectangle's Y is 64...256) - provide the real height of the rectangle
      // since 32x32 tiles are stored linearly anyway. In addition, the height
      // in RB_COPY_DEST_PITCH may be larger than needed - in Red Dead
      // Redemption, a UI texture for the letterbox bars alpha is located within
      // the range of a 1280x720 resolve target, so with resolution scaling it's
      // also wrongly detected as scaled, while only 1280x208 is being resolved.
      dest_height = uint32_t(y1 - y0);
      dest_depth = 1;
    }
    // Need a subregion size, not the full subresource size - thus not aligning
    // to xenos::kTextureSubresourceAlignmentBytes.
    copy_dest_length =
        texture_util::GetGuestTextureLayout(
            dest_dimension, copy_dest_pitch_aligned_div_32, uint32_t(x1 - x0),
            dest_height, dest_depth, true, dest_format, false, true, 0)
            .base.level_data_extent_bytes;
  } else {
    XELOGE("Tried to resolve to format {}, which is not a ColorFormat",
           dest_format_info.name);
    copy_dest_length = 0;
  }
  info_out.copy_dest_base = copy_dest_base_adjusted;
  info_out.copy_dest_length = copy_dest_length;

  // Offset to 160x32 (a multiple of both the EDRAM tile size and the texture
  // tile size), so the whole offset can be stored in a very small number of
  // bits, with bases adjusted instead. The destination pointer is already
  // offset.
  uint32_t local_offset_x = uint32_t(x0) % 160;
  uint32_t local_offset_y = uint32_t(y0) & 31;
  info_out.address.local_x_div_8 =
      local_offset_x >> xenos::kResolveAlignmentPixelsLog2;
  info_out.address.local_y_div_8 =
      local_offset_y >> xenos::kResolveAlignmentPixelsLog2;
  uint32_t base_offset_x_samples =
      (uint32_t(x0) - local_offset_x)
      << uint32_t(rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X);
  uint32_t base_offset_x_tiles =
      (base_offset_x_samples + (xenos::kEdramTileWidthSamples - 1)) /
      xenos::kEdramTileWidthSamples;
  uint32_t base_offset_y_samples =
      (uint32_t(y0) - local_offset_y)
      << uint32_t(rb_surface_info.msaa_samples >= xenos::MsaaSamples::k2X);
  uint32_t base_offset_y_tiles =
      (base_offset_y_samples + (xenos::kEdramTileHeightSamples - 1)) /
      xenos::kEdramTileHeightSamples;
  uint32_t surface_pitch_tiles = xenos::GetSurfacePitchTiles(
      rb_surface_info.surface_pitch, rb_surface_info.msaa_samples, false);
  uint32_t edram_base_offset_tiles =
      base_offset_y_tiles * surface_pitch_tiles + base_offset_x_tiles;

  // Write the color/depth EDRAM info.
  bool duplicate_second_pixel =
      resolution_scale > 1 &&
      cvars::resolve_resolution_scale_duplicate_second_pixel &&
      cvars::half_pixel_offset && !regs.Get<reg::PA_SU_VTX_CNTL>().pix_center;
  int32_t exp_bias = is_depth ? 0 : rb_copy_dest_info.copy_dest_exp_bias;
  ResolveEdramPackedInfo depth_edram_info;
  depth_edram_info.packed = 0;
  if (is_depth || rb_copy_control.depth_clear_enable) {
    depth_edram_info.pitch_tiles = surface_pitch_tiles;
    depth_edram_info.msaa_samples = rb_surface_info.msaa_samples;
    depth_edram_info.is_depth = 1;
    depth_edram_info.base_tiles =
        rb_depth_info.depth_base + edram_base_offset_tiles;
    depth_edram_info.format = uint32_t(rb_depth_info.depth_format);
    depth_edram_info.format_is_64bpp = 0;
    depth_edram_info.duplicate_second_pixel = uint32_t(duplicate_second_pixel);
    info_out.depth_original_base = rb_depth_info.depth_base;
  } else {
    info_out.depth_original_base = 0;
  }
  info_out.depth_edram_info = depth_edram_info;
  ResolveEdramPackedInfo color_edram_info;
  color_edram_info.packed = 0;
  if (!is_depth) {
    // Color.
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[rb_copy_control
                                                    .copy_src_select]);
    uint32_t is_64bpp = uint32_t(
        xenos::IsColorRenderTargetFormat64bpp(color_info.color_format));
    color_edram_info.pitch_tiles = surface_pitch_tiles << is_64bpp;
    color_edram_info.msaa_samples = rb_surface_info.msaa_samples;
    color_edram_info.is_depth = 0;
    color_edram_info.base_tiles =
        color_info.color_base + (edram_base_offset_tiles << is_64bpp);
    color_edram_info.format = uint32_t(color_info.color_format);
    color_edram_info.format_is_64bpp = is_64bpp;
    color_edram_info.duplicate_second_pixel = uint32_t(duplicate_second_pixel);
    if (fixed_16_truncated_to_minus_1_to_1 &&
        (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
         color_info.color_format ==
             xenos::ColorRenderTargetFormat::k_16_16_16_16)) {
      // The texture expects 0x8001 = -32, 0x7FFF = 32, but the hack making
      // 0x8001 = -1, 0x7FFF = 1 is used - revert (this won't be correct if the
      // requested exponent bias is 27 or above, but it's a hack anyway, no need
      // to create a new copy info structure with one more bit just for this).
      exp_bias = std::min(exp_bias + int32_t(5), int32_t(31));
    }
    info_out.color_original_base = color_info.color_base;
  } else {
    info_out.color_original_base = 0;
  }
  info_out.color_edram_info = color_edram_info;

  // Patch and write RB_COPY_DEST_INFO.
  info_out.copy_dest_info = rb_copy_dest_info;
  // Override with the depth format to make sure the shader doesn't have any
  // reason to try to do k_8_8_8_8 packing.
  info_out.copy_dest_info.copy_dest_format = xenos::ColorFormat(dest_format);
  // Handle k_16_16 and k_16_16_16_16 range.
  info_out.copy_dest_info.copy_dest_exp_bias = exp_bias;
  if (is_depth) {
    // Single component, nothing to swap.
    info_out.copy_dest_info.copy_dest_swap = false;
  }

  info_out.rb_depth_clear = regs[XE_GPU_REG_RB_DEPTH_CLEAR].u32;
  info_out.rb_color_clear = regs[XE_GPU_REG_RB_COLOR_CLEAR].u32;
  info_out.rb_color_clear_lo = regs[XE_GPU_REG_RB_COLOR_CLEAR_LO].u32;

  XELOGD(
      "Resolve: {},{} <= x,y < {},{}, {} -> {} at 0x{:08X} (first tile at "
      "0x{:08X}, length 0x{:08X})",
      x0, y0, x1, y1,
      is_depth ? xenos::GetDepthRenderTargetFormatName(
                     xenos::DepthRenderTargetFormat(depth_edram_info.format))
               : xenos::GetColorRenderTargetFormatName(
                     xenos::ColorRenderTargetFormat(color_edram_info.format)),
      dest_format_info.name, rb_copy_dest_base, copy_dest_base_adjusted,
      copy_dest_length);

  return true;
}

ResolveCopyShaderIndex ResolveInfo::GetCopyShader(
    uint32_t resolution_scale, ResolveCopyShaderConstants& constants_out,
    uint32_t& group_count_x_out, uint32_t& group_count_y_out) const {
  ResolveCopyShaderIndex shader = ResolveCopyShaderIndex::kUnknown;
  bool is_depth = IsCopyingDepth();
  ResolveEdramPackedInfo edram_info =
      is_depth ? depth_edram_info : color_edram_info;
  bool source_is_64bpp = !is_depth && color_edram_info.format_is_64bpp != 0;
  if (is_depth ||
      (!copy_dest_info.copy_dest_exp_bias &&
       xenos::IsSingleCopySampleSelected(address.copy_sample_select) &&
       xenos::IsColorResolveFormatBitwiseEquivalent(
           xenos::ColorRenderTargetFormat(color_edram_info.format),
           xenos::ColorFormat(copy_dest_info.copy_dest_format)))) {
    switch (resolution_scale) {
      case 1:
        if (edram_info.msaa_samples >= xenos::MsaaSamples::k4X) {
          shader = source_is_64bpp ? ResolveCopyShaderIndex::kFast64bpp4xMSAA
                                   : ResolveCopyShaderIndex::kFast32bpp4xMSAA;
        } else {
          shader = source_is_64bpp ? ResolveCopyShaderIndex::kFast64bpp1x2xMSAA
                                   : ResolveCopyShaderIndex::kFast32bpp1x2xMSAA;
        }
        break;
      case 2:
        shader = source_is_64bpp ? ResolveCopyShaderIndex::kFast64bpp2xRes
                                 : ResolveCopyShaderIndex::kFast32bpp2xRes;
        break;
      case 3:
        if (source_is_64bpp) {
          shader = ResolveCopyShaderIndex::kFast64bpp3xRes;
        } else {
          shader = edram_info.msaa_samples >= xenos::MsaaSamples::k4X
                       ? ResolveCopyShaderIndex::kFast32bpp3xRes4xMSAA
                       : ResolveCopyShaderIndex::kFast32bpp3xRes1x2xMSAA;
        }
        break;
      default:
        assert_unhandled_case(resolution_scale);
    }
  } else {
    const FormatInfo& dest_format_info =
        *FormatInfo::Get(xenos::TextureFormat(copy_dest_info.copy_dest_format));
    switch (resolution_scale) {
      case 1:
        switch (dest_format_info.bits_per_pixel) {
          case 8:
            shader = ResolveCopyShaderIndex::kFull8bpp;
            break;
          case 16:
            shader = ResolveCopyShaderIndex::kFull16bpp;
            break;
          case 32:
            shader = ResolveCopyShaderIndex::kFull32bpp;
            break;
          case 64:
            shader = ResolveCopyShaderIndex::kFull64bpp;
            break;
          case 128:
            shader = ResolveCopyShaderIndex::kFull128bpp;
            break;
          default:
            assert_unhandled_case(dest_format_info.bits_per_pixel);
        }
        break;
      case 2:
        switch (dest_format_info.bits_per_pixel) {
          case 8:
            shader = ResolveCopyShaderIndex::kFull8bpp2xRes;
            break;
          case 16:
            shader = ResolveCopyShaderIndex::kFull16bpp2xRes;
            break;
          case 32:
            shader = ResolveCopyShaderIndex::kFull32bpp2xRes;
            break;
          case 64:
            shader = ResolveCopyShaderIndex::kFull64bpp2xRes;
            break;
          case 128:
            shader = ResolveCopyShaderIndex::kFull128bpp2xRes;
            break;
          default:
            assert_unhandled_case(dest_format_info.bits_per_pixel);
        }
        break;
      case 3:
        switch (dest_format_info.bits_per_pixel) {
          case 8:
            shader = ResolveCopyShaderIndex::kFull8bpp3xRes;
            break;
          case 16:
            shader = source_is_64bpp
                         ? ResolveCopyShaderIndex::kFull16bppFrom64bpp3xRes
                         : ResolveCopyShaderIndex::kFull16bppFrom32bpp3xRes;
            break;
          case 32:
            shader = source_is_64bpp
                         ? ResolveCopyShaderIndex::kFull32bppFrom64bpp3xRes
                         : ResolveCopyShaderIndex::kFull32bppFrom32bpp3xRes;
            break;
          case 64:
            shader = source_is_64bpp
                         ? ResolveCopyShaderIndex::kFull64bppFrom64bpp3xRes
                         : ResolveCopyShaderIndex::kFull64bppFrom32bpp3xRes;
            break;
          case 128:
            shader = source_is_64bpp
                         ? ResolveCopyShaderIndex::kFull128bppFrom64bpp3xRes
                         : ResolveCopyShaderIndex::kFull128bppFrom32bpp3xRes;
            break;
          default:
            assert_unhandled_case(dest_format_info.bits_per_pixel);
        }
        break;
      default:
        assert_unhandled_case(resolution_scale);
    }
  }

  constants_out.dest_relative.edram_info = edram_info;
  constants_out.dest_relative.address_info = address;
  constants_out.dest_relative.dest_info = copy_dest_info;
  constants_out.dest_relative.dest_pitch_aligned = copy_dest_pitch_aligned;
  constants_out.dest_base = copy_dest_base;

  if (shader != ResolveCopyShaderIndex::kUnknown) {
    uint32_t width = address.width_div_8 << xenos::kResolveAlignmentPixelsLog2;
    uint32_t height = address.height_div_8
                      << xenos::kResolveAlignmentPixelsLog2;
    const ResolveCopyShaderInfo& shader_info =
        resolve_copy_shader_info[size_t(shader)];
    group_count_x_out = (width + ((1 << shader_info.group_size_x_log2) - 1)) >>
                        shader_info.group_size_x_log2;
    group_count_y_out = (height + ((1 << shader_info.group_size_y_log2) - 1)) >>
                        shader_info.group_size_y_log2;
  } else {
    XELOGE("No resolve copy compute shader for the provided configuration");
    assert_always();
    group_count_x_out = 0;
    group_count_y_out = 0;
  }

  return shader;
}

void GetPresentArea(uint32_t source_width, uint32_t source_height,
                    uint32_t window_width, uint32_t window_height,
                    int32_t& target_x_out, int32_t& target_y_out,
                    uint32_t& target_width_out, uint32_t& target_height_out) {
  if (!cvars::present_rescale) {
    target_x_out = (int32_t(window_width) - int32_t(source_width)) / 2;
    target_y_out = (int32_t(window_height) - int32_t(source_height)) / 2;
    target_width_out = source_width;
    target_height_out = source_height;
    return;
  }
  // Prevent division by zero.
  if (!source_width || !source_height) {
    target_x_out = 0;
    target_y_out = 0;
    target_width_out = 0;
    target_height_out = 0;
    return;
  }
  if (uint64_t(window_width) * source_height >
      uint64_t(source_width) * window_height) {
    // The window is wider that the source - crop along Y, then letterbox or
    // stretch along X.
    uint32_t present_safe_area;
    if (cvars::present_safe_area_y > 0 && cvars::present_safe_area_y < 100) {
      present_safe_area = uint32_t(cvars::present_safe_area_y);
    } else {
      present_safe_area = 100;
    }
    uint32_t target_height =
        uint32_t(uint64_t(window_width) * source_height / source_width);
    bool letterbox = false;
    if (target_height * present_safe_area > window_height * 100) {
      // Don't crop out more than the safe area margin - letterbox or stretch.
      target_height = window_height * 100 / present_safe_area;
      letterbox = true;
    }
    if (letterbox && cvars::present_letterbox) {
      uint32_t target_width =
          uint32_t(uint64_t(source_width) * window_height * 100 /
                   (source_height * present_safe_area));
      target_x_out = (int32_t(window_width) - int32_t(target_width)) / 2;
      target_width_out = target_width;
    } else {
      target_x_out = 0;
      target_width_out = window_width;
    }
    target_y_out = (int32_t(window_height) - int32_t(target_height)) / 2;
    target_height_out = target_height;
  } else {
    // The window is taller than the source - crop along X, then letterbox or
    // stretch along Y.
    uint32_t present_safe_area;
    if (cvars::present_safe_area_x > 0 && cvars::present_safe_area_x < 100) {
      present_safe_area = uint32_t(cvars::present_safe_area_x);
    } else {
      present_safe_area = 100;
    }
    uint32_t target_width =
        uint32_t(uint64_t(window_height) * source_width / source_height);
    bool letterbox = false;
    if (target_width * present_safe_area > window_width * 100) {
      // Don't crop out more than the safe area margin - letterbox or stretch.
      target_width = window_width * 100 / present_safe_area;
      letterbox = true;
    }
    if (letterbox && cvars::present_letterbox) {
      uint32_t target_height =
          uint32_t(uint64_t(source_height) * window_width * 100 /
                   (source_width * present_safe_area));
      target_y_out = (int32_t(window_height) - int32_t(target_height)) / 2;
      target_height_out = target_height;
    } else {
      target_y_out = 0;
      target_height_out = window_height;
    }
    target_x_out = (int32_t(window_width) - int32_t(target_width)) / 2;
    target_width_out = target_width;
  }
}

}  // namespace draw_util
}  // namespace gpu
}  // namespace xe
