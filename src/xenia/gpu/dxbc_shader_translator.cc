/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2018 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/dxbc_shader_translator.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>

#include "third_party/dxbc/DXBCChecksum.h"

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/math.h"
#include "xenia/gpu/dxbc_shader.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/graphics_provider.h"

DEFINE_bool(dxbc_switch, true,
            "Use switch rather than if for flow control. Turning this off or "
            "on may improve stability, though this heavily depends on the "
            "driver - on AMD, it's recommended to have this set to true, as "
            "Halo 3 appears to crash when if is used for flow control "
            "(possibly the shader compiler tries to flatten them). On Intel "
            "HD Graphics, this is ignored because of a crash with the switch "
            "instruction.",
            "GPU");
DEFINE_bool(dxbc_source_map, false,
            "Disassemble Xenos instructions as comments in the resulting DXBC "
            "for debugging.",
            "GPU");

namespace xe {
namespace gpu {
using namespace ucode;

// Notes about operands:
//
// Reading and writing:
// - r# (temporary registers) are 4-component and can be used anywhere.
// - v# (inputs) are 4-component and read-only.
// - o# (outputs) are 4-component and write-only.
// - oDepth (pixel shader depth output) is 1-component and write-only.
// - x# (indexable temporary registers) are 4-component and can be accessed
//   either via a mov load or a mov store (and those movs are counted as
//   ArrayInstructions in STAT, not as MovInstructions), even though the D3D11.3
//   functional specification says x# can be used wherever r# can be used, but
//   FXC emits only mov load/store in simple tests.
//
// Indexing:
// - Constant buffers use 3D indices in CBx[y][z] format, where x is the ID of
//   the binding (CB#), y is the register to access within its space, z is the
//   4-component vector to access within the register binding.
//   For example, if the requested vector is located in the beginning of the
//   second buffer in the descriptor array at b2, which is assigned to CB1, the
//   index would be CB1[3][0].
// - Resources and samplers use 2D indices, where the first dimension is the
//   S#/T#/U# binding index, and the second is the s#/t#/u# register index
//   within its space.

DxbcShaderTranslator::DxbcShaderTranslator(
    ui::GraphicsProvider::GpuVendorID vendor_id, bool bindless_resources_used,
    bool edram_rov_used, bool gamma_render_target_as_srgb,
    bool msaa_2x_supported, uint32_t draw_resolution_scale,
    bool force_emit_source_map)
    : a_(shader_code_, statistics_),
      ao_(shader_object_, statistics_),
      vendor_id_(vendor_id),
      bindless_resources_used_(bindless_resources_used),
      edram_rov_used_(edram_rov_used),
      gamma_render_target_as_srgb_(gamma_render_target_as_srgb),
      msaa_2x_supported_(msaa_2x_supported),
      draw_resolution_scale_(draw_resolution_scale),
      emit_source_map_(force_emit_source_map || cvars::dxbc_source_map) {
  assert_true(draw_resolution_scale >= 1);
  assert_true(draw_resolution_scale <= 3);
  // Don't allocate again and again for the first shader.
  shader_code_.reserve(8192);
  shader_object_.reserve(16384);
}
DxbcShaderTranslator::~DxbcShaderTranslator() = default;

std::vector<uint8_t> DxbcShaderTranslator::CreateDepthOnlyPixelShader() {
  is_depth_only_pixel_shader_ = true;
  // TODO(Triang3l): Handle in a nicer way (is_depth_only_pixel_shader_ is a
  // leftover from when a Shader object wasn't used during translation).
  Shader shader(xenos::ShaderType::kPixel, 0, nullptr, 0);
  shader.AnalyzeUcode(instruction_disassembly_buffer_);
  Shader::Translation& translation = *shader.GetOrCreateTranslation(0);
  TranslateAnalyzedShader(translation);
  is_depth_only_pixel_shader_ = false;
  return translation.translated_binary();
}

uint64_t DxbcShaderTranslator::GetDefaultVertexShaderModification(
    uint32_t dynamic_addressable_register_count,
    Shader::HostVertexShaderType host_vertex_shader_type) const {
  Modification shader_modification;
  shader_modification.vertex.dynamic_addressable_register_count =
      dynamic_addressable_register_count;
  shader_modification.vertex.host_vertex_shader_type = host_vertex_shader_type;
  return shader_modification.value;
}

uint64_t DxbcShaderTranslator::GetDefaultPixelShaderModification(
    uint32_t dynamic_addressable_register_count) const {
  Modification shader_modification;
  shader_modification.pixel.dynamic_addressable_register_count =
      dynamic_addressable_register_count;
  shader_modification.pixel.depth_stencil_mode =
      Modification::DepthStencilMode::kNoModifiers;
  return shader_modification.value;
}

void DxbcShaderTranslator::Reset() {
  ShaderTranslator::Reset();

  shader_code_.clear();

  cbuffer_count_ = 0;
  // System constants always used in prologues/epilogues.
  cbuffer_index_system_constants_ = cbuffer_count_++;
  cbuffer_index_float_constants_ = kBindingIndexUnallocated;
  cbuffer_index_bool_loop_constants_ = kBindingIndexUnallocated;
  cbuffer_index_fetch_constants_ = kBindingIndexUnallocated;
  cbuffer_index_descriptor_indices_ = kBindingIndexUnallocated;

  system_constants_used_ = 0;

  in_domain_location_used_ = 0;
  in_primitive_id_used_ = false;
  in_control_point_index_used_ = false;
  in_position_used_ = 0;
  in_front_face_used_ = false;

  system_temp_count_current_ = 0;
  system_temp_count_max_ = 0;

  cf_exec_bool_constant_ = kCfExecBoolConstantNone;
  cf_exec_predicated_ = false;
  cf_instruction_predicate_if_open_ = false;
  cf_exec_predicate_written_ = false;

  srv_count_ = 0;
  srv_index_shared_memory_ = kBindingIndexUnallocated;
  srv_index_bindless_textures_2d_ = kBindingIndexUnallocated;
  srv_index_bindless_textures_3d_ = kBindingIndexUnallocated;
  srv_index_bindless_textures_cube_ = kBindingIndexUnallocated;

  texture_bindings_.clear();
  texture_bindings_for_bindful_srv_indices_.clear();

  uav_count_ = 0;
  uav_index_shared_memory_ = kBindingIndexUnallocated;
  uav_index_edram_ = kBindingIndexUnallocated;

  sampler_bindings_.clear();

  memexport_alloc_current_count_ = 0;

  std::memset(&shader_feature_info_, 0, sizeof(shader_feature_info_));
  std::memset(&statistics_, 0, sizeof(statistics_));
}

uint32_t DxbcShaderTranslator::GetModificationRegisterCount() const {
  Modification modification = GetDxbcShaderModification();
  return is_vertex_shader()
             ? modification.vertex.dynamic_addressable_register_count
             : modification.pixel.dynamic_addressable_register_count;
}

bool DxbcShaderTranslator::UseSwitchForControlFlow() const {
  // Xenia crashes on Intel HD Graphics 4000 with switch.
  return cvars::dxbc_switch &&
         vendor_id_ != ui::GraphicsProvider::GpuVendorID::kIntel;
}

uint32_t DxbcShaderTranslator::PushSystemTemp(uint32_t zero_mask,
                                              uint32_t count) {
  uint32_t register_index = system_temp_count_current_;
  if (!is_depth_only_pixel_shader_ &&
      !current_shader().uses_register_dynamic_addressing()) {
    // Guest shader registers first if they're not in x0. Depth-only pixel
    // shader is a special case of the DXBC translator usage, where there are no
    // GPRs because there's no shader to translate, and a guest shader is not
    // loaded.
    register_index += register_count();
  }
  system_temp_count_current_ += count;
  system_temp_count_max_ =
      std::max(system_temp_count_max_, system_temp_count_current_);
  zero_mask &= 0b1111;
  if (zero_mask) {
    for (uint32_t i = 0; i < count; ++i) {
      a_.OpMov(dxbc::Dest::R(register_index + i, zero_mask), dxbc::Src::LU(0));
    }
  }
  return register_index;
}

void DxbcShaderTranslator::PopSystemTemp(uint32_t count) {
  assert_true(count <= system_temp_count_current_);
  system_temp_count_current_ -= std::min(count, system_temp_count_current_);
}

void DxbcShaderTranslator::ConvertPWLGamma(
    bool to_gamma, int32_t source_temp, uint32_t source_temp_component,
    uint32_t target_temp, uint32_t target_temp_component, uint32_t piece_temp,
    uint32_t piece_temp_component, uint32_t accumulator_temp,
    uint32_t accumulator_temp_component) {
  assert_true(source_temp != target_temp ||
              source_temp_component != target_temp_component ||
              ((target_temp != accumulator_temp ||
                target_temp_component != accumulator_temp_component) &&
               (target_temp != piece_temp ||
                target_temp_component != piece_temp_component)));
  assert_true(piece_temp != source_temp ||
              piece_temp_component != source_temp_component);
  assert_true(accumulator_temp != source_temp ||
              accumulator_temp_component != source_temp_component);
  assert_true(piece_temp != accumulator_temp ||
              piece_temp_component != accumulator_temp_component);
  dxbc::Src source_src(dxbc::Src::R(source_temp).Select(source_temp_component));
  dxbc::Dest piece_dest(dxbc::Dest::R(piece_temp, 1 << piece_temp_component));
  dxbc::Src piece_src(dxbc::Src::R(piece_temp).Select(piece_temp_component));
  dxbc::Dest accumulator_dest(
      dxbc::Dest::R(accumulator_temp, 1 << accumulator_temp_component));
  dxbc::Src accumulator_src(
      dxbc::Src::R(accumulator_temp).Select(accumulator_temp_component));
  // For each piece:
  // 1) Calculate how far we are on it. Multiply by 1/width, subtract
  //    start/width and saturate.
  // 2) Add the contribution of the piece - multiply the position on the piece
  //    by its slope*width and accumulate.
  // Piece 1.
  a_.OpMul(piece_dest, source_src,
           dxbc::Src::LF(to_gamma ? (1.0f / 0.0625f) : (1.0f / 0.25f)), true);
  a_.OpMul(accumulator_dest, piece_src,
           dxbc::Src::LF(to_gamma ? (4.0f * 0.0625f) : (0.25f * 0.25f)));
  // Piece 2.
  a_.OpMAd(piece_dest, source_src,
           dxbc::Src::LF(to_gamma ? (1.0f / 0.0625f) : (1.0f / 0.125f)),
           dxbc::Src::LF(to_gamma ? (-0.0625f / 0.0625f) : (-0.25f / 0.125f)),
           true);
  a_.OpMAd(accumulator_dest, piece_src,
           dxbc::Src::LF(to_gamma ? (2.0f * 0.0625f) : (0.5f * 0.125f)),
           accumulator_src);
  // Piece 3.
  a_.OpMAd(piece_dest, source_src,
           dxbc::Src::LF(to_gamma ? (1.0f / 0.375f) : (1.0f / 0.375f)),
           dxbc::Src::LF(to_gamma ? (-0.125f / 0.375f) : (-0.375f / 0.375f)),
           true);
  a_.OpMAd(accumulator_dest, piece_src,
           dxbc::Src::LF(to_gamma ? (1.0f * 0.375f) : (1.0f * 0.375f)),
           accumulator_src);
  // Piece 4.
  a_.OpMAd(piece_dest, source_src,
           dxbc::Src::LF(to_gamma ? (1.0f / 0.5f) : (1.0f / 0.25f)),
           dxbc::Src::LF(to_gamma ? (-0.5f / 0.5f) : (-0.75f / 0.25f)), true);
  a_.OpMAd(dxbc::Dest::R(target_temp, 1 << target_temp_component), piece_src,
           dxbc::Src::LF(to_gamma ? (0.5f * 0.5f) : (2.0f * 0.25f)),
           accumulator_src);
}

void DxbcShaderTranslator::StartVertexShader_LoadVertexIndex() {
  if (register_count() < 1) {
    return;
  }

  bool uses_register_dynamic_addressing =
      current_shader().uses_register_dynamic_addressing();

  // Writing the index to X of GPR 0 - either directly if not using indexable
  // registers, or via a system temporary register.
  uint32_t reg;
  if (uses_register_dynamic_addressing) {
    reg = PushSystemTemp();
  } else {
    reg = 0;
  }

  dxbc::Dest index_dest(dxbc::Dest::R(reg, 0b0001));
  dxbc::Src index_src(dxbc::Src::R(reg, dxbc::Src::kXXXX));

  // Check if the closing vertex of a non-indexed line loop is being processed.
  a_.OpINE(
      index_dest,
      dxbc::Src::V(uint32_t(InOutRegister::kVSInVertexIndex), dxbc::Src::kXXXX),
      LoadSystemConstant(SystemConstants::Index::kLineLoopClosingIndex,
                         offsetof(SystemConstants, line_loop_closing_index),
                         dxbc::Src::kXXXX));
  // Zero the index if processing the closing vertex of a line loop, or do
  // nothing (replace 0 with 0) if not needed.
  a_.OpAnd(
      index_dest,
      dxbc::Src::V(uint32_t(InOutRegister::kVSInVertexIndex), dxbc::Src::kXXXX),
      index_src);

  {
    // Swap the vertex index's endianness.
    dxbc::Src endian_src(LoadSystemConstant(
        SystemConstants::Index::kVertexIndexEndian,
        offsetof(SystemConstants, vertex_index_endian), dxbc::Src::kXXXX));
    dxbc::Dest swap_temp_dest(dxbc::Dest::R(reg, 0b0010));
    dxbc::Src swap_temp_src(dxbc::Src::R(reg, dxbc::Src::kYYYY));

    // 8-in-16 or one half of 8-in-32.
    a_.OpSwitch(endian_src);
    a_.OpCase(dxbc::Src::LU(uint32_t(xenos::Endian::k8in16)));
    a_.OpCase(dxbc::Src::LU(uint32_t(xenos::Endian::k8in32)));
    // Temp = X0Z0.
    a_.OpAnd(swap_temp_dest, index_src, dxbc::Src::LU(0x00FF00FF));
    // Index = YZW0.
    a_.OpUShR(index_dest, index_src, dxbc::Src::LU(8));
    // Index = Y0W0.
    a_.OpAnd(index_dest, index_src, dxbc::Src::LU(0x00FF00FF));
    // Index = YXWZ.
    a_.OpUMAd(index_dest, swap_temp_src, dxbc::Src::LU(256), index_src);
    a_.OpBreak();
    a_.OpEndSwitch();

    // 16-in-32 or another half of 8-in-32.
    a_.OpSwitch(endian_src);
    a_.OpCase(dxbc::Src::LU(uint32_t(xenos::Endian::k8in32)));
    a_.OpCase(dxbc::Src::LU(uint32_t(xenos::Endian::k16in32)));
    // Temp = ZW00.
    a_.OpUShR(swap_temp_dest, index_src, dxbc::Src::LU(16));
    // Index = ZWXY.
    a_.OpBFI(index_dest, dxbc::Src::LU(16), dxbc::Src::LU(16), index_src,
             swap_temp_src);
    a_.OpBreak();
    a_.OpEndSwitch();

    if (!uses_register_dynamic_addressing) {
      // Break register dependency.
      a_.OpMov(swap_temp_dest, dxbc::Src::LF(0.0f));
    }
  }

  // Add the base vertex index.
  a_.OpIAdd(index_dest, index_src,
            LoadSystemConstant(SystemConstants::Index::kVertexIndexOffset,
                               offsetof(SystemConstants, vertex_index_offset),
                               dxbc::Src::kXXXX));

  // Mask since the GPU only uses the lower 24 bits of the vertex index (tested
  // on an Adreno 200 phone). `((index & 0xFFFFFF) + offset) & 0xFFFFFF` is the
  // same as `(index + offset) & 0xFFFFFF`.
  a_.OpAnd(index_dest, index_src, dxbc::Src::LU(xenos::kVertexIndexMask));

  // Clamp the vertex index after offsetting.
  a_.OpUMax(index_dest, index_src,
            LoadSystemConstant(SystemConstants::Index::kVertexIndexMinMax,
                               offsetof(SystemConstants, vertex_index_min),
                               dxbc::Src::kXXXX));
  a_.OpUMin(index_dest, index_src,
            LoadSystemConstant(SystemConstants::Index::kVertexIndexMinMax,
                               offsetof(SystemConstants, vertex_index_max),
                               dxbc::Src::kXXXX));

  // Convert to float.
  a_.OpUToF(index_dest, index_src);

  if (uses_register_dynamic_addressing) {
    // Store to indexed GPR 0 in x0[0].
    a_.OpMov(dxbc::Dest::X(0, 0, 0b0001), index_src);
    PopSystemTemp();
  }
}

void DxbcShaderTranslator::StartVertexOrDomainShader() {
  bool uses_register_dynamic_addressing =
      current_shader().uses_register_dynamic_addressing();

  // Zero the interpolators.
  for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
    a_.OpMov(dxbc::Dest::O(uint32_t(InOutRegister::kVSDSOutInterpolators) + i),
             dxbc::Src::LF(0.0f));
  }

  // Remember that x# are only accessible via mov load or store - use a
  // temporary variable if need to do any computations!
  Shader::HostVertexShaderType host_vertex_shader_type =
      GetDxbcShaderModification().vertex.host_vertex_shader_type;
  switch (host_vertex_shader_type) {
    case Shader::HostVertexShaderType::kVertex:
      StartVertexShader_LoadVertexIndex();
      break;

    case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
      assert_true(register_count() >= 2);
      if (register_count() >= 1) {
        // Copy the domain location to r0.xyz.
        // ZYX swizzle according to Call of Duty 3 and Viva Pinata.
        in_domain_location_used_ |= 0b0111;
        a_.OpMov(uses_register_dynamic_addressing ? dxbc::Dest::X(0, 0, 0b0111)
                                                  : dxbc::Dest::R(0, 0b0111),
                 dxbc::Src::VDomain(0b000110));
        if (register_count() >= 2) {
          // Copy the control point indices (already swapped and converted to
          // float by the host vertex and hull shaders) to r1.xyz.
          dxbc::Dest control_point_index_dest(uses_register_dynamic_addressing
                                                  ? dxbc::Dest::X(0, 1)
                                                  : dxbc::Dest::R(1));
          in_control_point_index_used_ = true;
          for (uint32_t i = 0; i < 3; ++i) {
            a_.OpMov(control_point_index_dest.Mask(1 << i),
                     dxbc::Src::VICP(
                         i, uint32_t(InOutRegister::kDSInControlPointIndex),
                         dxbc::Src::kXXXX));
          }
        }
      }
      break;

    case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
      assert_true(register_count() >= 2);
      if (register_count() >= 1) {
        // Copy the domain location to r0.xyz.
        // ZYX swizzle with r1.y == 0, according to the water shader in
        // Banjo-Kazooie: Nuts & Bolts.
        in_domain_location_used_ |= 0b0111;
        a_.OpMov(uses_register_dynamic_addressing ? dxbc::Dest::X(0, 0, 0b0111)
                                                  : dxbc::Dest::R(0, 0b0111),
                 dxbc::Src::VDomain(0b000110));
        if (register_count() >= 2) {
          // Copy the primitive index to r1.x as a float.
          uint32_t primitive_id_temp =
              uses_register_dynamic_addressing ? PushSystemTemp() : 1;
          in_primitive_id_used_ = true;
          a_.OpUToF(dxbc::Dest::R(primitive_id_temp, 0b0001),
                    dxbc::Src::VPrim());
          if (uses_register_dynamic_addressing) {
            a_.OpMov(dxbc::Dest::X(0, 1, 0b0001),
                     dxbc::Src::R(primitive_id_temp, dxbc::Src::kXXXX));
            // Release primitive_id_temp.
            PopSystemTemp();
          }
          // Write the swizzle of the barycentric coordinates to r1.y. It
          // appears that the tessellator offloads the reordering of coordinates
          // for edges to game shaders.
          //
          // In Banjo-Kazooie: Nuts & Bolts, the water shader multiplies the
          // first control point's position by r0.z, the second CP's by r0.y,
          // and the third CP's by r0.x. But before doing that it swizzles
          // r0.xyz the following way depending on the value in r1.y:
          // - ZXY for 1.0.
          // - YZX for 2.0.
          // - XZY for 4.0.
          // - YXZ for 5.0.
          // - ZYX for 6.0.
          // Possibly, the logic here is that the value itself is the amount of
          // rotation of the swizzle to the right, and 1 << 2 is set when the
          // swizzle needs to be flipped before rotating.
          //
          // Direct3D 12 passes the coordinates in a consistent order, so can
          // just use the identity swizzle.
          a_.OpMov(uses_register_dynamic_addressing
                       ? dxbc::Dest::X(0, 1, 0b0010)
                       : dxbc::Dest::R(1, 0b0010),
                   dxbc::Src::LF(0.0f));
        }
      }
      break;

    case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
      assert_true(register_count() >= 2);
      if (register_count() >= 1) {
        // Copy the domain location to r0.xy.
        in_domain_location_used_ |= 0b0011;
        a_.OpMov(uses_register_dynamic_addressing ? dxbc::Dest::X(0, 0, 0b0011)
                                                  : dxbc::Dest::R(0, 0b0011),
                 dxbc::Src::VDomain());
        // Control point indices according to the shader from the main menu of
        // Defender, which starts from `cndeq r2, c255.xxxy, r1.xyzz, r0.zzzz`,
        // where c255.x is 0, and c255.y is 1.
        // r0.z for (1 - r0.x) * (1 - r0.y)
        // r1.x for r0.x * (1 - r0.y)
        // r1.y for r0.x * r0.y
        // r1.z for (1 - r0.x) * r0.y
        in_control_point_index_used_ = true;
        a_.OpMov(
            uses_register_dynamic_addressing ? dxbc::Dest::X(0, 0, 0b0100)
                                             : dxbc::Dest::R(0, 0b0100),
            dxbc::Src::VICP(0, uint32_t(InOutRegister::kDSInControlPointIndex),
                            dxbc::Src::kXXXX));
        if (register_count() >= 2) {
          dxbc::Dest r1_dest(uses_register_dynamic_addressing
                                 ? dxbc::Dest::X(0, 1)
                                 : dxbc::Dest::R(1));
          for (uint32_t i = 0; i < 3; ++i) {
            a_.OpMov(r1_dest.Mask(1 << i),
                     dxbc::Src::VICP(
                         1 + i, uint32_t(InOutRegister::kDSInControlPointIndex),
                         dxbc::Src::kXXXX));
          }
        }
      }
      break;

    case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
      assert_true(register_count() >= 2);
      if (register_count() >= 1) {
        // Copy the domain location to r0.yz.
        // XY swizzle according to the ground shader in Viva Pinata.
        in_domain_location_used_ |= 0b0011;
        a_.OpMov(uses_register_dynamic_addressing ? dxbc::Dest::X(0, 0, 0b0110)
                                                  : dxbc::Dest::R(0, 0b0110),
                 dxbc::Src::VDomain(0b010000));
        // Copy the primitive index to r0.x as a float.
        uint32_t primitive_id_temp =
            uses_register_dynamic_addressing ? PushSystemTemp() : 0;
        in_primitive_id_used_ = true;
        a_.OpUToF(dxbc::Dest::R(primitive_id_temp, 0b0001), dxbc::Src::VPrim());
        if (uses_register_dynamic_addressing) {
          a_.OpMov(dxbc::Dest::X(0, 0, 0b0001),
                   dxbc::Src::R(primitive_id_temp, dxbc::Src::kXXXX));
          // Release primitive_id_temp.
          PopSystemTemp();
        }
        if (register_count() >= 2) {
          // Write the swizzle of the UV coordinates to r1.x. It appears that
          // the tessellator offloads the reordering of coordinates for edges to
          // game shaders.
          //
          // In Viva Pinata, if we assume that r0.y is U and r0.z is V, the
          // factors each control point value is multiplied by are the
          // following:
          // - (1-u)*(1-v), u*(1-v), (1-u)*v, u*v for 0.0 (identity swizzle).
          // - u*(1-v), (1-u)*(1-v), u*v, (1-u)*v for 1.0 (YXWZ).
          // - u*v, (1-u)*v, u*(1-v), (1-u)*(1-v) for 2.0 (WZYX).
          // - (1-u)*v, u*v, (1-u)*(1-v), u*(1-v) for 3.0 (ZWXY).
          //
          // Direct3D 12 passes the coordinates in a consistent order, so can
          // just use the identity swizzle.
          a_.OpMov(uses_register_dynamic_addressing
                       ? dxbc::Dest::X(0, 1, 0b0001)
                       : dxbc::Dest::R(1, 0b0001),
                   dxbc::Src::LF(0.0f));
        }
      }
      break;

    default:
      // TODO(Triang3l): Support line and non-adaptive quad patches.
      assert_unhandled_case(host_vertex_shader_type);
      EmitTranslationError(
          "Unsupported host vertex shader type in StartVertexOrDomainShader");
      break;
  }
}

void DxbcShaderTranslator::StartPixelShader() {
  if (edram_rov_used_) {
    // Load the EDRAM addresses and the coverage.
    StartPixelShader_LoadROVParameters();

    if (ROV_IsDepthStencilEarly()) {
      // Do early 2x2 quad rejection if it's safe.
      ROV_DepthStencilTest();
    } else {
      if (!current_shader().writes_depth()) {
        // Get the derivatives of the screen-space (but not clamped to the
        // viewport depth bounds yet - this happens after the pixel shader in
        // Direct3D 11+; also linear within the triangle - thus constant
        // derivatives along the triangle) Z for calculating per-sample depth
        // values and the slope-scaled polygon offset to
        // system_temp_depth_stencil_ before any return statement is possibly
        // reached.
        assert_true(system_temp_depth_stencil_ != UINT32_MAX);
        dxbc::Src in_position_z(dxbc::Src::V(
            uint32_t(InOutRegister::kPSInPosition), dxbc::Src::kZZZZ));
        in_position_used_ |= 0b0100;
        a_.OpDerivRTXCoarse(dxbc::Dest::R(system_temp_depth_stencil_, 0b0001),
                            in_position_z);
        a_.OpDerivRTYCoarse(dxbc::Dest::R(system_temp_depth_stencil_, 0b0010),
                            in_position_z);
      }
    }
  }

  // If not translating anything, we only need the depth.
  if (is_depth_only_pixel_shader_) {
    return;
  }

  bool uses_register_dynamic_addressing =
      current_shader().uses_register_dynamic_addressing();

  uint32_t interpolator_count =
      std::min(xenos::kMaxInterpolators, register_count());
  if (interpolator_count != 0) {
    // Copy interpolants to GPRs.
    uint32_t centroid_temp =
        uses_register_dynamic_addressing ? PushSystemTemp() : UINT32_MAX;
    dxbc::Src sampling_pattern_src(LoadSystemConstant(
        SystemConstants::Index::kInterpolatorSamplingPattern,
        offsetof(SystemConstants, interpolator_sampling_pattern),
        dxbc::Src::kXXXX));
    for (uint32_t i = 0; i < interpolator_count; ++i) {
      // With GPR dynamic addressing, first evaluate to centroid_temp r#, then
      // store to the x#.
      uint32_t centroid_register =
          uses_register_dynamic_addressing ? centroid_temp : i;
      // Check if the input needs to be interpolated at center (if the bit is
      // set).
      a_.OpAnd(dxbc::Dest::R(centroid_register, 0b0001), sampling_pattern_src,
               dxbc::Src::LU(uint32_t(1) << i));
      a_.OpIf(bool(xenos::SampleLocation::kCenter),
              dxbc::Src::R(centroid_register, dxbc::Src::kXXXX));
      // At center.
      a_.OpMov(uses_register_dynamic_addressing ? dxbc::Dest::X(0, i)
                                                : dxbc::Dest::R(i),
               dxbc::Src::V(uint32_t(InOutRegister::kPSInInterpolators) + i));
      a_.OpElse();
      // At centroid. Not really important that 2x MSAA is emulated using
      // ForcedSampleCount 4 - what matters is that the sample position will
      // be within the primitive, and the value will not be extrapolated.
      a_.OpEvalCentroid(
          dxbc::Dest::R(centroid_register),
          dxbc::Src::V(uint32_t(InOutRegister::kPSInInterpolators) + i));
      if (uses_register_dynamic_addressing) {
        a_.OpMov(dxbc::Dest::X(0, i), dxbc::Src::R(centroid_register));
      }
      a_.OpEndIf();
    }
    if (centroid_temp != UINT32_MAX) {
      PopSystemTemp();
    }

    // Write pixel parameters - screen (XY absolute value) and point sprite (ZW
    // absolute value) coordinates, facing (X sign bit) - to the specified
    // interpolator register (ps_param_gen).
    dxbc::Src param_gen_index_src(LoadSystemConstant(
        SystemConstants::Index::kPSParamGen,
        offsetof(SystemConstants, ps_param_gen), dxbc::Src::kXXXX));
    uint32_t param_gen_temp = PushSystemTemp();
    // Check if pixel parameters need to be written.
    a_.OpULT(dxbc::Dest::R(param_gen_temp, 0b0001), param_gen_index_src,
             dxbc::Src::LU(interpolator_count));
    a_.OpIf(true, dxbc::Src::R(param_gen_temp, dxbc::Src::kXXXX));
    {
      // XY - floored pixel position (Direct3D VPOS) in the absolute value,
      // faceness as X sign bit. Using Z as scratch register now.
      // Get XY address of the current host pixel as float (no matter whether
      // the position is pixel-rate or sample-rate also due to float24 depth
      // conversion requirements, it will be rounded the same). Rounding down,
      // and taking the absolute value (because the sign bit of X stores the
      // faceness), so in case the host GPU for some reason has quads used for
      // derivative calculation at odd locations, the left and top edges will
      // have correct derivative magnitude and LODs.
      in_position_used_ |= 0b0011;
      a_.OpRoundNI(dxbc::Dest::R(param_gen_temp, 0b0011),
                   dxbc::Src::V(uint32_t(InOutRegister::kPSInPosition)));
      if (draw_resolution_scale_ > 1) {
        // Revert resolution scale - after truncating, so if the pixel position
        // is passed to tfetch (assuming the game doesn't round it by itself),
        // it will be sampled with higher resolution too.
        a_.OpMul(dxbc::Dest::R(param_gen_temp, 0b0011),
                 dxbc::Src::R(param_gen_temp),
                 dxbc::Src::LF(1.0f / draw_resolution_scale_));
      }
      a_.OpMov(dxbc::Dest::R(param_gen_temp, 0b0011),
               dxbc::Src::R(param_gen_temp).Abs());
      // Check if faceness applies to the current primitive type.
      a_.OpAnd(dxbc::Dest::R(param_gen_temp, 0b0100), LoadFlagsSystemConstant(),
               dxbc::Src::LU(kSysFlag_PrimitivePolygonal));
      a_.OpIf(true, dxbc::Src::R(param_gen_temp, dxbc::Src::kZZZZ));
      {
        // Negate modifier flips the sign bit even for 0 - set it to minus for
        // backfaces.
        in_front_face_used_ = true;
        a_.OpMovC(
            dxbc::Dest::R(param_gen_temp, 0b0001),
            dxbc::Src::V(uint32_t(InOutRegister::kPSInFrontFaceAndSampleIndex),
                         dxbc::Src::kXXXX),
            dxbc::Src::R(param_gen_temp, dxbc::Src::kXXXX),
            -dxbc::Src::R(param_gen_temp, dxbc::Src::kXXXX));
      }
      a_.OpEndIf();
      // ZW - UV within a point sprite in the absolute value, at centroid if
      // requested for the interpolator.
      dxbc::Dest point_coord_r_zw_dest(dxbc::Dest::R(param_gen_temp, 0b1100));
      dxbc::Src point_coord_v_xxxy_src(dxbc::Src::V(
          uint32_t(InOutRegister::kPSInPointParameters), 0b01000000));
      a_.OpUBFE(dxbc::Dest::R(param_gen_temp, 0b0100), dxbc::Src::LU(1),
                param_gen_index_src,
                LoadSystemConstant(
                    SystemConstants::Index::kInterpolatorSamplingPattern,
                    offsetof(SystemConstants, interpolator_sampling_pattern),
                    dxbc::Src::kXXXX));
      a_.OpIf(bool(xenos::SampleLocation::kCenter),
              dxbc::Src::R(param_gen_temp, dxbc::Src::kZZZZ));
      // At center.
      a_.OpMov(point_coord_r_zw_dest, point_coord_v_xxxy_src);
      a_.OpElse();
      // At centroid.
      a_.OpEvalCentroid(point_coord_r_zw_dest, point_coord_v_xxxy_src);
      a_.OpEndIf();
      // Write ps_param_gen to the specified GPR.
      dxbc::Src param_gen_src(dxbc::Src::R(param_gen_temp));
      if (uses_register_dynamic_addressing) {
        // Copy the GPR number to r# for relative addressing.
        uint32_t param_gen_copy_temp = PushSystemTemp();
        a_.OpMov(dxbc::Dest::R(param_gen_copy_temp, 0b0001),
                 param_gen_index_src);
        // Write to the GPR.
        a_.OpMov(dxbc::Dest::X(0, dxbc::Index(param_gen_copy_temp, 0)),
                 param_gen_src);
        // Release param_gen_copy_temp.
        PopSystemTemp();
      } else {
        if (interpolator_count == 1) {
          a_.OpMov(dxbc::Dest::R(0), param_gen_src);
        } else {
          // Write to the r# using binary search.
          uint32_t param_gen_copy_temp = PushSystemTemp();
          auto param_gen_copy_node = [&](uint32_t low, uint32_t high,
                                         const auto& self) -> void {
            assert_true(low < high);
            uint32_t mid = low + (high - low + 1) / 2;
            a_.OpULT(dxbc::Dest::R(param_gen_copy_temp, 0b0001),
                     param_gen_index_src, dxbc::Src::LU(mid));
            a_.OpIf(true, dxbc::Src::R(param_gen_copy_temp, dxbc::Src::kXXXX));
            {
              if (low + 1 == mid) {
                a_.OpMov(dxbc::Dest::R(low), param_gen_src);
              } else {
                self(low, mid - 1, self);
              }
            }
            a_.OpElse();
            {
              if (mid == high) {
                a_.OpMov(dxbc::Dest::R(mid), param_gen_src);
              } else {
                self(mid, high, self);
              }
            }
            a_.OpEndIf();
          };
          param_gen_copy_node(0, interpolator_count - 1, param_gen_copy_node);
          // Release param_gen_copy_temp.
          PopSystemTemp();
        }
      }
    }
    // Close the ps_param_gen check.
    a_.OpEndIf();
    // Release param_gen_temp.
    PopSystemTemp();
  }
}

void DxbcShaderTranslator::StartTranslation() {
  // Allocate global system temporary registers that may also be used in the
  // epilogue.
  if (is_vertex_shader()) {
    system_temp_position_ = PushSystemTemp(0b1111);
    system_temp_point_size_edge_flag_kill_vertex_ = PushSystemTemp(0b0100);
    // Set the point size to a negative value to tell the geometry shader that
    // it should use the global point size if the vertex shader does not
    // override it.
    a_.OpMov(
        dxbc::Dest::R(system_temp_point_size_edge_flag_kill_vertex_, 0b0001),
        dxbc::Src::LF(-1.0f));
  } else if (is_pixel_shader()) {
    if (edram_rov_used_) {
      // Will be initialized unconditionally.
      system_temp_rov_params_ = PushSystemTemp();
    }
    if (IsDepthStencilSystemTempUsed()) {
      uint32_t depth_stencil_temp_zero_mask;
      if (current_shader().writes_depth()) {
        // X holds the guest oDepth - make sure it's always initialized because
        // assumptions can't be made about the integrity of the guest code.
        depth_stencil_temp_zero_mask = 0b0001;
      } else {
        assert_true(edram_rov_used_);
        if (ROV_IsDepthStencilEarly()) {
          // XYZW hold per-sample depth / stencil after the early test - written
          // conditionally based on the coverage, ensure registers are
          // initialized unconditionally for safety.
          depth_stencil_temp_zero_mask = 0b1111;
        } else {
          // XY hold Z gradients, written unconditionally in the beginning.
          depth_stencil_temp_zero_mask = 0b0000;
        }
      }
      system_temp_depth_stencil_ = PushSystemTemp(depth_stencil_temp_zero_mask);
    }
    uint32_t shader_writes_color_targets =
        current_shader().writes_color_targets();
    for (uint32_t i = 0; i < 4; ++i) {
      if (shader_writes_color_targets & (1 << i)) {
        system_temps_color_[i] = PushSystemTemp(0b1111);
      }
    }
  }

  if (!is_depth_only_pixel_shader_) {
    // Allocate temporary registers for memexport addresses and data.
    std::memset(system_temps_memexport_address_, 0xFF,
                sizeof(system_temps_memexport_address_));
    std::memset(system_temps_memexport_data_, 0xFF,
                sizeof(system_temps_memexport_data_));
    system_temp_memexport_written_ = UINT32_MAX;
    const uint8_t* memexports_written = current_shader().memexport_eM_written();
    for (uint32_t i = 0; i < Shader::kMaxMemExports; ++i) {
      uint32_t memexport_alloc_written = memexports_written[i];
      if (memexport_alloc_written == 0) {
        continue;
      }
      // If memexport is used at all, allocate a register containing whether eM#
      // have actually been written to.
      if (system_temp_memexport_written_ == UINT32_MAX) {
        system_temp_memexport_written_ = PushSystemTemp(0b1111);
      }
      system_temps_memexport_address_[i] = PushSystemTemp(0b1111);
      uint32_t memexport_data_index;
      while (xe::bit_scan_forward(memexport_alloc_written,
                                  &memexport_data_index)) {
        memexport_alloc_written &= ~(1u << memexport_data_index);
        system_temps_memexport_data_[i][memexport_data_index] =
            PushSystemTemp();
      }
    }

    // Allocate system temporary variables for the translated code. Since access
    // depends on the guest code (thus no guarantees), initialize everything
    // now (except for pv, it's an internal temporary variable, not accessible
    // by the guest).
    system_temp_result_ = PushSystemTemp();
    system_temp_ps_pc_p0_a0_ = PushSystemTemp(0b1111);
    system_temp_aL_ = PushSystemTemp(0b1111);
    system_temp_loop_count_ = PushSystemTemp(0b1111);
    system_temp_grad_h_lod_ = PushSystemTemp(0b1111);
    system_temp_grad_v_ = PushSystemTemp(0b0111);

    // Zero general-purpose registers to prevent crashes when the game
    // references them after only initializing them conditionally.
    for (uint32_t i = is_pixel_shader() ? xenos::kMaxInterpolators : 0;
         i < register_count(); ++i) {
      a_.OpMov(current_shader().uses_register_dynamic_addressing()
                   ? dxbc::Dest::X(0, i)
                   : dxbc::Dest::R(i),
               dxbc::Src::LF(0.0f));
    }
  }

  // Write stage-specific prologue.
  if (is_vertex_shader()) {
    StartVertexOrDomainShader();
  } else if (is_pixel_shader()) {
    StartPixelShader();
  }

  // If not translating anything, don't start the main loop.
  if (is_depth_only_pixel_shader_) {
    return;
  }

  // Start the main loop (for jumping to labels by setting pc and continuing).
  a_.OpLoop();
  // Switch and the first label (pc == 0).
  if (UseSwitchForControlFlow()) {
    a_.OpSwitch(dxbc::Src::R(system_temp_ps_pc_p0_a0_, dxbc::Src::kYYYY));
    a_.OpCase(dxbc::Src::LU(0));
  } else {
    a_.OpIf(false, dxbc::Src::R(system_temp_ps_pc_p0_a0_, dxbc::Src::kYYYY));
  }
}

void DxbcShaderTranslator::CompleteVertexOrDomainShader() {
  uint32_t temp = PushSystemTemp();
  dxbc::Dest temp_x_dest(dxbc::Dest::R(temp, 0b0001));
  dxbc::Src temp_x_src(dxbc::Src::R(temp, dxbc::Src::kXXXX));

  dxbc::Src flags_src(LoadFlagsSystemConstant());

  // Check if the shader already returns W, not 1/W, and if it doesn't, turn 1/W
  // into W. Using div rather than relaxed-precision rcp for safety.
  a_.OpAnd(temp_x_dest, flags_src, dxbc::Src::LU(kSysFlag_WNotReciprocal));
  a_.OpIf(false, temp_x_src);
  a_.OpDiv(dxbc::Dest::R(system_temp_position_, 0b1000), dxbc::Src::LF(1.0f),
           dxbc::Src::R(system_temp_position_, dxbc::Src::kWWWW));
  a_.OpEndIf();

  // Check if the shader returns XY/W rather than XY, and if it does, revert
  // that.
  // TODO(Triang3l): Check if having XY or Z pre-divided by W should result in
  // affine interpolation.
  a_.OpAnd(temp_x_dest, flags_src, dxbc::Src::LU(kSysFlag_XYDividedByW));
  a_.OpIf(true, temp_x_src);
  a_.OpMul(dxbc::Dest::R(system_temp_position_, 0b0011),
           dxbc::Src::R(system_temp_position_),
           dxbc::Src::R(system_temp_position_, dxbc::Src::kWWWW));
  a_.OpEndIf();

  // Check if the shader returns Z/W rather than Z, and if it does, revert that.
  // TODO(Triang3l): Check if having XY or Z pre-divided by W should result in
  // affine interpolation.
  a_.OpAnd(temp_x_dest, flags_src, dxbc::Src::LU(kSysFlag_ZDividedByW));
  a_.OpIf(true, temp_x_src);
  a_.OpMul(dxbc::Dest::R(system_temp_position_, 0b0100),
           dxbc::Src::R(system_temp_position_, dxbc::Src::kZZZZ),
           dxbc::Src::R(system_temp_position_, dxbc::Src::kWWWW));
  a_.OpEndIf();

  // Zero-initialize SV_ClipDistance# (for user clip planes) and SV_CullDistance
  // (for vertex kill) in case they're not needed.
  a_.OpMov(dxbc::Dest::O(uint32_t(InOutRegister::kVSDSOutClipDistance0123)),
           dxbc::Src::LF(0.0f));
  a_.OpMov(dxbc::Dest::O(
               uint32_t(InOutRegister::kVSDSOutClipDistance45AndCullDistance),
               0b0111),
           dxbc::Src::LF(0.0f));
  // Clip against user clip planes.
  // Not possible to handle UCP_CULL_ONLY_ENA with the same shader though, since
  // there can be only 8 SV_ClipDistance + SV_CullDistance values at most, but
  // 12 would be needed.
  for (uint32_t i = 0; i < 6; ++i) {
    // Check if the clip plane is enabled - this `if` is needed, as opposed to
    // just zeroing the clip planes in the constants, so Infinity and NaN in the
    // position won't have any effect caused by this if clip planes are
    // disabled.
    a_.OpAnd(temp_x_dest, flags_src,
             dxbc::Src::LU(kSysFlag_UserClipPlane0 << i));
    a_.OpIf(true, temp_x_src);
    a_.OpDP4(dxbc::Dest::O(
                 uint32_t(InOutRegister::kVSDSOutClipDistance0123) + (i >> 2),
                 1 << (i & 3)),
             dxbc::Src::R(system_temp_position_),
             LoadSystemConstant(SystemConstants::Index::kUserClipPlanes,
                                offsetof(SystemConstants, user_clip_planes) +
                                    sizeof(float) * 4 * i,
                                dxbc::Src::kXYZW));
    a_.OpEndIf();
  }

  // Apply scale for guest to host viewport and clip space conversion. Also, if
  // the vertex shader is multipass, the NDC scale constant can be used to set
  // position to NaN to kill all primitives.
  a_.OpMul(dxbc::Dest::R(system_temp_position_, 0b0111),
           dxbc::Src::R(system_temp_position_),
           LoadSystemConstant(SystemConstants::Index::kNDCScale,
                              offsetof(SystemConstants, ndc_scale), 0b100100));

  // Apply offset (multiplied by W) used for the same purposes.
  a_.OpMAd(dxbc::Dest::R(system_temp_position_, 0b0111),
           LoadSystemConstant(SystemConstants::Index::kNDCOffset,
                              offsetof(SystemConstants, ndc_offset), 0b100100),
           dxbc::Src::R(system_temp_position_, dxbc::Src::kWWWW),
           dxbc::Src::R(system_temp_position_));

  // Assuming SV_CullDistance was zeroed earlier in this function.
  // Kill the primitive if needed - check if the shader wants to kill.
  // TODO(Triang3l): Find if the condition is actually the flag being non-zero.
  a_.OpNE(temp_x_dest,
          dxbc::Src::R(system_temp_point_size_edge_flag_kill_vertex_,
                       dxbc::Src::kZZZZ),
          dxbc::Src::LF(0.0f));
  a_.OpIf(true, temp_x_src);
  {
    // Extract the killing condition.
    a_.OpAnd(temp_x_dest, flags_src,
             dxbc::Src::LU(kSysFlag_KillIfAnyVertexKilled));
    a_.OpIf(true, temp_x_src);
    {
      // Kill the primitive if any vertex is killed - write NaN to position.
      a_.OpMov(dxbc::Dest::R(system_temp_position_, 0b1000),
               dxbc::Src::LF(std::nanf("")));
    }
    a_.OpElse();
    {
      // Kill the primitive if all vertices are killed - set SV_CullDistance to
      // negative.
      a_.OpMov(
          dxbc::Dest::O(
              uint32_t(InOutRegister::kVSDSOutClipDistance45AndCullDistance),
              0b0100),
          dxbc::Src::LF(-1.0f));
    }
    a_.OpEndIf();
  }
  a_.OpEndIf();

  // Write the position to the output.
  a_.OpMov(dxbc::Dest::O(uint32_t(InOutRegister::kVSDSOutPosition)),
           dxbc::Src::R(system_temp_position_));

  // Zero the point coordinate (will be set in the geometry shader if needed)
  // and write the point size.
  a_.OpMov(
      dxbc::Dest::O(uint32_t(InOutRegister::kVSDSOutPointParameters), 0b0011),
      dxbc::Src::LF(0.0f));
  a_.OpMov(
      dxbc::Dest::O(uint32_t(InOutRegister::kVSDSOutPointParameters), 0b0100),
      dxbc::Src::R(system_temp_point_size_edge_flag_kill_vertex_,
                   dxbc::Src::kXXXX));

  // Release temp.
  PopSystemTemp();
}

void DxbcShaderTranslator::CompleteShaderCode() {
  if (!is_depth_only_pixel_shader_) {
    // Close the last exec, there's nothing to merge it with anymore, and we're
    // closing upper-level flow control blocks.
    CloseExecConditionals();
    // Close the last label and the switch.
    if (UseSwitchForControlFlow()) {
      a_.OpBreak();
      a_.OpEndSwitch();
    } else {
      a_.OpEndIf();
    }
    // End the main loop.
    a_.OpBreak();
    a_.OpEndLoop();

    // Release the following system temporary values so epilogue can reuse them:
    // - system_temp_result_.
    // - system_temp_ps_pc_p0_a0_.
    // - system_temp_aL_.
    // - system_temp_loop_count_.
    // - system_temp_grad_h_lod_.
    // - system_temp_grad_v_.
    PopSystemTemp(6);

    // Write memexported data to the shared memory UAV.
    ExportToMemory();

    // Release memexport temporary registers.
    for (int i = Shader::kMaxMemExports - 1; i >= 0; --i) {
      if (system_temps_memexport_address_[i] == UINT32_MAX) {
        continue;
      }
      // Release exported data registers.
      for (int j = 4; j >= 0; --j) {
        if (system_temps_memexport_data_[i][j] != UINT32_MAX) {
          PopSystemTemp();
        }
      }
      // Release the address register.
      PopSystemTemp();
    }
    if (system_temp_memexport_written_ != UINT32_MAX) {
      PopSystemTemp();
    }
  }

  // Write stage-specific epilogue.
  if (is_vertex_shader()) {
    CompleteVertexOrDomainShader();
  } else if (is_pixel_shader()) {
    CompletePixelShader();
  }

  // Return from `main`.
  a_.OpRet();

  if (is_vertex_shader()) {
    // Release system_temp_position_ and
    // system_temp_point_size_edge_flag_kill_vertex_.
    PopSystemTemp(2);
  } else if (is_pixel_shader()) {
    // Release system_temps_color_.
    uint32_t shader_writes_color_targets =
        current_shader().writes_color_targets();
    for (int32_t i = 3; i >= 0; --i) {
      if (shader_writes_color_targets & (1 << i)) {
        PopSystemTemp();
      }
    }
    if (IsDepthStencilSystemTempUsed()) {
      // Release system_temp_depth_stencil_.
      PopSystemTemp();
    }
    if (edram_rov_used_) {
      // Release system_temp_rov_params_.
      PopSystemTemp();
    }
  }
}

std::vector<uint8_t> DxbcShaderTranslator::CompleteTranslation() {
  // Because of shader_object_.resize(), pointers can't be kept persistently
  // here! Resizing also zeroes the memory.

  // Write the code epilogue.
  CompleteShaderCode();

  shader_object_.clear();

  // 6 or 7 blobs - RDEF, ISGN, optionally PCSG, OSGN, SHEX, SFI0, STAT.
  // Whether SFI0 is needed at this point is not known, always writing it.
  uint32_t blob_count = 6 + uint32_t(IsDxbcDomainShader());
  // Allocate space for the header and the blob offsets.
  shader_object_.resize(sizeof(dxbc::ContainerHeader) / sizeof(uint32_t) +
                        blob_count);

  uint32_t blob_offset_position_dwords =
      sizeof(dxbc::ContainerHeader) / sizeof(uint32_t);
  uint32_t blob_position_dwords = uint32_t(shader_object_.size());
  constexpr uint32_t kBlobHeaderSizeDwords =
      sizeof(dxbc::BlobHeader) / sizeof(uint32_t);

  // Resource definition.
  shader_object_[blob_offset_position_dwords] =
      uint32_t(blob_position_dwords * sizeof(uint32_t));
  shader_object_.resize(blob_position_dwords + kBlobHeaderSizeDwords);
  WriteResourceDefinition();
  {
    auto& blob_header = *reinterpret_cast<dxbc::BlobHeader*>(
        shader_object_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kResourceDefinition;
    blob_position_dwords = uint32_t(shader_object_.size());
    blob_header.size_bytes =
        (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
        shader_object_[blob_offset_position_dwords++];
  }

  // Input signature.
  shader_object_[blob_offset_position_dwords] =
      uint32_t(blob_position_dwords * sizeof(uint32_t));
  shader_object_.resize(blob_position_dwords + kBlobHeaderSizeDwords);
  WriteInputSignature();
  {
    auto& blob_header = *reinterpret_cast<dxbc::BlobHeader*>(
        shader_object_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kInputSignature;
    blob_position_dwords = uint32_t(shader_object_.size());
    blob_header.size_bytes =
        (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
        shader_object_[blob_offset_position_dwords++];
  }

  // Patch constant signature.
  if (IsDxbcDomainShader()) {
    shader_object_[blob_offset_position_dwords] =
        uint32_t(blob_position_dwords * sizeof(uint32_t));
    shader_object_.resize(blob_position_dwords + kBlobHeaderSizeDwords);
    WritePatchConstantSignature();
    {
      auto& blob_header = *reinterpret_cast<dxbc::BlobHeader*>(
          shader_object_.data() + blob_position_dwords);
      blob_header.fourcc = dxbc::BlobHeader::FourCC::kPatchConstantSignature;
      blob_position_dwords = uint32_t(shader_object_.size());
      blob_header.size_bytes =
          (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
          shader_object_[blob_offset_position_dwords++];
    }
  }

  // Output signature.
  shader_object_[blob_offset_position_dwords] =
      uint32_t(blob_position_dwords * sizeof(uint32_t));
  shader_object_.resize(blob_position_dwords + kBlobHeaderSizeDwords);
  WriteOutputSignature();
  {
    auto& blob_header = *reinterpret_cast<dxbc::BlobHeader*>(
        shader_object_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kOutputSignature;
    blob_position_dwords = uint32_t(shader_object_.size());
    blob_header.size_bytes =
        (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
        shader_object_[blob_offset_position_dwords++];
  }

  // Shader program.
  shader_object_[blob_offset_position_dwords] =
      uint32_t(blob_position_dwords * sizeof(uint32_t));
  shader_object_.resize(blob_position_dwords + kBlobHeaderSizeDwords);
  WriteShaderCode();
  {
    auto& blob_header = *reinterpret_cast<dxbc::BlobHeader*>(
        shader_object_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kShaderEx;
    blob_position_dwords = uint32_t(shader_object_.size());
    blob_header.size_bytes =
        (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
        shader_object_[blob_offset_position_dwords++];
  }

  // Shader feature info.
  shader_object_[blob_offset_position_dwords] =
      uint32_t(blob_position_dwords * sizeof(uint32_t));
  shader_object_.resize(blob_position_dwords + kBlobHeaderSizeDwords +
                        sizeof(dxbc::ShaderFeatureInfo) / sizeof(uint32_t));
  std::memcpy(
      shader_object_.data() + blob_position_dwords + kBlobHeaderSizeDwords,
      &shader_feature_info_, sizeof(shader_feature_info_));
  {
    auto& blob_header = *reinterpret_cast<dxbc::BlobHeader*>(
        shader_object_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kShaderFeatureInfo;
    blob_position_dwords = uint32_t(shader_object_.size());
    blob_header.size_bytes =
        (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
        shader_object_[blob_offset_position_dwords++];
  }

  // Statistics.
  shader_object_[blob_offset_position_dwords] =
      uint32_t(blob_position_dwords * sizeof(uint32_t));
  shader_object_.resize(blob_position_dwords + kBlobHeaderSizeDwords +
                        sizeof(dxbc::Statistics) / sizeof(uint32_t));
  std::memcpy(
      shader_object_.data() + blob_position_dwords + kBlobHeaderSizeDwords,
      &statistics_, sizeof(statistics_));
  {
    auto& blob_header = *reinterpret_cast<dxbc::BlobHeader*>(
        shader_object_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kStatistics;
    blob_position_dwords = uint32_t(shader_object_.size());
    blob_header.size_bytes =
        (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
        shader_object_[blob_offset_position_dwords++];
  }

  // Header.
  uint32_t shader_object_size_bytes =
      uint32_t(shader_object_.size() * sizeof(uint32_t));
  {
    auto& container_header =
        *reinterpret_cast<dxbc::ContainerHeader*>(shader_object_.data());
    container_header.InitializeIdentification();
    container_header.size_bytes = shader_object_size_bytes;
    container_header.blob_count = blob_count;
    CalculateDXBCChecksum(
        reinterpret_cast<unsigned char*>(shader_object_.data()),
        static_cast<unsigned int>(shader_object_size_bytes),
        reinterpret_cast<unsigned int*>(&container_header.hash));
  }

  // TODO(Triang3l): Avoid copy?
  std::vector<uint8_t> shader_object_bytes;
  shader_object_bytes.resize(shader_object_size_bytes);
  std::memcpy(shader_object_bytes.data(), shader_object_.data(),
              shader_object_size_bytes);
  return shader_object_bytes;
}

void DxbcShaderTranslator::PostTranslation() {
  Shader::Translation& translation = current_translation();
  if (!translation.is_valid()) {
    return;
  }
  DxbcShader* dxbc_shader = dynamic_cast<DxbcShader*>(&translation.shader());
  if (dxbc_shader && !dxbc_shader->bindings_setup_entered_.test_and_set(
                         std::memory_order_relaxed)) {
    dxbc_shader->texture_bindings_.clear();
    dxbc_shader->texture_bindings_.reserve(texture_bindings_.size());
    dxbc_shader->used_texture_mask_ = 0;
    for (const TextureBinding& translator_binding : texture_bindings_) {
      DxbcShader::TextureBinding& shader_binding =
          dxbc_shader->texture_bindings_.emplace_back();
      // For a stable hash.
      std::memset(&shader_binding, 0, sizeof(shader_binding));
      shader_binding.bindless_descriptor_index =
          translator_binding.bindless_descriptor_index;
      shader_binding.fetch_constant = translator_binding.fetch_constant;
      shader_binding.dimension = translator_binding.dimension;
      shader_binding.is_signed = translator_binding.is_signed;
      dxbc_shader->used_texture_mask_ |= 1u
                                         << translator_binding.fetch_constant;
    }
    dxbc_shader->sampler_bindings_.clear();
    dxbc_shader->sampler_bindings_.reserve(sampler_bindings_.size());
    for (const SamplerBinding& translator_binding : sampler_bindings_) {
      DxbcShader::SamplerBinding& shader_binding =
          dxbc_shader->sampler_bindings_.emplace_back();
      shader_binding.bindless_descriptor_index =
          translator_binding.bindless_descriptor_index;
      shader_binding.fetch_constant = translator_binding.fetch_constant;
      shader_binding.mag_filter = translator_binding.mag_filter;
      shader_binding.min_filter = translator_binding.min_filter;
      shader_binding.mip_filter = translator_binding.mip_filter;
      shader_binding.aniso_filter = translator_binding.aniso_filter;
    }
  }
}

void DxbcShaderTranslator::EmitInstructionDisassembly() {
  if (!emit_source_map_) {
    return;
  }
  const char* source = instruction_disassembly_buffer_.buffer();
  uint32_t length = uint32_t(instruction_disassembly_buffer_.length());
  // Trim leading spaces and trailing new line.
  while (length != 0 && source[0] == ' ') {
    ++source;
    --length;
  }
  while (length != 0 && source[length - 1] == '\n') {
    --length;
  }
  if (length == 0) {
    return;
  }
  char* dest = reinterpret_cast<char*>(
      a_.OpCustomData(dxbc::CustomDataClass::kComment, length + 1));
  std::memcpy(dest, source, length);
  dest[length] = '\0';
}

dxbc::Src DxbcShaderTranslator::LoadOperand(const InstructionOperand& operand,
                                            uint32_t needed_components,
                                            bool& temp_pushed_out) {
  temp_pushed_out = false;

  uint32_t first_needed_component;
  if (!xe::bit_scan_forward(needed_components, &first_needed_component)) {
    return dxbc::Src::LF(0.0f);
  }

  dxbc::Index index(operand.storage_index);
  switch (operand.storage_addressing_mode) {
    case InstructionStorageAddressingMode::kStatic:
      break;
    case InstructionStorageAddressingMode::kAddressAbsolute:
      index = dxbc::Index(system_temp_ps_pc_p0_a0_, 3, operand.storage_index);
      break;
    case InstructionStorageAddressingMode::kAddressRelative:
      index = dxbc::Index(system_temp_aL_, 0, operand.storage_index);
      break;
  }

  dxbc::Src src(dxbc::Src::LF(0.0f));
  switch (operand.storage_source) {
    case InstructionStorageSource::kRegister: {
      if (current_shader().uses_register_dynamic_addressing()) {
        // Load x#[#] to r# because x#[#] can be used only with mov.
        uint32_t temp = PushSystemTemp();
        temp_pushed_out = true;
        uint32_t used_swizzle_components = 0;
        for (uint32_t i = 0; i < uint32_t(operand.component_count); ++i) {
          if (!(needed_components & (1 << i))) {
            continue;
          }
          SwizzleSource component = operand.GetComponent(i);
          assert_true(component >= SwizzleSource::kX &&
                      component <= SwizzleSource::kW);
          used_swizzle_components |=
              1 << (uint32_t(component) - uint32_t(SwizzleSource::kX));
        }
        assert_not_zero(used_swizzle_components);
        a_.OpMov(dxbc::Dest::R(temp, used_swizzle_components),
                 dxbc::Src::X(0, index));
        src = dxbc::Src::R(temp);
      } else {
        assert_true(operand.storage_addressing_mode ==
                    InstructionStorageAddressingMode::kStatic);
        src = dxbc::Src::R(index.index_);
      }
    } break;
    case InstructionStorageSource::kConstantFloat: {
      if (cbuffer_index_float_constants_ == kBindingIndexUnallocated) {
        cbuffer_index_float_constants_ = cbuffer_count_++;
      }
      const Shader::ConstantRegisterMap& constant_register_map =
          current_shader().constant_register_map();
      if (operand.storage_addressing_mode ==
          InstructionStorageAddressingMode::kStatic) {
        uint32_t float_constant_index =
            constant_register_map.GetPackedFloatConstantIndex(
                operand.storage_index);
        assert_true(float_constant_index != UINT32_MAX);
        if (float_constant_index == UINT32_MAX) {
          return dxbc::Src::LF(0.0f);
        }
        index.index_ = float_constant_index;
      } else {
        assert_true(constant_register_map.float_dynamic_addressing);
      }
      src = dxbc::Src::CB(cbuffer_index_float_constants_,
                          uint32_t(CbufferRegister::kFloatConstants), index);
    } break;
    default:
      assert_unhandled_case(operand.storage_source);
      return dxbc::Src::LF(0.0f);
  }

  // Swizzle, skipping unneeded components similar to how FXC skips components,
  // by replacing them with the leftmost used one.
  uint32_t swizzle = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    SwizzleSource component = operand.GetComponent(
        (needed_components & (1 << i)) ? i : first_needed_component);
    assert_true(component >= SwizzleSource::kX &&
                component <= SwizzleSource::kW);
    swizzle |= (uint32_t(component) - uint32_t(SwizzleSource::kX)) << (i * 2);
  }
  src = src.Swizzle(swizzle);

  return src.WithModifiers(operand.is_absolute_value, operand.is_negated);
}

void DxbcShaderTranslator::StoreResult(const InstructionResult& result,
                                       const dxbc::Src& src,
                                       bool can_store_memexport_address) {
  uint32_t used_write_mask = result.GetUsedWriteMask();
  if (!used_write_mask) {
    return;
  }

  // Get the destination address and type.
  dxbc::Dest dest(dxbc::Dest::Null());
  bool is_clamped = result.is_clamped;
  switch (result.storage_target) {
    case InstructionStorageTarget::kNone:
      return;
    case InstructionStorageTarget::kRegister:
      if (current_shader().uses_register_dynamic_addressing()) {
        dxbc::Index register_index(result.storage_index);
        switch (result.storage_addressing_mode) {
          case InstructionStorageAddressingMode::kStatic:
            break;
          case InstructionStorageAddressingMode::kAddressAbsolute:
            register_index =
                dxbc::Index(system_temp_ps_pc_p0_a0_, 3, result.storage_index);
            break;
          case InstructionStorageAddressingMode::kAddressRelative:
            register_index =
                dxbc::Index(system_temp_aL_, 0, result.storage_index);
            break;
        }
        dest = dxbc::Dest::X(0, register_index);
      } else {
        assert_true(result.storage_addressing_mode ==
                    InstructionStorageAddressingMode::kStatic);
        dest = dxbc::Dest::R(result.storage_index);
      }
      break;
    case InstructionStorageTarget::kInterpolator:
      dest = dxbc::Dest::O(uint32_t(InOutRegister::kVSDSOutInterpolators) +
                           result.storage_index);
      break;
    case InstructionStorageTarget::kPosition:
      dest = dxbc::Dest::R(system_temp_position_);
      break;
    case InstructionStorageTarget::kPointSizeEdgeFlagKillVertex:
      assert_zero(used_write_mask & 0b1000);
      dest = dxbc::Dest::R(system_temp_point_size_edge_flag_kill_vertex_);
      break;
    case InstructionStorageTarget::kExportAddress:
      // Validate memexport writes (Halo 3 has some weird invalid ones).
      if (!can_store_memexport_address || memexport_alloc_current_count_ == 0 ||
          memexport_alloc_current_count_ > Shader::kMaxMemExports ||
          system_temps_memexport_address_[memexport_alloc_current_count_ - 1] ==
              UINT32_MAX) {
        return;
      }
      dest = dxbc::Dest::R(
          system_temps_memexport_address_[memexport_alloc_current_count_ - 1]);
      break;
    case InstructionStorageTarget::kExportData: {
      // Validate memexport writes (Halo 3 has some weird invalid ones).
      if (memexport_alloc_current_count_ == 0 ||
          memexport_alloc_current_count_ > Shader::kMaxMemExports ||
          system_temps_memexport_data_[memexport_alloc_current_count_ - 1]
                                      [result.storage_index] == UINT32_MAX) {
        return;
      }
      dest = dxbc::Dest::R(
          system_temps_memexport_data_[memexport_alloc_current_count_ - 1]
                                      [result.storage_index]);
      // Mark that the eM# has been written to and needs to be exported.
      assert_not_zero(used_write_mask);
      uint32_t memexport_index = memexport_alloc_current_count_ - 1;
      a_.OpOr(dxbc::Dest::R(system_temp_memexport_written_,
                            1 << (memexport_index >> 2)),
              dxbc::Src::R(system_temp_memexport_written_)
                  .Select(memexport_index >> 2),
              dxbc::Src::LU(uint32_t(1) << (result.storage_index +
                                            ((memexport_index & 3) << 3))));
    } break;
    case InstructionStorageTarget::kColor:
      assert_not_zero(used_write_mask);
      assert_true(current_shader().writes_color_target(result.storage_index));
      dest = dxbc::Dest::R(system_temps_color_[result.storage_index]);
      if (edram_rov_used_) {
        // For ROV output, mark that the color has been written to.
        // According to:
        // https://docs.microsoft.com/en-us/windows/desktop/direct3dhlsl/dx9-graphics-reference-asm-ps-registers-output-color
        // if a color target hasn't been written to - including due to flow
        // control - the render target must not be modified (the unwritten
        // components of a written target are undefined, not sure if this
        // behavior is respected on the real GPU, but the ROV code currently
        // doesn't preserve unmodified components).
        a_.OpOr(dxbc::Dest::R(system_temp_rov_params_, 0b0001),
                dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
                dxbc::Src::LU(uint32_t(1) << (8 + result.storage_index)));
      }
      break;
    case InstructionStorageTarget::kDepth:
      // Writes X to scalar oDepth or to X of system_temp_depth_stencil_, no
      // additional swizzling needed.
      assert_true(used_write_mask == 0b0001);
      assert_true(current_shader().writes_depth());
      if (IsDepthStencilSystemTempUsed()) {
        dest = dxbc::Dest::R(system_temp_depth_stencil_);
      } else {
        dest = dxbc::Dest::ODepth();
      }
      // Depth outside [0, 1] is not safe for use with the ROV code, with
      // 20e4-as-32 conversion and with 0...1 to 0...0.5 float24 remapping.
      // Though 20e4 float depth can store values between 1 and 2, it's a very
      // unusual case. Direct3D 10+ SV_Depth, however, can accept any values,
      // including specials, when the depth buffer is floating-point.
      is_clamped = true;
      break;
  }
  if (dest.type_ == dxbc::OperandType::kNull) {
    return;
  }

  // Write.
  uint32_t src_additional_swizzle = 0;
  uint32_t constant_mask = 0, constant_1_mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(used_write_mask & (1 << i))) {
      continue;
    }
    SwizzleSource component = result.components[i];
    if (component >= SwizzleSource::kX && component <= SwizzleSource::kW) {
      src_additional_swizzle |=
          (uint32_t(component) - uint32_t(SwizzleSource::kX)) << (i * 2);
    } else {
      constant_mask |= 1 << i;
      if (component == SwizzleSource::k1) {
        constant_1_mask |= 1 << i;
      }
    }
  }
  if (used_write_mask != constant_mask) {
    a_.OpMov(dest.Mask(used_write_mask & ~constant_mask),
             src.SwizzleSwizzled(src_additional_swizzle), is_clamped);
  }
  if (constant_mask) {
    a_.OpMov(dest.Mask(constant_mask),
             dxbc::Src::LF(float(constant_1_mask & 1),
                           float((constant_1_mask >> 1) & 1),
                           float((constant_1_mask >> 2) & 1),
                           float((constant_1_mask >> 3) & 1)));
  }
}

void DxbcShaderTranslator::UpdateExecConditionalsAndEmitDisassembly(
    ParsedExecInstruction::Type type, uint32_t bool_constant_index,
    bool condition) {
  // Check if we can merge the new exec with the previous one, or the jump with
  // the previous exec. The instruction-level predicate check is also merged in
  // this case.
  bool merge = false;
  if (type == ParsedExecInstruction::Type::kConditional) {
    // Can merge conditional with conditional, as long as the bool constant and
    // the expected values are the same.
    if (cf_exec_bool_constant_ == bool_constant_index &&
        cf_exec_bool_constant_condition_ == condition) {
      merge = true;
    }
  } else if (type == ParsedExecInstruction::Type::kPredicated) {
    // Can merge predicated with predicated if the conditions are the same and
    // the previous exec hasn't modified the predicate register.
    if (!cf_exec_predicate_written_ && cf_exec_predicated_ &&
        cf_exec_predicate_condition_ == condition) {
      merge = true;
    }
  } else {
    // Can merge unconditional with unconditional.
    if (cf_exec_bool_constant_ == kCfExecBoolConstantNone &&
        !cf_exec_predicated_) {
      merge = true;
    }
  }

  if (merge) {
    // Emit the disassembly for the exec/jump merged with the previous one.
    EmitInstructionDisassembly();
    return;
  }

  CloseExecConditionals();

  // Emit the disassembly for the new exec/jump.
  EmitInstructionDisassembly();

  if (type == ParsedExecInstruction::Type::kConditional) {
    uint32_t bool_constant_test_temp = PushSystemTemp();
    // Check the bool constant value.
    if (cbuffer_index_bool_loop_constants_ == kBindingIndexUnallocated) {
      cbuffer_index_bool_loop_constants_ = cbuffer_count_++;
    }
    a_.OpAnd(dxbc::Dest::R(bool_constant_test_temp, 0b0001),
             dxbc::Src::CB(cbuffer_index_bool_loop_constants_,
                           uint32_t(CbufferRegister::kBoolLoopConstants),
                           bool_constant_index >> 7)
                 .Select((bool_constant_index >> 5) & 3),
             dxbc::Src::LU(uint32_t(1) << (bool_constant_index & 31)));
    // Open the new `if`.
    a_.OpIf(condition, dxbc::Src::R(bool_constant_test_temp, dxbc::Src::kXXXX));
    // Release bool_constant_test_temp.
    PopSystemTemp();
    cf_exec_bool_constant_ = bool_constant_index;
    cf_exec_bool_constant_condition_ = condition;
  } else if (type == ParsedExecInstruction::Type::kPredicated) {
    a_.OpIf(condition,
            dxbc::Src::R(system_temp_ps_pc_p0_a0_, dxbc::Src::kZZZZ));
    cf_exec_predicated_ = true;
    cf_exec_predicate_condition_ = condition;
  }
}

void DxbcShaderTranslator::CloseExecConditionals() {
  // Within the exec - instruction-level predicate check.
  CloseInstructionPredication();
  // Exec level.
  if (cf_exec_bool_constant_ != kCfExecBoolConstantNone ||
      cf_exec_predicated_) {
    a_.OpEndIf();
    cf_exec_bool_constant_ = kCfExecBoolConstantNone;
    cf_exec_predicated_ = false;
  }
  // Nothing relies on the predicate value being unchanged now.
  cf_exec_predicate_written_ = false;
}

void DxbcShaderTranslator::UpdateInstructionPredicationAndEmitDisassembly(
    bool predicated, bool condition) {
  if (!predicated) {
    CloseInstructionPredication();
    EmitInstructionDisassembly();
    return;
  }

  if (cf_instruction_predicate_if_open_) {
    if (cf_instruction_predicate_condition_ == condition) {
      // Already in the needed instruction-level `if`.
      EmitInstructionDisassembly();
      return;
    }
    CloseInstructionPredication();
  }

  // Emit the disassembly before opening (or not opening) the new conditional.
  EmitInstructionDisassembly();

  // If the instruction predicate condition is the same as the exec predicate
  // condition, no need to open a check. However, if there was a `setp` prior
  // to this instruction, the predicate value now may be different than it was
  // in the beginning of the exec.
  if (!cf_exec_predicate_written_ && cf_exec_predicated_ &&
      cf_exec_predicate_condition_ == condition) {
    return;
  }

  a_.OpIf(condition, dxbc::Src::R(system_temp_ps_pc_p0_a0_, dxbc::Src::kZZZZ));
  cf_instruction_predicate_if_open_ = true;
  cf_instruction_predicate_condition_ = condition;
}

void DxbcShaderTranslator::CloseInstructionPredication() {
  if (cf_instruction_predicate_if_open_) {
    a_.OpEndIf();
    cf_instruction_predicate_if_open_ = false;
  }
}

void DxbcShaderTranslator::JumpToLabel(uint32_t address) {
  a_.OpMov(dxbc::Dest::R(system_temp_ps_pc_p0_a0_, 0b0010),
           dxbc::Src::LU(address));
  a_.OpContinue();
}

void DxbcShaderTranslator::ProcessLabel(uint32_t cf_index) {
  if (cf_index == 0) {
    // 0 already added in the beginning.
    return;
  }
  // Close flow control on the deeper levels below - prevent attempts to merge
  // execs across labels.
  CloseExecConditionals();
  if (UseSwitchForControlFlow()) {
    // Fallthrough to the label from the previous one on the next iteration if
    // no `continue` was done. Can't simply fallthrough because in DXBC, a
    // non-empty switch case must end with a break.
    JumpToLabel(cf_index);
    // Close the previous label.
    a_.OpBreak();
    // Go to the next label.
    a_.OpCase(dxbc::Src::LU(cf_index));
  } else {
    // Close the previous label.
    a_.OpEndIf();
    // if (pc <= cf_index)
    uint32_t test_temp = PushSystemTemp();
    a_.OpUGE(dxbc::Dest::R(test_temp, 0b0001), dxbc::Src::LU(cf_index),
             dxbc::Src::R(system_temp_ps_pc_p0_a0_, dxbc::Src::kYYYY));
    a_.OpIf(true, dxbc::Src::R(test_temp, dxbc::Src::kXXXX));
    // Release test_temp.
    PopSystemTemp();
  }
}

void DxbcShaderTranslator::ProcessExecInstructionBegin(
    const ParsedExecInstruction& instr) {
  if (emit_source_map_) {
    instruction_disassembly_buffer_.Reset();
    instr.Disassemble(&instruction_disassembly_buffer_);
  }
  UpdateExecConditionalsAndEmitDisassembly(
      instr.type, instr.bool_constant_index, instr.condition);
}

void DxbcShaderTranslator::ProcessExecInstructionEnd(
    const ParsedExecInstruction& instr) {
  if (instr.is_end) {
    // Break out of the main loop.
    CloseInstructionPredication();
    if (UseSwitchForControlFlow()) {
      // Write an invalid value to pc.
      a_.OpMov(dxbc::Dest::R(system_temp_ps_pc_p0_a0_, 0b0010),
               dxbc::Src::LU(UINT32_MAX));
      // Go to the next iteration, where switch cases won't be reached.
      a_.OpContinue();
    } else {
      a_.OpBreak();
    }
  }
}

void DxbcShaderTranslator::ProcessLoopStartInstruction(
    const ParsedLoopStartInstruction& instr) {
  // loop il<idx>, L<idx> - loop with loop data il<idx>, end @ L<idx>

  // Loop control is outside execs - actually close the last exec.
  CloseExecConditionals();

  if (emit_source_map_) {
    instruction_disassembly_buffer_.Reset();
    instr.Disassemble(&instruction_disassembly_buffer_);
    EmitInstructionDisassembly();
  }

  // Count (unsigned) in bits 0:7 of the loop constant, initial aL (unsigned) in
  // 8:15. Starting from vector 2 because of bool constants.
  if (cbuffer_index_bool_loop_constants_ == kBindingIndexUnallocated) {
    cbuffer_index_bool_loop_constants_ = cbuffer_count_++;
  }
  dxbc::Src loop_constant_src(
      dxbc::Src::CB(cbuffer_index_bool_loop_constants_,
                    uint32_t(CbufferRegister::kBoolLoopConstants),
                    2 + (instr.loop_constant_index >> 2))
          .Select(instr.loop_constant_index & 3));

  // Push the count to the loop count stack - move XYZ to YZW and set X to this
  // loop count.
  a_.OpMov(dxbc::Dest::R(system_temp_loop_count_, 0b1110),
           dxbc::Src::R(system_temp_loop_count_, 0b10010000));
  a_.OpAnd(dxbc::Dest::R(system_temp_loop_count_, 0b0001), loop_constant_src,
           dxbc::Src::LU(UINT8_MAX));

  // Push aL - keep the same value as in the previous loop if repeating, or the
  // new one otherwise.
  a_.OpMov(dxbc::Dest::R(system_temp_aL_, instr.is_repeat ? 0b1111 : 0b1110),
           dxbc::Src::R(system_temp_aL_, 0b10010000));
  if (!instr.is_repeat) {
    a_.OpUBFE(dxbc::Dest::R(system_temp_aL_, 0b0001), dxbc::Src::LU(8),
              dxbc::Src::LU(8), loop_constant_src);
  }

  // Break if the loop counter is 0 (since the condition is checked in the end).
  a_.OpIf(false, dxbc::Src::R(system_temp_loop_count_, dxbc::Src::kXXXX));
  JumpToLabel(instr.loop_skip_address);
  a_.OpEndIf();
}

void DxbcShaderTranslator::ProcessLoopEndInstruction(
    const ParsedLoopEndInstruction& instr) {
  // endloop il<idx>, L<idx> - end loop w/ data il<idx>, head @ L<idx>

  // Loop control is outside execs - actually close the last exec.
  CloseExecConditionals();

  if (emit_source_map_) {
    instruction_disassembly_buffer_.Reset();
    instr.Disassemble(&instruction_disassembly_buffer_);
    EmitInstructionDisassembly();
  }

  // Subtract 1 from the loop counter.
  a_.OpIAdd(dxbc::Dest::R(system_temp_loop_count_, 0b0001),
            dxbc::Src::R(system_temp_loop_count_, dxbc::Src::kXXXX),
            dxbc::Src::LI(-1));

  if (instr.is_predicated_break) {
    // if (loop_count.x == 0 || [!]p0)
    uint32_t break_case_temp = PushSystemTemp();
    if (instr.predicate_condition) {
      // If p0 is non-zero, set the test value to 0 (since if_z is used,
      // otherwise check if the loop counter is zero).
      a_.OpMovC(dxbc::Dest::R(break_case_temp, 0b0001),
                dxbc::Src::R(system_temp_ps_pc_p0_a0_, dxbc::Src::kZZZZ),
                dxbc::Src::LU(0),
                dxbc::Src::R(system_temp_loop_count_, dxbc::Src::kXXXX));
    } else {
      // If p0 is zero, set the test value to 0 (since if_z is used, otherwise
      // check if the loop counter is zero).
      a_.OpMovC(dxbc::Dest::R(break_case_temp, 0b0001),
                dxbc::Src::R(system_temp_ps_pc_p0_a0_, dxbc::Src::kZZZZ),
                dxbc::Src::R(system_temp_loop_count_, dxbc::Src::kXXXX),
                dxbc::Src::LU(0));
    }
    a_.OpIf(false, dxbc::Src::R(break_case_temp, dxbc::Src::kXXXX));
    // Release break_case_temp.
    PopSystemTemp();
  } else {
    // if (loop_count.x == 0)
    a_.OpIf(false, dxbc::Src::R(system_temp_loop_count_, dxbc::Src::kXXXX));
  }
  {
    // Break case.
    // Pop the current loop off the loop counter and the relative address
    // stacks - move YZW to XYZ and set W to 0.
    a_.OpMov(dxbc::Dest::R(system_temp_loop_count_, 0b0111),
             dxbc::Src::R(system_temp_loop_count_, 0b111001));
    a_.OpMov(dxbc::Dest::R(system_temp_loop_count_, 0b1000), dxbc::Src::LU(0));
    a_.OpMov(dxbc::Dest::R(system_temp_aL_, 0b0111),
             dxbc::Src::R(system_temp_aL_, 0b111001));
    a_.OpMov(dxbc::Dest::R(system_temp_aL_, 0b1000), dxbc::Src::LI(0));
    // Now going to fall through to the next exec (no need to jump).
  }
  a_.OpElse();
  {
    // Continue case.
    uint32_t aL_add_temp = PushSystemTemp();
    // Extract the value to add to aL (signed, in bits 16:23 of the loop
    // constant). Starting from vector 2 because of bool constants.
    if (cbuffer_index_bool_loop_constants_ == kBindingIndexUnallocated) {
      cbuffer_index_bool_loop_constants_ = cbuffer_count_++;
    }
    a_.OpIBFE(dxbc::Dest::R(aL_add_temp, 0b0001), dxbc::Src::LU(8),
              dxbc::Src::LU(16),
              dxbc::Src::CB(cbuffer_index_bool_loop_constants_,
                            uint32_t(CbufferRegister::kBoolLoopConstants),
                            2 + (instr.loop_constant_index >> 2))
                  .Select(instr.loop_constant_index & 3));
    // Add the needed value to aL.
    a_.OpIAdd(dxbc::Dest::R(system_temp_aL_, 0b0001),
              dxbc::Src::R(system_temp_aL_, dxbc::Src::kXXXX),
              dxbc::Src::R(aL_add_temp, dxbc::Src::kXXXX));
    // Release aL_add_temp.
    PopSystemTemp();
    // Jump back to the beginning of the loop body.
    JumpToLabel(instr.loop_body_address);
  }
  a_.OpEndIf();
}

void DxbcShaderTranslator::ProcessJumpInstruction(
    const ParsedJumpInstruction& instr) {
  if (emit_source_map_) {
    instruction_disassembly_buffer_.Reset();
    instr.Disassemble(&instruction_disassembly_buffer_);
  }

  // Treat like exec, merge with execs if possible, since it's an if too.
  ParsedExecInstruction::Type type;
  if (instr.type == ParsedJumpInstruction::Type::kConditional) {
    type = ParsedExecInstruction::Type::kConditional;
  } else if (instr.type == ParsedJumpInstruction::Type::kPredicated) {
    type = ParsedExecInstruction::Type::kPredicated;
  } else {
    type = ParsedExecInstruction::Type::kUnconditional;
  }
  UpdateExecConditionalsAndEmitDisassembly(type, instr.bool_constant_index,
                                           instr.condition);

  // UpdateExecConditionalsAndEmitDisassembly may not necessarily close the
  // instruction-level predicate check (it's not necessary if the execs are
  // merged), but here the instruction itself is on the flow control level, so
  // the predicate check is on the flow control level too.
  CloseInstructionPredication();

  JumpToLabel(instr.target_address);
}

void DxbcShaderTranslator::ProcessAllocInstruction(
    const ParsedAllocInstruction& instr) {
  if (emit_source_map_) {
    instruction_disassembly_buffer_.Reset();
    instr.Disassemble(&instruction_disassembly_buffer_);
    EmitInstructionDisassembly();
  }

  if (instr.type == AllocType::kMemory) {
    ++memexport_alloc_current_count_;
  }
}

const DxbcShaderTranslator::ShaderRdefType
    DxbcShaderTranslator::rdef_types_[size_t(
        DxbcShaderTranslator::ShaderRdefTypeIndex::kCount)] = {
        // kFloat
        {"float", dxbc::RdefVariableClass::kScalar,
         dxbc::RdefVariableType::kFloat, 1, 1, 0,
         ShaderRdefTypeIndex::kUnknown},
        // kFloat2
        {"float2", dxbc::RdefVariableClass::kVector,
         dxbc::RdefVariableType::kFloat, 1, 2, 0,
         ShaderRdefTypeIndex::kUnknown},
        // kFloat3
        {"float3", dxbc::RdefVariableClass::kVector,
         dxbc::RdefVariableType::kFloat, 1, 3, 0,
         ShaderRdefTypeIndex::kUnknown},
        // kFloat4
        {"float4", dxbc::RdefVariableClass::kVector,
         dxbc::RdefVariableType::kFloat, 1, 4, 0,
         ShaderRdefTypeIndex::kUnknown},
        // kUint
        {"dword", dxbc::RdefVariableClass::kScalar,
         dxbc::RdefVariableType::kUInt, 1, 1, 0, ShaderRdefTypeIndex::kUnknown},
        // kUint2
        {"uint2", dxbc::RdefVariableClass::kVector,
         dxbc::RdefVariableType::kUInt, 1, 2, 0, ShaderRdefTypeIndex::kUnknown},
        // kUint4
        {"uint4", dxbc::RdefVariableClass::kVector,
         dxbc::RdefVariableType::kUInt, 1, 4, 0, ShaderRdefTypeIndex::kUnknown},
        // kFloat4Array4
        {nullptr, dxbc::RdefVariableClass::kVector,
         dxbc::RdefVariableType::kFloat, 1, 4, 4, ShaderRdefTypeIndex::kFloat4},
        // kFloat4Array6
        {nullptr, dxbc::RdefVariableClass::kVector,
         dxbc::RdefVariableType::kFloat, 1, 4, 6, ShaderRdefTypeIndex::kFloat4},
        // kFloat4ConstantArray - float constants - size written dynamically.
        {nullptr, dxbc::RdefVariableClass::kVector,
         dxbc::RdefVariableType::kFloat, 1, 4, 0, ShaderRdefTypeIndex::kFloat4},
        // kUint4Array2
        {nullptr, dxbc::RdefVariableClass::kVector,
         dxbc::RdefVariableType::kUInt, 1, 4, 2, ShaderRdefTypeIndex::kUint4},
        // kUint4Array8
        {nullptr, dxbc::RdefVariableClass::kVector,
         dxbc::RdefVariableType::kUInt, 1, 4, 8, ShaderRdefTypeIndex::kUint4},
        // kUint4Array48
        {nullptr, dxbc::RdefVariableClass::kVector,
         dxbc::RdefVariableType::kUInt, 1, 4, 48, ShaderRdefTypeIndex::kUint4},
        // kUint4DescriptorIndexArray - bindless descriptor indices - size
        // written
        // dynamically.
        {nullptr, dxbc::RdefVariableClass::kVector,
         dxbc::RdefVariableType::kUInt, 1, 4, 0, ShaderRdefTypeIndex::kUint4},
};

const DxbcShaderTranslator::SystemConstantRdef
    DxbcShaderTranslator::system_constant_rdef_[size_t(
        DxbcShaderTranslator::SystemConstants::Index::kCount)] = {
        {"xe_flags", ShaderRdefTypeIndex::kUint, sizeof(uint32_t)},
        {"xe_tessellation_factor_range", ShaderRdefTypeIndex::kFloat2,
         sizeof(float) * 2},
        {"xe_line_loop_closing_index", ShaderRdefTypeIndex::kUint,
         sizeof(uint32_t)},

        {"xe_vertex_index_endian", ShaderRdefTypeIndex::kUint,
         sizeof(uint32_t)},
        {"xe_vertex_index_offset", ShaderRdefTypeIndex::kUint, sizeof(int32_t)},
        {"xe_vertex_index_min_max", ShaderRdefTypeIndex::kUint2,
         sizeof(uint32_t) * 2},

        {"xe_user_clip_planes", ShaderRdefTypeIndex::kFloat4Array6,
         sizeof(float) * 4 * 6},

        {"xe_ndc_scale", ShaderRdefTypeIndex::kFloat3, sizeof(float) * 3},
        {"xe_point_size_x", ShaderRdefTypeIndex::kFloat, sizeof(float)},

        {"xe_ndc_offset", ShaderRdefTypeIndex::kFloat3, sizeof(float) * 3},
        {"xe_point_size_y", ShaderRdefTypeIndex::kFloat, sizeof(float)},

        {"xe_point_size_min_max", ShaderRdefTypeIndex::kFloat2,
         sizeof(float) * 2},
        {"xe_point_screen_to_ndc", ShaderRdefTypeIndex::kFloat2,
         sizeof(float) * 2},

        {"xe_interpolator_sampling_pattern", ShaderRdefTypeIndex::kUint,
         sizeof(uint32_t)},
        {"xe_ps_param_gen", ShaderRdefTypeIndex::kUint, sizeof(uint32_t)},
        {"xe_sample_count_log2", ShaderRdefTypeIndex::kUint2,
         sizeof(uint32_t) * 2},

        {"xe_texture_swizzled_signs", ShaderRdefTypeIndex::kUint4Array2,
         sizeof(uint32_t) * 4 * 2},

        {"xe_textures_resolved", ShaderRdefTypeIndex::kUint, sizeof(uint32_t)},
        {"xe_alpha_test_reference", ShaderRdefTypeIndex::kFloat, sizeof(float)},
        {"xe_alpha_to_mask", ShaderRdefTypeIndex::kUint, sizeof(uint32_t)},
        {"xe_edram_pitch_tiles", ShaderRdefTypeIndex::kUint, sizeof(uint32_t)},

        {"xe_color_exp_bias", ShaderRdefTypeIndex::kFloat4, sizeof(float) * 4},

        {"xe_edram_poly_offset_front", ShaderRdefTypeIndex::kFloat2,
         sizeof(float) * 2},
        {"xe_edram_poly_offset_back", ShaderRdefTypeIndex::kFloat2,
         sizeof(float) * 2},

        {"xe_edram_depth_base_dwords", ShaderRdefTypeIndex::kUint,
         sizeof(uint32_t), sizeof(float) * 3},

        {"xe_edram_stencil", ShaderRdefTypeIndex::kUint4Array2,
         sizeof(uint32_t) * 4 * 2},

        {"xe_edram_rt_base_dwords_scaled", ShaderRdefTypeIndex::kUint4,
         sizeof(uint32_t) * 4},

        {"xe_edram_rt_format_flags", ShaderRdefTypeIndex::kUint4,
         sizeof(uint32_t) * 4},

        {"xe_edram_rt_clamp", ShaderRdefTypeIndex::kFloat4Array4,
         sizeof(float) * 4 * 4},

        {"xe_edram_rt_keep_mask", ShaderRdefTypeIndex::kUint4Array2,
         sizeof(uint32_t) * 4 * 2},

        {"xe_edram_rt_blend_factors_ops", ShaderRdefTypeIndex::kUint4,
         sizeof(uint32_t) * 4},

        {"xe_edram_blend_constant", ShaderRdefTypeIndex::kFloat4,
         sizeof(float) * 4},
};

void DxbcShaderTranslator::WriteResourceDefinition() {
  // Because of shader_object_.resize(), pointers can't be kept persistently
  // here! Resizing also zeroes the memory.

  uint32_t blob_position_dwords = uint32_t(shader_object_.size());
  uint32_t name_ptr;

  const Shader::ConstantRegisterMap& constant_register_map =
      current_shader().constant_register_map();

  // Allocate space for the header, will fill when all pointers and counts are
  // known.
  shader_object_.resize(shader_object_.size() +
                        sizeof(dxbc::RdefHeader) / sizeof(uint32_t));
  // Generator name.
  dxbc::AppendAlignedString(shader_object_, "Xenia");

  // ***************************************************************************
  // Constant types
  // ***************************************************************************

  // Type names.
  name_ptr = (uint32_t(shader_object_.size()) - blob_position_dwords) *
             sizeof(uint32_t);
  uint32_t type_name_ptrs[size_t(ShaderRdefTypeIndex::kCount)];
  for (uint32_t i = 0; i < uint32_t(ShaderRdefTypeIndex::kCount); ++i) {
    const ShaderRdefType& type = rdef_types_[i];
    if (type.name == nullptr) {
      // Array - use the name of the element type.
      assert_true(uint32_t(type.array_element_type) < i);
      type_name_ptrs[i] = type_name_ptrs[uint32_t(type.array_element_type)];
      continue;
    }
    type_name_ptrs[i] = name_ptr;
    name_ptr += dxbc::AppendAlignedString(shader_object_, type.name);
  }
  // Types.
  uint32_t types_position_dwords = uint32_t(shader_object_.size());
  uint32_t types_ptr =
      (types_position_dwords - blob_position_dwords) * sizeof(uint32_t);
  shader_object_.resize(types_position_dwords +
                        sizeof(dxbc::RdefType) / sizeof(uint32_t) *
                            uint32_t(ShaderRdefTypeIndex::kCount));
  {
    auto types = reinterpret_cast<dxbc::RdefType*>(shader_object_.data() +
                                                   types_position_dwords);
    for (uint32_t i = 0; i < uint32_t(ShaderRdefTypeIndex::kCount); ++i) {
      dxbc::RdefType& type = types[i];
      const ShaderRdefType& translator_type = rdef_types_[i];
      type.variable_class = translator_type.variable_class;
      type.variable_type = translator_type.variable_type;
      type.row_count = translator_type.row_count;
      type.column_count = translator_type.column_count;
      switch (ShaderRdefTypeIndex(i)) {
        case ShaderRdefTypeIndex::kFloat4ConstantArray:
          // Declaring a 0-sized array may not be safe, so write something valid
          // even if they aren't used.
          type.element_count = std::max(
              uint16_t(constant_register_map.float_count), uint16_t(1));
          break;
        case ShaderRdefTypeIndex::kUint4DescriptorIndexArray:
          type.element_count = std::max(
              uint16_t((GetBindlessResourceCount() + 3) >> 2), uint16_t(1));
          break;
        default:
          type.element_count = translator_type.element_count;
      }
      type.name_ptr = type_name_ptrs[i];
    }
  }

  // ***************************************************************************
  // Constants
  // ***************************************************************************

  // Names.
  name_ptr = (uint32_t(shader_object_.size()) - blob_position_dwords) *
             sizeof(uint32_t);
  uint32_t constant_name_ptrs_system[size_t(SystemConstants::Index::kCount)];
  if (cbuffer_index_system_constants_ != kBindingIndexUnallocated) {
    for (size_t i = 0; i < size_t(SystemConstants::Index::kCount); ++i) {
      constant_name_ptrs_system[i] = name_ptr;
      name_ptr += dxbc::AppendAlignedString(shader_object_,
                                            system_constant_rdef_[i].name);
    }
  }
  uint32_t constant_name_ptr_float = name_ptr;
  if (cbuffer_index_float_constants_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_float_constants");
  }
  uint32_t constant_name_ptr_bool = name_ptr;
  uint32_t constant_name_ptr_loop = name_ptr;
  if (cbuffer_index_bool_loop_constants_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_bool_constants");
    constant_name_ptr_loop = name_ptr;
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_loop_constants");
  }
  uint32_t constant_name_ptr_fetch = name_ptr;
  if (cbuffer_index_fetch_constants_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_fetch_constants");
  }
  uint32_t constant_name_ptr_descriptor_indices = name_ptr;
  if (cbuffer_index_descriptor_indices_ != kBindingIndexUnallocated) {
    name_ptr +=
        dxbc::AppendAlignedString(shader_object_, "xe_descriptor_indices");
  }

  // System constants.
  uint32_t constant_position_dwords_system = uint32_t(shader_object_.size());
  if (cbuffer_index_system_constants_ != kBindingIndexUnallocated) {
    shader_object_.resize(constant_position_dwords_system +
                          sizeof(dxbc::RdefVariable) / sizeof(uint32_t) *
                              size_t(SystemConstants::Index::kCount));
    auto constants_system = reinterpret_cast<dxbc::RdefVariable*>(
        shader_object_.data() + constant_position_dwords_system);
    uint32_t constant_offset_system = 0;
    for (size_t i = 0; i < size_t(SystemConstants::Index::kCount); ++i) {
      dxbc::RdefVariable& constant_system = constants_system[i];
      const SystemConstantRdef& translator_constant_system =
          system_constant_rdef_[i];
      constant_system.name_ptr = constant_name_ptrs_system[i];
      constant_system.start_offset_bytes = constant_offset_system;
      constant_system.size_bytes = translator_constant_system.size;
      constant_system.flags = (system_constants_used_ & (uint64_t(1) << i))
                                  ? dxbc::kRdefVariableFlagUsed
                                  : 0;
      constant_system.type_ptr =
          types_ptr +
          sizeof(dxbc::RdefType) * uint32_t(translator_constant_system.type);
      constant_system.start_texture = UINT32_MAX;
      constant_system.start_sampler = UINT32_MAX;
      constant_offset_system += translator_constant_system.size +
                                translator_constant_system.padding_after;
    }
  }

  // Float constants.
  uint32_t constant_position_dwords_float = uint32_t(shader_object_.size());
  if (cbuffer_index_float_constants_ != kBindingIndexUnallocated) {
    assert_not_zero(constant_register_map.float_count);
    shader_object_.resize(constant_position_dwords_float +
                          sizeof(dxbc::RdefVariable) / sizeof(uint32_t));
    auto& constant_float = *reinterpret_cast<dxbc::RdefVariable*>(
        shader_object_.data() + constant_position_dwords_float);
    constant_float.name_ptr = constant_name_ptr_float;
    constant_float.size_bytes =
        sizeof(float) * 4 * constant_register_map.float_count;
    constant_float.flags = dxbc::kRdefVariableFlagUsed;
    constant_float.type_ptr =
        types_ptr + sizeof(dxbc::RdefType) *
                        uint32_t(ShaderRdefTypeIndex::kFloat4ConstantArray);
    constant_float.start_texture = UINT32_MAX;
    constant_float.start_sampler = UINT32_MAX;
  }

  // Bool and loop constants.
  uint32_t constant_position_dwords_bool_loop = uint32_t(shader_object_.size());
  if (cbuffer_index_bool_loop_constants_ != kBindingIndexUnallocated) {
    shader_object_.resize(constant_position_dwords_bool_loop +
                          sizeof(dxbc::RdefVariable) / sizeof(uint32_t) * 2);
    auto constants_bool_loop = reinterpret_cast<dxbc::RdefVariable*>(
        shader_object_.data() + constant_position_dwords_bool_loop);

    constants_bool_loop[0].name_ptr = constant_name_ptr_bool;
    constants_bool_loop[0].size_bytes = sizeof(uint32_t) * 4 * 2;
    for (size_t i = 0; i < xe::countof(constant_register_map.bool_bitmap);
         ++i) {
      if (constant_register_map.bool_bitmap[i]) {
        constants_bool_loop[0].flags |= dxbc::kRdefVariableFlagUsed;
        break;
      }
    }
    constants_bool_loop[0].type_ptr =
        types_ptr +
        sizeof(dxbc::RdefType) * uint32_t(ShaderRdefTypeIndex::kUint4Array2);
    constants_bool_loop[0].start_texture = UINT32_MAX;
    constants_bool_loop[0].start_sampler = UINT32_MAX;

    constants_bool_loop[1].name_ptr = constant_name_ptr_loop;
    constants_bool_loop[1].start_offset_bytes = sizeof(uint32_t) * 4 * 2;
    constants_bool_loop[1].size_bytes = sizeof(uint32_t) * 4 * 8;
    constants_bool_loop[1].flags =
        constant_register_map.loop_bitmap ? dxbc::kRdefVariableFlagUsed : 0;
    constants_bool_loop[1].type_ptr =
        types_ptr +
        sizeof(dxbc::RdefType) * uint32_t(ShaderRdefTypeIndex::kUint4Array8);
    constants_bool_loop[1].start_texture = UINT32_MAX;
    constants_bool_loop[1].start_sampler = UINT32_MAX;
  }

  // Fetch constants.
  uint32_t constant_position_dwords_fetch = uint32_t(shader_object_.size());
  if (cbuffer_index_fetch_constants_ != kBindingIndexUnallocated) {
    shader_object_.resize(constant_position_dwords_fetch +
                          sizeof(dxbc::RdefVariable) / sizeof(uint32_t));
    auto& constant_fetch = *reinterpret_cast<dxbc::RdefVariable*>(
        shader_object_.data() + constant_position_dwords_fetch);
    constant_fetch.name_ptr = constant_name_ptr_fetch;
    constant_fetch.size_bytes = sizeof(uint32_t) * 6 * 32;
    constant_fetch.flags = dxbc::kRdefVariableFlagUsed;
    constant_fetch.type_ptr =
        types_ptr +
        sizeof(dxbc::RdefType) * uint32_t(ShaderRdefTypeIndex::kUint4Array48);
    constant_fetch.start_texture = UINT32_MAX;
    constant_fetch.start_sampler = UINT32_MAX;
  }

  // Bindless description indices.
  uint32_t constant_position_dwords_descriptor_indices =
      uint32_t(shader_object_.size());
  if (cbuffer_index_descriptor_indices_ != kBindingIndexUnallocated) {
    assert_not_zero(GetBindlessResourceCount());
    shader_object_.resize(constant_position_dwords_descriptor_indices +
                          sizeof(dxbc::RdefVariable) / sizeof(uint32_t));
    auto& constant_descriptor_indices = *reinterpret_cast<dxbc::RdefVariable*>(
        shader_object_.data() + constant_position_dwords_descriptor_indices);
    constant_descriptor_indices.name_ptr = constant_name_ptr_descriptor_indices;
    constant_descriptor_indices.size_bytes =
        sizeof(uint32_t) * xe::align(GetBindlessResourceCount(), uint32_t(4));
    constant_descriptor_indices.flags = dxbc::kRdefVariableFlagUsed;
    constant_descriptor_indices.type_ptr =
        types_ptr +
        sizeof(dxbc::RdefType) *
            uint32_t(ShaderRdefTypeIndex::kUint4DescriptorIndexArray);
    constant_descriptor_indices.start_texture = UINT32_MAX;
    constant_descriptor_indices.start_sampler = UINT32_MAX;
  }

  // ***************************************************************************
  // Constant buffers
  // ***************************************************************************

  // Names.
  name_ptr = (uint32_t(shader_object_.size()) - blob_position_dwords) *
             sizeof(uint32_t);
  uint32_t cbuffer_name_ptr_system = name_ptr;
  if (cbuffer_index_system_constants_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_system_cbuffer");
  }
  uint32_t cbuffer_name_ptr_float = name_ptr;
  if (cbuffer_index_float_constants_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_float_cbuffer");
  }
  uint32_t cbuffer_name_ptr_bool_loop = name_ptr;
  if (cbuffer_index_bool_loop_constants_ != kBindingIndexUnallocated) {
    name_ptr +=
        dxbc::AppendAlignedString(shader_object_, "xe_bool_loop_cbuffer");
  }
  uint32_t cbuffer_name_ptr_fetch = name_ptr;
  if (cbuffer_index_fetch_constants_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_fetch_cbuffer");
  }
  uint32_t cbuffer_name_ptr_descriptor_indices = name_ptr;
  if (cbuffer_index_descriptor_indices_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_,
                                          "xe_descriptor_indices_cbuffer");
  }

  // All the constant buffers, sorted by their binding index.
  uint32_t cbuffers_position_dwords = uint32_t(shader_object_.size());
  shader_object_.resize(cbuffers_position_dwords + sizeof(dxbc::RdefCbuffer) /
                                                       sizeof(uint32_t) *
                                                       cbuffer_count_);
  {
    auto cbuffers = reinterpret_cast<dxbc::RdefCbuffer*>(
        shader_object_.data() + cbuffers_position_dwords);
    for (uint32_t i = 0; i < cbuffer_count_; ++i) {
      dxbc::RdefCbuffer& cbuffer = cbuffers[i];
      cbuffer.type = dxbc::RdefCbufferType::kCbuffer;
      if (i == cbuffer_index_system_constants_) {
        cbuffer.name_ptr = cbuffer_name_ptr_system;
        cbuffer.variable_count = uint32_t(SystemConstants::Index::kCount);
        cbuffer.variables_ptr =
            (constant_position_dwords_system - blob_position_dwords) *
            sizeof(uint32_t);
        cbuffer.size_vector_aligned_bytes =
            uint32_t(xe::align(sizeof(SystemConstants), sizeof(uint32_t) * 4));
      } else if (i == cbuffer_index_float_constants_) {
        assert_not_zero(constant_register_map.float_count);
        cbuffer.name_ptr = cbuffer_name_ptr_float;
        cbuffer.variable_count = 1;
        cbuffer.variables_ptr =
            (constant_position_dwords_float - blob_position_dwords) *
            sizeof(uint32_t);
        cbuffer.size_vector_aligned_bytes =
            sizeof(float) * 4 * constant_register_map.float_count;
      } else if (i == cbuffer_index_bool_loop_constants_) {
        cbuffer.name_ptr = cbuffer_name_ptr_bool_loop;
        cbuffer.variable_count = 2;
        cbuffer.variables_ptr =
            (constant_position_dwords_bool_loop - blob_position_dwords) *
            sizeof(uint32_t);
        cbuffer.size_vector_aligned_bytes = sizeof(uint32_t) * 4 * (2 + 8);
      } else if (i == cbuffer_index_fetch_constants_) {
        cbuffer.name_ptr = cbuffer_name_ptr_fetch;
        cbuffer.variable_count = 1;
        cbuffer.variables_ptr =
            (constant_position_dwords_fetch - blob_position_dwords) *
            sizeof(uint32_t);
        cbuffer.size_vector_aligned_bytes = sizeof(uint32_t) * 6 * 32;
      } else if (i == cbuffer_index_descriptor_indices_) {
        assert_not_zero(GetBindlessResourceCount());
        cbuffer.name_ptr = cbuffer_name_ptr_descriptor_indices;
        cbuffer.variable_count = 1;
        cbuffer.variables_ptr = (constant_position_dwords_descriptor_indices -
                                 blob_position_dwords) *
                                sizeof(uint32_t);
        cbuffer.size_vector_aligned_bytes =
            sizeof(uint32_t) *
            xe::align(GetBindlessResourceCount(), uint32_t(4));
      } else {
        assert_unhandled_case(i);
      }
    }
  }

  // ***************************************************************************
  // Bindings, in s#, t#, u#, cb# order
  // ***************************************************************************

  // Names, except for constant buffers because their names are written already.
  name_ptr = (uint32_t(shader_object_.size()) - blob_position_dwords) *
             sizeof(uint32_t);
  uint32_t sampler_name_ptr = name_ptr;
  if (!sampler_bindings_.empty()) {
    if (bindless_resources_used_) {
      name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_samplers");
    } else {
      for (uint32_t i = 0; i < uint32_t(sampler_bindings_.size()); ++i) {
        name_ptr += dxbc::AppendAlignedString(
            shader_object_, sampler_bindings_[i].name.c_str());
      }
    }
  }
  uint32_t shared_memory_srv_name_ptr = name_ptr;
  if (srv_index_shared_memory_ != kBindingIndexUnallocated) {
    name_ptr +=
        dxbc::AppendAlignedString(shader_object_, "xe_shared_memory_srv");
  }
  uint32_t bindless_textures_2d_name_ptr = name_ptr;
  uint32_t bindless_textures_3d_name_ptr = name_ptr;
  uint32_t bindless_textures_cube_name_ptr = name_ptr;
  if (bindless_resources_used_) {
    if (srv_index_bindless_textures_2d_ != kBindingIndexUnallocated) {
      bindless_textures_2d_name_ptr = name_ptr;
      name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_textures_2d");
    }
    if (srv_index_bindless_textures_3d_ != kBindingIndexUnallocated) {
      bindless_textures_3d_name_ptr = name_ptr;
      name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_textures_3d");
    }
    if (srv_index_bindless_textures_cube_ != kBindingIndexUnallocated) {
      bindless_textures_cube_name_ptr = name_ptr;
      name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_textures_cube");
    }
  } else {
    for (TextureBinding& texture_binding : texture_bindings_) {
      texture_binding.bindful_srv_rdef_name_ptr = name_ptr;
      name_ptr += dxbc::AppendAlignedString(shader_object_,
                                            texture_binding.name.c_str());
    }
  }
  uint32_t shared_memory_uav_name_ptr = name_ptr;
  if (uav_index_shared_memory_ != kBindingIndexUnallocated) {
    name_ptr +=
        dxbc::AppendAlignedString(shader_object_, "xe_shared_memory_uav");
  }
  uint32_t edram_name_ptr = name_ptr;
  if (uav_index_edram_ != kBindingIndexUnallocated) {
    name_ptr += dxbc::AppendAlignedString(shader_object_, "xe_edram");
  }

  uint32_t bindings_position_dwords = uint32_t(shader_object_.size());

  // Samplers.
  if (!sampler_bindings_.empty()) {
    uint32_t samplers_position_dwords = uint32_t(shader_object_.size());
    shader_object_.resize(
        samplers_position_dwords +
        sizeof(dxbc::RdefInputBind) / sizeof(uint32_t) *
            (bindless_resources_used_ ? 1 : sampler_bindings_.size()));
    auto samplers = reinterpret_cast<dxbc::RdefInputBind*>(
        shader_object_.data() + samplers_position_dwords);
    if (bindless_resources_used_) {
      // Bindless sampler heap.
      samplers[0].name_ptr = sampler_name_ptr;
      samplers[0].type = dxbc::RdefInputType::kSampler;
    } else {
      // Bindful samplers.
      uint32_t sampler_current_name_ptr = sampler_name_ptr;
      for (size_t i = 0; i < sampler_bindings_.size(); ++i) {
        dxbc::RdefInputBind& sampler = samplers[i];
        sampler.name_ptr = sampler_current_name_ptr;
        sampler.type = dxbc::RdefInputType::kSampler;
        sampler.bind_point = uint32_t(i);
        sampler.bind_count = 1;
        sampler.id = uint32_t(i);
        sampler_current_name_ptr +=
            dxbc::GetAlignedStringLength(sampler_bindings_[i].name.c_str());
      }
    }
  }

  // Shader resource views, sorted by binding index.
  uint32_t srvs_position_dwords = uint32_t(shader_object_.size());
  shader_object_.resize(srvs_position_dwords + sizeof(dxbc::RdefInputBind) /
                                                   sizeof(uint32_t) *
                                                   srv_count_);
  {
    auto srvs = reinterpret_cast<dxbc::RdefInputBind*>(shader_object_.data() +
                                                       srvs_position_dwords);
    for (uint32_t i = 0; i < srv_count_; ++i) {
      dxbc::RdefInputBind& srv = srvs[i];
      srv.id = i;
      if (i == srv_index_shared_memory_) {
        // Shared memory (when memexport isn't used in the pipeline).
        srv.name_ptr = shared_memory_srv_name_ptr;
        srv.type = dxbc::RdefInputType::kByteAddress;
        srv.return_type = dxbc::ResourceReturnType::kMixed;
        srv.dimension = dxbc::RdefDimension::kSRVBuffer;
        srv.bind_point = uint32_t(SRVMainRegister::kSharedMemory);
        srv.bind_count = 1;
        srv.bind_point_space = uint32_t(SRVSpace::kMain);
      } else {
        // Bindful texture or bindless textures.
        srv.type = dxbc::RdefInputType::kTexture;
        srv.return_type = dxbc::ResourceReturnType::kFloat;
        srv.sample_count = UINT32_MAX;
        srv.flags = dxbc::kRdefInputFlags4Component;
        if (bindless_resources_used_) {
          // Bindless texture heap.
          if (i == srv_index_bindless_textures_3d_) {
            srv.name_ptr = bindless_textures_3d_name_ptr;
            srv.dimension = dxbc::RdefDimension::kSRVTexture3D;
            srv.bind_point_space = uint32_t(SRVSpace::kBindlessTextures3D);
          } else if (i == srv_index_bindless_textures_cube_) {
            srv.name_ptr = bindless_textures_cube_name_ptr;
            srv.dimension = dxbc::RdefDimension::kSRVTextureCube;
            srv.bind_point_space = uint32_t(SRVSpace::kBindlessTexturesCube);
          } else {
            assert_true(i == srv_index_bindless_textures_2d_);
            srv.name_ptr = bindless_textures_2d_name_ptr;
            srv.dimension = dxbc::RdefDimension::kSRVTexture2DArray;
            srv.bind_point_space = uint32_t(SRVSpace::kBindlessTextures2DArray);
          }
        } else {
          // Bindful texture.
          auto it = texture_bindings_for_bindful_srv_indices_.find(i);
          assert_true(it != texture_bindings_for_bindful_srv_indices_.end());
          uint32_t texture_binding_index = it->second;
          const TextureBinding& texture_binding =
              texture_bindings_[texture_binding_index];
          srv.name_ptr = texture_binding.bindful_srv_rdef_name_ptr;
          switch (texture_binding.dimension) {
            case xenos::FetchOpDimension::k3DOrStacked:
              srv.dimension = dxbc::RdefDimension::kSRVTexture3D;
              break;
            case xenos::FetchOpDimension::kCube:
              srv.dimension = dxbc::RdefDimension::kSRVTextureCube;
              break;
            default:
              assert_true(texture_binding.dimension ==
                          xenos::FetchOpDimension::k2D);
              srv.dimension = dxbc::RdefDimension::kSRVTexture2DArray;
          }
          srv.bind_point = uint32_t(SRVMainRegister::kBindfulTexturesStart) +
                           texture_binding_index;
          srv.bind_count = 1;
          srv.bind_point_space = uint32_t(SRVSpace::kMain);
        }
      }
    }
  }

  // Unordered access views, sorted by binding index.
  uint32_t uavs_position_dwords = uint32_t(shader_object_.size());
  shader_object_.resize(uavs_position_dwords + sizeof(dxbc::RdefInputBind) /
                                                   sizeof(uint32_t) *
                                                   uav_count_);
  {
    auto uavs = reinterpret_cast<dxbc::RdefInputBind*>(shader_object_.data() +
                                                       uavs_position_dwords);
    for (uint32_t i = 0; i < uav_count_; ++i) {
      dxbc::RdefInputBind& uav = uavs[i];
      uav.bind_count = 1;
      uav.id = i;
      if (i == uav_index_shared_memory_) {
        // Shared memory (when memexport is used in the pipeline).
        uav.name_ptr = shared_memory_uav_name_ptr;
        uav.type = dxbc::RdefInputType::kUAVRWByteAddress;
        uav.return_type = dxbc::ResourceReturnType::kMixed;
        uav.dimension = dxbc::RdefDimension::kUAVBuffer;
        uav.bind_point = uint32_t(UAVRegister::kSharedMemory);
      } else if (i == uav_index_edram_) {
        // EDRAM R32_UINT buffer.
        uav.name_ptr = edram_name_ptr;
        uav.type = dxbc::RdefInputType::kUAVRWTyped;
        uav.return_type = dxbc::ResourceReturnType::kUInt;
        uav.dimension = dxbc::RdefDimension::kUAVBuffer;
        uav.sample_count = UINT32_MAX;
        uav.bind_point = uint32_t(UAVRegister::kEdram);
      } else {
        assert_unhandled_case(i);
      }
    }
  }

  // Constant buffers.
  uint32_t cbuffer_binding_position_dwords = uint32_t(shader_object_.size());
  shader_object_.resize(cbuffer_binding_position_dwords +
                        sizeof(dxbc::RdefInputBind) / sizeof(uint32_t) *
                            cbuffer_count_);
  {
    auto cbuffers = reinterpret_cast<dxbc::RdefInputBind*>(
        shader_object_.data() + cbuffer_binding_position_dwords);
    for (uint32_t i = 0; i < cbuffer_count_; ++i) {
      dxbc::RdefInputBind& cbuffer = cbuffers[i];
      cbuffer.type = dxbc::RdefInputType::kCbuffer;
      cbuffer.bind_count = 1;
      // Like `cbuffer`, don't need `ConstantBuffer<T>` properties.
      cbuffer.flags = dxbc::kRdefInputFlagUserPacked;
      cbuffer.id = i;
      if (i == cbuffer_index_system_constants_) {
        cbuffer.name_ptr = cbuffer_name_ptr_system;
        cbuffer.bind_point = uint32_t(CbufferRegister::kSystemConstants);
      } else if (i == cbuffer_index_float_constants_) {
        cbuffer.name_ptr = cbuffer_name_ptr_float;
        cbuffer.bind_point = uint32_t(CbufferRegister::kFloatConstants);
      } else if (i == cbuffer_index_bool_loop_constants_) {
        cbuffer.name_ptr = cbuffer_name_ptr_bool_loop;
        cbuffer.bind_point = uint32_t(CbufferRegister::kBoolLoopConstants);
      } else if (i == cbuffer_index_fetch_constants_) {
        cbuffer.name_ptr = cbuffer_name_ptr_fetch;
        cbuffer.bind_point = uint32_t(CbufferRegister::kFetchConstants);
      } else if (i == cbuffer_index_descriptor_indices_) {
        cbuffer.name_ptr = cbuffer_name_ptr_descriptor_indices;
        cbuffer.bind_point = uint32_t(CbufferRegister::kDescriptorIndices);
      } else {
        assert_unhandled_case(i);
      }
    }
  }

  uint32_t bindings_end_position_dwords = uint32_t(shader_object_.size());

  // ***************************************************************************
  // Header
  // ***************************************************************************

  {
    auto& header = *reinterpret_cast<dxbc::RdefHeader*>(shader_object_.data() +
                                                        blob_position_dwords);
    header.cbuffer_count = cbuffer_count_;
    header.cbuffers_ptr =
        (cbuffers_position_dwords - blob_position_dwords) * sizeof(uint32_t);
    header.input_bind_count =
        (bindings_end_position_dwords - bindings_position_dwords) *
        sizeof(uint32_t) / sizeof(dxbc::RdefInputBind);
    header.input_binds_ptr =
        (bindings_position_dwords - blob_position_dwords) * sizeof(uint32_t);
    if (IsDxbcVertexShader()) {
      header.shader_model = dxbc::RdefShaderModel::kVertexShader5_1;
    } else if (IsDxbcDomainShader()) {
      header.shader_model = dxbc::RdefShaderModel::kDomainShader5_1;
    } else {
      assert_true(is_pixel_shader());
      header.shader_model = dxbc::RdefShaderModel::kPixelShader5_1;
    }
    header.compile_flags = dxbc::kCompileFlagNoPreshader |
                           dxbc::kCompileFlagPreferFlowControl |
                           dxbc::kCompileFlagIeeeStrictness;
    if (bindless_resources_used_) {
      header.compile_flags |= dxbc::kCompileFlagEnableUnboundedDescriptorTables;
    }
    // Generator name placed directly after the header.
    header.generator_name_ptr = sizeof(dxbc::RdefHeader);
    header.fourcc = dxbc::RdefHeader::FourCC::k5_1;
    header.InitializeSizes();
  }
}

void DxbcShaderTranslator::WriteInputSignature() {
  // Because of shader_object_.resize(), pointers can't be kept persistently
  // here! Resizing also zeroes the memory.
  uint32_t blob_position = uint32_t(shader_object_.size());
  // Reserve space for the header.
  shader_object_.resize(shader_object_.size() +
                        sizeof(dxbc::Signature) / sizeof(uint32_t));
  uint32_t parameter_count = 0;
  constexpr size_t kParameterDwords =
      sizeof(dxbc::SignatureParameter) / sizeof(uint32_t);

  if (IsDxbcVertexShader()) {
    // Unswapped vertex index (SV_VertexID).
    size_t vertex_id_position = shader_object_.size();
    shader_object_.resize(shader_object_.size() + kParameterDwords);
    ++parameter_count;
    {
      auto& vertex_id = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + vertex_id_position);
      vertex_id.system_value = dxbc::Name::kVertexID;
      vertex_id.component_type = dxbc::SignatureRegisterComponentType::kUInt32;
      vertex_id.register_index = uint32_t(InOutRegister::kVSInVertexIndex);
      vertex_id.mask = 0b0001;
      vertex_id.always_reads_mask = (register_count() >= 1) ? 0b0001 : 0b0000;
    }

    // Semantic names.
    uint32_t semantic_offset =
        uint32_t((shader_object_.size() - blob_position) * sizeof(uint32_t));
    {
      auto& vertex_id = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + vertex_id_position);
      vertex_id.semantic_name_ptr = semantic_offset;
    }
    semantic_offset += dxbc::AppendAlignedString(shader_object_, "SV_VertexID");
  } else if (IsDxbcDomainShader()) {
    // Control point indices, byte-swapped, biased according to the base index
    // and converted to float by the host vertex and hull shaders
    // (XEVERTEXID). Needed even for patch-indexed tessellation modes because
    // hull and domain shaders have strict linkage requirements, all hull shader
    // outputs must be declared in a domain shader, and the same hull shaders
    // are used for control-point-indexed and patch-indexed tessellation modes.
    size_t control_point_index_position = shader_object_.size();
    shader_object_.resize(shader_object_.size() + kParameterDwords);
    ++parameter_count;
    {
      auto& control_point_index = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + control_point_index_position);
      control_point_index.component_type =
          dxbc::SignatureRegisterComponentType::kFloat32;
      control_point_index.register_index =
          uint32_t(InOutRegister::kDSInControlPointIndex);
      control_point_index.mask = 0b0001;
      control_point_index.always_reads_mask =
          in_control_point_index_used_ ? 0b0001 : 0b0000;
    }

    // Semantic names.
    uint32_t semantic_offset =
        uint32_t((shader_object_.size() - blob_position) * sizeof(uint32_t));
    {
      auto& control_point_index = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + control_point_index_position);
      control_point_index.semantic_name_ptr = semantic_offset;
    }
    semantic_offset += dxbc::AppendAlignedString(shader_object_, "XEVERTEXID");
  } else if (is_pixel_shader()) {
    // Written dynamically, so assume it's always used if it can be written to
    // any interpolator register.
    bool param_gen_used = !is_depth_only_pixel_shader_ && register_count() != 0;

    // Intepolators (TEXCOORD#).
    size_t interpolator_position = shader_object_.size();
    shader_object_.resize(shader_object_.size() +
                          xenos::kMaxInterpolators * kParameterDwords);
    parameter_count += xenos::kMaxInterpolators;
    {
      auto interpolators = reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + interpolator_position);
      for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
        dxbc::SignatureParameter& interpolator = interpolators[i];
        interpolator.semantic_index = i;
        interpolator.component_type =
            dxbc::SignatureRegisterComponentType::kFloat32;
        interpolator.register_index =
            uint32_t(InOutRegister::kPSInInterpolators) + i;
        interpolator.mask = 0b1111;
        // Interpolators are copied to GPRs in the beginning of the shader. If
        // there's a register to copy to, this interpolator is used.
        interpolator.always_reads_mask =
            (!is_depth_only_pixel_shader_ && i < register_count()) ? 0b1111
                                                                   : 0b0000;
      }
    }

    // Point parameters for ps_param_gen - coordinate on the point and point
    // size as a float3 TEXCOORD (but the size in Z is not needed).
    size_t point_parameters_position = shader_object_.size();
    shader_object_.resize(shader_object_.size() + kParameterDwords);
    ++parameter_count;
    {
      auto& point_parameters = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + point_parameters_position);
      point_parameters.semantic_index = kPointParametersTexCoord;
      point_parameters.component_type =
          dxbc::SignatureRegisterComponentType::kFloat32;
      point_parameters.register_index =
          uint32_t(InOutRegister::kPSInPointParameters);
      point_parameters.mask = 0b0111;
      point_parameters.always_reads_mask = param_gen_used ? 0b0011 : 0b0000;
    }

    // Pixel position (SV_Position).
    size_t position_position = shader_object_.size();
    shader_object_.resize(shader_object_.size() + kParameterDwords);
    ++parameter_count;
    {
      auto& position = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + position_position);
      position.system_value = dxbc::Name::kPosition;
      position.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
      position.register_index = uint32_t(InOutRegister::kPSInPosition);
      position.mask = 0b1111;
      position.always_reads_mask = in_position_used_;
    }

    // Is front face (SV_IsFrontFace).
    size_t is_front_face_position = shader_object_.size();
    shader_object_.resize(shader_object_.size() + kParameterDwords);
    ++parameter_count;
    {
      auto& is_front_face = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + is_front_face_position);
      is_front_face.system_value = dxbc::Name::kIsFrontFace;
      is_front_face.component_type =
          dxbc::SignatureRegisterComponentType::kUInt32;
      is_front_face.register_index =
          uint32_t(InOutRegister::kPSInFrontFaceAndSampleIndex);
      is_front_face.mask = 0b0001;
      is_front_face.always_reads_mask = in_front_face_used_ ? 0b0001 : 0b0000;
    }

    // Sample index (SV_SampleIndex) for safe memexport with sample-rate
    // shading.
    size_t sample_index_position = SIZE_MAX;
    if (current_shader().is_valid_memexport_used() && IsSampleRate()) {
      size_t sample_index_position = shader_object_.size();
      shader_object_.resize(shader_object_.size() + kParameterDwords);
      ++parameter_count;
      {
        auto& sample_index = *reinterpret_cast<dxbc::SignatureParameter*>(
            shader_object_.data() + sample_index_position);
        sample_index.system_value = dxbc::Name::kSampleIndex;
        sample_index.component_type =
            dxbc::SignatureRegisterComponentType::kUInt32;
        sample_index.register_index =
            uint32_t(InOutRegister::kPSInFrontFaceAndSampleIndex);
        sample_index.mask = 0b0010;
        sample_index.always_reads_mask = 0b0010;
      }
    }

    // Semantic names.
    uint32_t semantic_offset =
        uint32_t((shader_object_.size() - blob_position) * sizeof(uint32_t));
    {
      auto interpolators = reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + interpolator_position);
      for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
        interpolators[i].semantic_name_ptr = semantic_offset;
      }
      auto& point_parameters = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + point_parameters_position);
      point_parameters.semantic_name_ptr = semantic_offset;
    }
    semantic_offset += dxbc::AppendAlignedString(shader_object_, "TEXCOORD");
    {
      auto& position = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + position_position);
      position.semantic_name_ptr = semantic_offset;
    }
    semantic_offset += dxbc::AppendAlignedString(shader_object_, "SV_Position");
    {
      auto& is_front_face = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + is_front_face_position);
      is_front_face.semantic_name_ptr = semantic_offset;
    }
    semantic_offset +=
        dxbc::AppendAlignedString(shader_object_, "SV_IsFrontFace");
    if (sample_index_position != SIZE_MAX) {
      {
        auto& sample_index = *reinterpret_cast<dxbc::SignatureParameter*>(
            shader_object_.data() + sample_index_position);
        sample_index.semantic_name_ptr = semantic_offset;
      }
      semantic_offset +=
          dxbc::AppendAlignedString(shader_object_, "SV_SampleIndex");
    }
  }

  // Header.
  {
    auto& header = *reinterpret_cast<dxbc::Signature*>(shader_object_.data() +
                                                       blob_position);
    header.parameter_count = parameter_count;
    header.parameter_info_ptr = sizeof(dxbc::Signature);
  }
}

void DxbcShaderTranslator::WritePatchConstantSignature() {
  assert_true(IsDxbcDomainShader());
  // Because of shader_object_.resize(), pointers can't be kept persistently
  // here! Resizing also zeroes the memory.
  uint32_t blob_position = uint32_t(shader_object_.size());
  // Reserve space for the header.
  shader_object_.resize(shader_object_.size() +
                        sizeof(dxbc::Signature) / sizeof(uint32_t));
  uint32_t parameter_count = 0;
  constexpr size_t kParameterDwords =
      sizeof(dxbc::SignatureParameter) / sizeof(uint32_t);

  // FXC always compiles with SV_TessFactor and SV_InsideTessFactor input, so
  // this is required even if not referenced (HS and DS have very strict
  // linkage, by the way, everything that HS outputs must be listed in DS
  // inputs).
  uint32_t tess_factor_edge_count = 0;
  dxbc::Name tess_factor_edge_system_value = dxbc::Name::kUndefined;
  uint32_t tess_factor_inside_count = 0;
  dxbc::Name tess_factor_inside_system_value = dxbc::Name::kUndefined;
  Shader::HostVertexShaderType host_vertex_shader_type =
      GetDxbcShaderModification().vertex.host_vertex_shader_type;
  switch (host_vertex_shader_type) {
    case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
    case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
      tess_factor_edge_count = 3;
      tess_factor_edge_system_value = dxbc::Name::kFinalTriEdgeTessFactor;
      tess_factor_inside_count = 1;
      tess_factor_inside_system_value = dxbc::Name::kFinalTriInsideTessFactor;
      break;
    case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
    case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
      tess_factor_edge_count = 4;
      tess_factor_edge_system_value = dxbc::Name::kFinalQuadEdgeTessFactor;
      tess_factor_inside_count = 2;
      tess_factor_inside_system_value = dxbc::Name::kFinalQuadInsideTessFactor;
      break;
    default:
      // TODO(Triang3l): Support line patches.
      assert_unhandled_case(host_vertex_shader_type);
      EmitTranslationError(
          "Unsupported host vertex shader type in WritePatchConstantSignature");
  }

  // Edge tessellation factors (SV_TessFactor).
  size_t tess_factor_edge_position = shader_object_.size();
  shader_object_.resize(shader_object_.size() +
                        tess_factor_edge_count * kParameterDwords);
  parameter_count += tess_factor_edge_count;
  {
    auto tess_factors_edge = reinterpret_cast<dxbc::SignatureParameter*>(
        shader_object_.data() + tess_factor_edge_position);
    for (uint32_t i = 0; i < tess_factor_edge_count; ++i) {
      dxbc::SignatureParameter& tess_factor_edge = tess_factors_edge[i];
      tess_factor_edge.semantic_index = i;
      tess_factor_edge.system_value = tess_factor_edge_system_value;
      tess_factor_edge.component_type =
          dxbc::SignatureRegisterComponentType::kFloat32;
      // Not using any of these, just assigning consecutive registers.
      tess_factor_edge.register_index = i;
      tess_factor_edge.mask = 0b0001;
    }
  }

  // Inside tessellation factors (SV_InsideTessFactor).
  size_t tess_factor_inside_position = shader_object_.size();
  shader_object_.resize(shader_object_.size() +
                        tess_factor_inside_count * kParameterDwords);
  parameter_count += tess_factor_inside_count;
  {
    auto tess_factors_inside = reinterpret_cast<dxbc::SignatureParameter*>(
        shader_object_.data() + tess_factor_inside_position);
    for (uint32_t i = 0; i < tess_factor_inside_count; ++i) {
      dxbc::SignatureParameter& tess_factor_inside = tess_factors_inside[i];
      tess_factor_inside.semantic_index = i;
      tess_factor_inside.system_value = tess_factor_inside_system_value;
      tess_factor_inside.component_type =
          dxbc::SignatureRegisterComponentType::kFloat32;
      // Not using any of these, just assigning consecutive registers.
      tess_factor_inside.register_index = tess_factor_edge_count + i;
      tess_factor_inside.mask = 0b0001;
    }
  }

  // Semantic names.
  uint32_t semantic_offset =
      uint32_t((shader_object_.size() - blob_position) * sizeof(uint32_t));
  {
    auto tess_factors_edge = reinterpret_cast<dxbc::SignatureParameter*>(
        shader_object_.data() + tess_factor_edge_position);
    for (uint32_t i = 0; i < tess_factor_edge_count; ++i) {
      tess_factors_edge[i].semantic_name_ptr = semantic_offset;
    }
  }
  semantic_offset += dxbc::AppendAlignedString(shader_object_, "SV_TessFactor");
  {
    auto tess_factors_inside = reinterpret_cast<dxbc::SignatureParameter*>(
        shader_object_.data() + tess_factor_inside_position);
    for (uint32_t i = 0; i < tess_factor_inside_count; ++i) {
      tess_factors_inside[i].semantic_name_ptr = semantic_offset;
    }
  }
  semantic_offset +=
      dxbc::AppendAlignedString(shader_object_, "SV_InsideTessFactor");

  // Header.
  {
    auto& header = *reinterpret_cast<dxbc::Signature*>(shader_object_.data() +
                                                       blob_position);
    header.parameter_count = parameter_count;
    header.parameter_info_ptr = sizeof(dxbc::Signature);
  }
}

void DxbcShaderTranslator::WriteOutputSignature() {
  // Because of shader_object_.resize(), pointers can't be kept persistently
  // here! Resizing also zeroes the memory.
  uint32_t blob_position = uint32_t(shader_object_.size());
  // Reserve space for the header.
  shader_object_.resize(shader_object_.size() +
                        sizeof(dxbc::Signature) / sizeof(uint32_t));
  uint32_t parameter_count = 0;
  constexpr size_t kParameterDwords =
      sizeof(dxbc::SignatureParameter) / sizeof(uint32_t);

  if (is_vertex_shader()) {
    // Intepolators (TEXCOORD#).
    size_t interpolator_position = shader_object_.size();
    shader_object_.resize(shader_object_.size() +
                          xenos::kMaxInterpolators * kParameterDwords);
    parameter_count += xenos::kMaxInterpolators;
    {
      auto interpolators = reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + interpolator_position);
      for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
        dxbc::SignatureParameter& interpolator = interpolators[i];
        interpolator.semantic_index = i;
        interpolator.component_type =
            dxbc::SignatureRegisterComponentType::kFloat32;
        interpolator.register_index =
            uint32_t(InOutRegister::kVSDSOutInterpolators) + i;
        interpolator.mask = 0b1111;
      }
    }

    // Point parameters - coordinate on the point and point size as a float3
    // TEXCOORD. Always used because reset to (0, 0, -1).
    size_t point_parameters_position = shader_object_.size();
    shader_object_.resize(shader_object_.size() + kParameterDwords);
    ++parameter_count;
    {
      auto& point_parameters = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + point_parameters_position);
      point_parameters.semantic_index = kPointParametersTexCoord;
      point_parameters.component_type =
          dxbc::SignatureRegisterComponentType::kFloat32;
      point_parameters.register_index =
          uint32_t(InOutRegister::kVSDSOutPointParameters);
      point_parameters.mask = 0b0111;
      point_parameters.never_writes_mask = 0b1000;
    }

    // Position (SV_Position).
    size_t position_position = shader_object_.size();
    shader_object_.resize(shader_object_.size() + kParameterDwords);
    ++parameter_count;
    {
      auto& position = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + position_position);
      position.system_value = dxbc::Name::kPosition;
      position.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
      position.register_index = uint32_t(InOutRegister::kVSDSOutPosition);
      position.mask = 0b1111;
    }

    // Clip (SV_ClipDistance) and cull (SV_CullDistance) distances.
    size_t clip_distance_0123_position = shader_object_.size();
    shader_object_.resize(shader_object_.size() + kParameterDwords);
    ++parameter_count;
    {
      auto& clip_distance_0123 = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + clip_distance_0123_position);
      clip_distance_0123.system_value = dxbc::Name::kClipDistance;
      clip_distance_0123.component_type =
          dxbc::SignatureRegisterComponentType::kFloat32;
      clip_distance_0123.register_index =
          uint32_t(InOutRegister::kVSDSOutClipDistance0123);
      clip_distance_0123.mask = 0b1111;
    }
    size_t clip_distance_45_position = shader_object_.size();
    shader_object_.resize(shader_object_.size() + kParameterDwords);
    ++parameter_count;
    {
      auto& clip_distance_45 = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + clip_distance_45_position);
      clip_distance_45.semantic_index = 1;
      clip_distance_45.system_value = dxbc::Name::kClipDistance;
      clip_distance_45.component_type =
          dxbc::SignatureRegisterComponentType::kFloat32;
      clip_distance_45.register_index =
          uint32_t(InOutRegister::kVSDSOutClipDistance45AndCullDistance);
      clip_distance_45.mask = 0b0011;
      clip_distance_45.never_writes_mask = 0b1100;
    }
    size_t cull_distance_position = shader_object_.size();
    shader_object_.resize(shader_object_.size() + kParameterDwords);
    ++parameter_count;
    {
      auto& cull_distance = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + cull_distance_position);
      cull_distance.system_value = dxbc::Name::kCullDistance;
      cull_distance.component_type =
          dxbc::SignatureRegisterComponentType::kFloat32;
      cull_distance.register_index =
          uint32_t(InOutRegister::kVSDSOutClipDistance45AndCullDistance);
      cull_distance.mask = 0b0100;
      cull_distance.never_writes_mask = 0b1011;
    }

    // Semantic names.
    uint32_t semantic_offset =
        uint32_t((shader_object_.size() - blob_position) * sizeof(uint32_t));
    {
      auto interpolators = reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + interpolator_position);
      for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
        interpolators[i].semantic_name_ptr = semantic_offset;
      }
      auto& point_parameters = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + point_parameters_position);
      point_parameters.semantic_name_ptr = semantic_offset;
    }
    semantic_offset += dxbc::AppendAlignedString(shader_object_, "TEXCOORD");
    {
      auto& position = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + position_position);
      position.semantic_name_ptr = semantic_offset;
    }
    semantic_offset += dxbc::AppendAlignedString(shader_object_, "SV_Position");
    {
      auto& clip_distance_0123 = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + clip_distance_0123_position);
      clip_distance_0123.semantic_name_ptr = semantic_offset;
      auto& clip_distance_45 = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + clip_distance_45_position);
      clip_distance_45.semantic_name_ptr = semantic_offset;
    }
    semantic_offset +=
        dxbc::AppendAlignedString(shader_object_, "SV_ClipDistance");
    {
      auto& cull_distance = *reinterpret_cast<dxbc::SignatureParameter*>(
          shader_object_.data() + cull_distance_position);
      cull_distance.semantic_name_ptr = semantic_offset;
    }
    semantic_offset +=
        dxbc::AppendAlignedString(shader_object_, "SV_CullDistance");
  } else if (is_pixel_shader()) {
    if (!edram_rov_used_) {
      uint32_t color_targets_written = current_shader().writes_color_targets();

      // Color render targets (SV_Target#).
      size_t target_position = SIZE_MAX;
      uint32_t color_targets_written_count =
          xe::bit_count(color_targets_written);
      if (color_targets_written) {
        target_position = shader_object_.size();
        shader_object_.resize(shader_object_.size() +
                              color_targets_written_count * kParameterDwords);
        parameter_count += color_targets_written_count;
        auto targets = reinterpret_cast<dxbc::SignatureParameter*>(
            shader_object_.data() + target_position);
        uint32_t target_index = 0;
        for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
          if (!(color_targets_written & (uint32_t(1) << i))) {
            continue;
          }
          dxbc::SignatureParameter& target = targets[target_index++];
          target.semantic_index = i;
          target.component_type =
              dxbc::SignatureRegisterComponentType::kFloat32;
          target.register_index = i;
          target.mask = 0b1111;
        }
      }

      // Coverage output for alpha to mask (SV_Coverage).
      size_t coverage_position = SIZE_MAX;
      if (color_targets_written & 0b1) {
        coverage_position = shader_object_.size();
        shader_object_.resize(shader_object_.size() + kParameterDwords);
        ++parameter_count;
        auto& coverage = *reinterpret_cast<dxbc::SignatureParameter*>(
            shader_object_.data() + coverage_position);
        coverage.component_type = dxbc::SignatureRegisterComponentType::kUInt32;
        coverage.register_index = UINT32_MAX;
        coverage.mask = 0b0001;
        coverage.never_writes_mask = 0b1110;
      }

      // Depth (SV_Depth or SV_DepthLessEqual).
      Modification::DepthStencilMode depth_stencil_mode =
          GetDxbcShaderModification().pixel.depth_stencil_mode;
      size_t depth_position = SIZE_MAX;
      if (current_shader().writes_depth() || DSV_IsWritingFloat24Depth()) {
        depth_position = shader_object_.size();
        shader_object_.resize(shader_object_.size() + kParameterDwords);
        ++parameter_count;
        auto& depth = *reinterpret_cast<dxbc::SignatureParameter*>(
            shader_object_.data() + depth_position);
        depth.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
        depth.register_index = UINT32_MAX;
        depth.mask = 0b0001;
        depth.never_writes_mask = 0b1110;
      }

      // Semantic names.
      uint32_t semantic_offset =
          uint32_t((shader_object_.size() - blob_position) * sizeof(uint32_t));
      if (target_position != SIZE_MAX) {
        {
          auto targets = reinterpret_cast<dxbc::SignatureParameter*>(
              shader_object_.data() + target_position);
          for (uint32_t i = 0; i < color_targets_written_count; ++i) {
            targets[i].semantic_name_ptr = semantic_offset;
          }
        }
        semantic_offset +=
            dxbc::AppendAlignedString(shader_object_, "SV_Target");
      }
      if (coverage_position != SIZE_MAX) {
        {
          auto& coverage = *reinterpret_cast<dxbc::SignatureParameter*>(
              shader_object_.data() + coverage_position);
          coverage.semantic_name_ptr = semantic_offset;
        }
        semantic_offset +=
            dxbc::AppendAlignedString(shader_object_, "SV_Coverage");
      }
      if (depth_position != SIZE_MAX) {
        {
          auto& depth = *reinterpret_cast<dxbc::SignatureParameter*>(
              shader_object_.data() + depth_position);
          depth.semantic_name_ptr = semantic_offset;
        }
        const char* depth_semantic_name;
        if (!current_shader().writes_depth() &&
            GetDxbcShaderModification().pixel.depth_stencil_mode ==
                Modification::DepthStencilMode::kFloat24Truncating) {
          depth_semantic_name = "SV_DepthLessEqual";
        } else {
          depth_semantic_name = "SV_Depth";
        }
        semantic_offset +=
            dxbc::AppendAlignedString(shader_object_, depth_semantic_name);
      }
    }
  }

  // Header.
  {
    auto& header = *reinterpret_cast<dxbc::Signature*>(shader_object_.data() +
                                                       blob_position);
    header.parameter_count = parameter_count;
    header.parameter_info_ptr = sizeof(dxbc::Signature);
  }
}

void DxbcShaderTranslator::WriteShaderCode() {
  uint32_t blob_position_dwords = uint32_t(shader_object_.size());

  dxbc::ProgramType program_type;
  if (IsDxbcVertexShader()) {
    program_type = dxbc::ProgramType::kVertexShader;
  } else if (IsDxbcDomainShader()) {
    program_type = dxbc::ProgramType::kDomainShader;
  } else {
    assert_true(is_pixel_shader());
    program_type = dxbc::ProgramType::kPixelShader;
  }
  shader_object_.push_back(dxbc::VersionToken(program_type, 5, 1));
  // Reserve space for the length token.
  shader_object_.push_back(0);

  Modification shader_modification = GetDxbcShaderModification();

  if (IsDxbcDomainShader()) {
    // Not using control point data since Xenos only has a vertex shader acting
    // as both vertex shader and domain shader.
    uint32_t control_point_count = 3;
    dxbc::TessellatorDomain tessellator_domain =
        dxbc::TessellatorDomain::kTriangle;
    switch (shader_modification.vertex.host_vertex_shader_type) {
      case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
      case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
        control_point_count = 3;
        tessellator_domain = dxbc::TessellatorDomain::kTriangle;
        break;
      case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
      case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
        control_point_count = 4;
        tessellator_domain = dxbc::TessellatorDomain::kQuad;
        break;
      default:
        // TODO(Triang3l): Support line patches.
        assert_unhandled_case(
            shader_modification.vertex.host_vertex_shader_type);
        EmitTranslationError(
            "Unsupported host vertex shader type in WriteShaderCode");
    }
    ao_.OpDclInputControlPointCount(control_point_count);
    ao_.OpDclTessDomain(tessellator_domain);
  }

  // Don't allow refactoring when converting to native code to maintain position
  // invariance (needed even in pixel shaders for oDepth invariance).
  uint32_t global_flags = 0;
  if (is_pixel_shader() &&
      GetDxbcShaderModification().pixel.depth_stencil_mode ==
          Modification::DepthStencilMode::kEarlyHint &&
      !edram_rov_used_ && current_shader().implicit_early_z_write_allowed()) {
    global_flags |= dxbc::kGlobalFlagForceEarlyDepthStencil;
  }
  ao_.OpDclGlobalFlags(global_flags);

  // Constant buffers, from most frequenly accessed to least frequently accessed
  // (the order is a hint to the driver according to the DXBC header).
  if (cbuffer_index_float_constants_ != kBindingIndexUnallocated) {
    const Shader::ConstantRegisterMap& constant_register_map =
        current_shader().constant_register_map();
    assert_not_zero(constant_register_map.float_count);
    ao_.OpDclConstantBuffer(
        dxbc::Src::CB(dxbc::Src::Dcl, cbuffer_index_float_constants_,
                      uint32_t(CbufferRegister::kFloatConstants),
                      uint32_t(CbufferRegister::kFloatConstants)),
        constant_register_map.float_count,
        constant_register_map.float_dynamic_addressing
            ? dxbc::ConstantBufferAccessPattern::kDynamicIndexed
            : dxbc::ConstantBufferAccessPattern::kImmediateIndexed);
  }
  if (cbuffer_index_system_constants_ != kBindingIndexUnallocated) {
    ao_.OpDclConstantBuffer(
        dxbc::Src::CB(dxbc::Src::Dcl, cbuffer_index_system_constants_,
                      uint32_t(CbufferRegister::kSystemConstants),
                      uint32_t(CbufferRegister::kSystemConstants)),
        (sizeof(SystemConstants) + 15) >> 4);
  }
  if (cbuffer_index_fetch_constants_ != kBindingIndexUnallocated) {
    ao_.OpDclConstantBuffer(
        dxbc::Src::CB(dxbc::Src::Dcl, cbuffer_index_fetch_constants_,
                      uint32_t(CbufferRegister::kFetchConstants),
                      uint32_t(CbufferRegister::kFetchConstants)),
        48);
  }
  if (cbuffer_index_descriptor_indices_ != kBindingIndexUnallocated) {
    assert_not_zero(GetBindlessResourceCount());
    ao_.OpDclConstantBuffer(
        dxbc::Src::CB(dxbc::Src::Dcl, cbuffer_index_descriptor_indices_,
                      uint32_t(CbufferRegister::kDescriptorIndices),
                      uint32_t(CbufferRegister::kDescriptorIndices)),
        (GetBindlessResourceCount() + 3) >> 2);
  }
  if (cbuffer_index_bool_loop_constants_ != kBindingIndexUnallocated) {
    ao_.OpDclConstantBuffer(
        dxbc::Src::CB(dxbc::Src::Dcl, cbuffer_index_bool_loop_constants_,
                      uint32_t(CbufferRegister::kBoolLoopConstants),
                      uint32_t(CbufferRegister::kBoolLoopConstants)),
        2 + 8);
  }

  // Samplers.
  if (!sampler_bindings_.empty()) {
    if (bindless_resources_used_) {
      // Bindless sampler heap.
      ao_.OpDclSampler(dxbc::Src::S(dxbc::Src::Dcl, 0, 0, UINT32_MAX));
    } else {
      // Bindful samplers.
      for (uint32_t i = 0; i < uint32_t(sampler_bindings_.size()); ++i) {
        const SamplerBinding& sampler_binding = sampler_bindings_[i];
        ao_.OpDclSampler(dxbc::Src::S(dxbc::Src::Dcl, i, i, i));
      }
    }
  }

  // Shader resource views, sorted by binding index.
  for (uint32_t i = 0; i < srv_count_; ++i) {
    if (i == srv_index_shared_memory_) {
      // Shared memory ByteAddressBuffer.
      ao_.OpDclResourceRaw(
          dxbc::Src::T(dxbc::Src::Dcl, srv_index_shared_memory_,
                       uint32_t(SRVMainRegister::kSharedMemory),
                       uint32_t(SRVMainRegister::kSharedMemory)),
          uint32_t(SRVSpace::kMain));
    } else {
      // Texture or texture heap.
      dxbc::ResourceDimension texture_dimension;
      uint32_t texture_register_lower_bound, texture_register_upper_bound;
      SRVSpace texture_register_space;
      if (bindless_resources_used_) {
        // Bindless texture heap.
        texture_register_lower_bound = 0;
        texture_register_upper_bound = UINT32_MAX;
        if (i == srv_index_bindless_textures_3d_) {
          texture_dimension = dxbc::ResourceDimension::kTexture3D;
          texture_register_space = SRVSpace::kBindlessTextures3D;
        } else if (i == srv_index_bindless_textures_cube_) {
          texture_dimension = dxbc::ResourceDimension::kTextureCube;
          texture_register_space = SRVSpace::kBindlessTexturesCube;
        } else {
          assert_true(i == srv_index_bindless_textures_2d_);
          texture_dimension = dxbc::ResourceDimension::kTexture2DArray;
          texture_register_space = SRVSpace::kBindlessTextures2DArray;
        }
      } else {
        // Bindful texture.
        auto it = texture_bindings_for_bindful_srv_indices_.find(i);
        assert_true(it != texture_bindings_for_bindful_srv_indices_.end());
        uint32_t texture_binding_index = it->second;
        const TextureBinding& texture_binding =
            texture_bindings_[texture_binding_index];
        switch (texture_binding.dimension) {
          case xenos::FetchOpDimension::k3DOrStacked:
            texture_dimension = dxbc::ResourceDimension::kTexture3D;
            break;
          case xenos::FetchOpDimension::kCube:
            texture_dimension = dxbc::ResourceDimension::kTextureCube;
            break;
          default:
            assert_true(texture_binding.dimension ==
                        xenos::FetchOpDimension::k2D);
            texture_dimension = dxbc::ResourceDimension::kTexture2DArray;
        }
        texture_register_lower_bound =
            uint32_t(SRVMainRegister::kBindfulTexturesStart) +
            texture_binding_index;
        texture_register_upper_bound = texture_register_lower_bound;
        texture_register_space = SRVSpace::kMain;
      }
      ao_.OpDclResource(
          texture_dimension,
          dxbc::ResourceReturnTypeX4Token(dxbc::ResourceReturnType::kFloat),
          dxbc::Src::T(dxbc::Src::Dcl, i, texture_register_lower_bound,
                       texture_register_upper_bound),
          uint32_t(texture_register_space));
    }
  }

  // Unordered access views, sorted by binding index.
  for (uint32_t i = 0; i < uav_count_; ++i) {
    if (i == uav_index_shared_memory_) {
      // Shared memory RWByteAddressBuffer.
      if (!is_pixel_shader()) {
        shader_feature_info_.feature_flags[0] |=
            dxbc::kShaderFeature0_UAVsAtEveryStage;
      }
      ao_.OpDclUnorderedAccessViewRaw(
          0, dxbc::Src::U(dxbc::Src::Dcl, uav_index_shared_memory_,
                          uint32_t(UAVRegister::kSharedMemory),
                          uint32_t(UAVRegister::kSharedMemory)));
    } else if (i == uav_index_edram_) {
      // EDRAM buffer R32_UINT rasterizer-ordered view.
      shader_feature_info_.feature_flags[0] |= dxbc::kShaderFeature0_ROVs;
      ao_.OpDclUnorderedAccessViewTyped(
          dxbc::ResourceDimension::kBuffer,
          dxbc::kUAVFlagRasterizerOrderedAccess,
          dxbc::ResourceReturnTypeX4Token(dxbc::ResourceReturnType::kUInt),
          dxbc::Src::U(dxbc::Src::Dcl, uav_index_edram_,
                       uint32_t(UAVRegister::kEdram),
                       uint32_t(UAVRegister::kEdram)));
    } else {
      assert_unhandled_case(i);
    }
  }

  // Inputs and outputs.
  if (is_vertex_shader()) {
    if (IsDxbcDomainShader()) {
      if (in_domain_location_used_) {
        // Domain location input.
        ao_.OpDclInput(dxbc::Dest::VDomain(in_domain_location_used_));
      }
      if (in_primitive_id_used_) {
        // Primitive (patch) index input.
        ao_.OpDclInput(dxbc::Dest::VPrim());
      }
      if (in_control_point_index_used_) {
        // Control point indices as float input.
        uint32_t control_point_array_size = 3;
        switch (shader_modification.vertex.host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
            control_point_array_size = 3;
            break;
          case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
            control_point_array_size = 4;
            break;
          default:
            // TODO(Triang3l): Support line patches.
            assert_unhandled_case(
                shader_modification.vertex.host_vertex_shader_type);
            EmitTranslationError(
                "Unsupported host vertex shader type in "
                "StartVertexOrDomainShader");
        }
        ao_.OpDclInput(dxbc::Dest::VICP(
            control_point_array_size,
            uint32_t(InOutRegister::kDSInControlPointIndex), 0b0001));
      }
    } else {
      if (register_count()) {
        // Unswapped vertex index input (only X component).
        ao_.OpDclInputSGV(
            dxbc::Dest::V(uint32_t(InOutRegister::kVSInVertexIndex), 0b0001),
            dxbc::Name::kVertexID);
      }
    }
    // Interpolator output.
    for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
      ao_.OpDclOutput(
          dxbc::Dest::O(uint32_t(InOutRegister::kVSDSOutInterpolators) + i));
    }
    // Point parameters output.
    ao_.OpDclOutput(dxbc::Dest::O(
        uint32_t(InOutRegister::kVSDSOutPointParameters), 0b0111));
    // Position output.
    ao_.OpDclOutputSIV(dxbc::Dest::O(uint32_t(InOutRegister::kVSDSOutPosition)),
                       dxbc::Name::kPosition);
    // Clip distance outputs.
    for (uint32_t i = 0; i < 2; ++i) {
      ao_.OpDclOutputSIV(
          dxbc::Dest::O(uint32_t(InOutRegister::kVSDSOutClipDistance0123) + i,
                        i ? 0b0011 : 0b1111),
          dxbc::Name::kClipDistance);
    }
    // Cull distance output.
    ao_.OpDclOutputSIV(
        dxbc::Dest::O(
            uint32_t(InOutRegister::kVSDSOutClipDistance45AndCullDistance),
            0b0100),
        dxbc::Name::kCullDistance);
  } else if (is_pixel_shader()) {
    bool is_writing_float24_depth = DSV_IsWritingFloat24Depth();
    bool shader_writes_depth = current_shader().writes_depth();
    // Interpolator input.
    if (!is_depth_only_pixel_shader_) {
      uint32_t interpolator_count =
          std::min(xenos::kMaxInterpolators, register_count());
      for (uint32_t i = 0; i < interpolator_count; ++i) {
        ao_.OpDclInputPS(
            dxbc::InterpolationMode::kLinear,
            dxbc::Dest::V(uint32_t(InOutRegister::kPSInInterpolators) + i));
      }
      if (register_count()) {
        // Point parameters input (only coordinates, not size, needed).
        ao_.OpDclInputPS(
            dxbc::InterpolationMode::kLinear,
            dxbc::Dest::V(uint32_t(InOutRegister::kPSInPointParameters),
                          0b0011));
      }
    }
    if (in_position_used_) {
      // Position input (XY needed for ps_param_gen, Z needed for non-ROV
      // float24 conversion; the ROV depth code calculates the depth the from
      // clip space Z and W with pull-mode per-sample interpolation instead).
      // At the cost of possibility of MSAA with pixel-rate shading, need
      // per-sample depth - otherwise intersections cannot be antialiased, and
      // with SV_DepthLessEqual, per-sample (or centroid, but this isn't
      // applicable here) position is mandatory. However, with depth output, on
      // the guest, there's only one depth value for the whole pixel.
      ao_.OpDclInputPSSIV(
          (is_writing_float24_depth && !shader_writes_depth)
              ? dxbc::InterpolationMode::kLinearNoPerspectiveSample
              : dxbc::InterpolationMode::kLinearNoPerspective,
          dxbc::Dest::V(uint32_t(InOutRegister::kPSInPosition),
                        in_position_used_),
          dxbc::Name::kPosition);
    }
    bool sample_rate_memexport =
        current_shader().is_valid_memexport_used() && IsSampleRate();
    // Sample-rate shading can't be done with UAV-only rendering (sample-rate
    // shading is only needed for float24 depth conversion when using a float32
    // host depth buffer).
    assert_false(sample_rate_memexport && edram_rov_used_);
    uint32_t front_face_and_sample_index_mask =
        uint32_t(in_front_face_used_) | (uint32_t(sample_rate_memexport) << 1);
    if (front_face_and_sample_index_mask) {
      // Is front face, sample index.
      ao_.OpDclInputPSSGV(
          dxbc::Dest::V(uint32_t(InOutRegister::kPSInFrontFaceAndSampleIndex),
                        front_face_and_sample_index_mask),
          dxbc::Name::kIsFrontFace);
    }
    if (edram_rov_used_) {
      // Sample coverage input.
      ao_.OpDclInput(dxbc::Dest::VCoverage());
    } else {
      if (sample_rate_memexport) {
        // Sample coverage input.
        ao_.OpDclInput(dxbc::Dest::VCoverage());
      }
      // Color output.
      uint32_t color_targets_written = current_shader().writes_color_targets();
      for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
        if (color_targets_written & (uint32_t(1) << i)) {
          ao_.OpDclOutput(dxbc::Dest::O(i));
        }
      }
      // Coverage output for alpha to mask.
      if (color_targets_written & 0b1) {
        ao_.OpDclOutput(dxbc::Dest::OMask());
      }
      // Depth output.
      if (is_writing_float24_depth || shader_writes_depth) {
        if (!shader_writes_depth &&
            GetDxbcShaderModification().pixel.depth_stencil_mode ==
                Modification::DepthStencilMode::kFloat24Truncating) {
          ao_.OpDclOutput(dxbc::Dest::ODepthLE());
        } else {
          ao_.OpDclOutput(dxbc::Dest::ODepth());
        }
      }
    }
  }

  // Temporary registers - guest general-purpose registers if not using dynamic
  // indexing and Xenia internal registers.
  uint32_t temp_register_count = system_temp_count_max_;
  if (!is_depth_only_pixel_shader_ &&
      !current_shader().uses_register_dynamic_addressing()) {
    temp_register_count += register_count();
  }
  if (temp_register_count) {
    ao_.OpDclTemps(temp_register_count);
  }
  // General-purpose registers if using dynamic indexing (x0).
  if (!is_depth_only_pixel_shader_ &&
      current_shader().uses_register_dynamic_addressing()) {
    assert_not_zero(register_count());
    ao_.OpDclIndexableTemp(0, register_count(), 4);
  }

  // Write the translated shader code.
  size_t code_size_dwords = shader_code_.size();
  if (code_size_dwords) {
    shader_object_.resize(shader_object_.size() + code_size_dwords);
    std::memcpy(
        shader_object_.data() + (shader_object_.size() - code_size_dwords),
        shader_code_.data(), code_size_dwords * sizeof(uint32_t));
  }

  // Write the length.
  shader_object_[blob_position_dwords + 1] =
      uint32_t(shader_object_.size()) - blob_position_dwords;
}

}  // namespace gpu
}  // namespace xe
