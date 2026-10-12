#include "mkwv/runtime/hle_native.hpp"

#include "mkwv/gx/capture.hpp"
#include "mkwv/gx/fifo.hpp"
#include "mkwv/gx/draw_state.hpp"
#include "mkwv/gx/efb_copy.hpp"
#include "mkwv/gx/perf.hpp"
#include "mkwv/gx/vertex_worker.hpp"
#include "mkwv/runtime/compact_memory.hpp"
#include "mkwv/runtime/guest_access.hpp"
#include "mkwv/runtime/diagnostics.hpp"
#include "mkwv/runtime/hle_fifo.hpp"
#include "mkwv/runtime/hle_audio.hpp"
#include "mkwv/runtime/game_options.hpp"
#include "mkwv/runtime/engine_native.hpp"
#include "mkwv/runtime/guest_prof.hpp"
#include "mkwv/runtime/guest_sync.hpp"
#include "mkwv/runtime/hle_os.hpp"

#include "abi_bridge.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

extern "C" std::int32_t NAND__Check_8019ead0(std::uint32_t num_bytes, std::uint32_t num_inodes,
                                             std::uint32_t answer_ptr);
extern "C" void nw4r__lyt__detail__DrawQuad_800847c0(CpuContext* ctx);
extern "C" void nw4r__lyt__detail__DrawQuad_80084d20(CpuContext* ctx);
extern "C" void LCEnable_801a1834(CpuContext*);
extern "C" void LCDisable_801a186c(CpuContext*);
extern "C" void LCLoadBlocks_801a1894(CpuContext*);
extern "C" void LCStoreBlocks_801a18b8(CpuContext*);
extern "C" void LCStoreData_801a18dc(CpuContext*);
extern "C" void LCQueueLength_801a197c(CpuContext*);
extern "C" void LCQueueWait_801a1988(CpuContext*);
extern "C" void PPCSync_8012e59c(CpuContext*);
extern "C" void PADReset_801af0dc(CpuContext*);

#if defined(__vita__)
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
#endif

namespace mkwv::runtime {
bool hle_gx_in_display_list();
namespace {

struct NativeEntry {
    std::uint32_t address;
    TranslatedFn fn;
};

HleFifo* g_fifo = nullptr;
gx::Processor* g_processor = nullptr;
std::uint32_t g_orphan_copy_drains = 0;  // empty drains kept because they carried an EFB copy
HlePresentFn g_present = nullptr;
const RenderOffload* g_offload = nullptr;  // set when the render thread owns decode + GL
std::uint32_t g_frames = 0;
std::uint32_t g_last_draws = 0;
std::uint32_t g_last_verts = 0;
std::uint32_t g_last_fifo = 0;
std::uint32_t g_max_draws = 0;
std::uint32_t g_skipped = 0;
char g_draw_dump[5120];
bool g_draw_dump_ready = false;
std::uint8_t g_fifo_bytes[1400];
std::uint32_t g_fifo_bytes_len = 0;

// GX natives reached through find_hle_native (indirect calls and calls the compile-time
// bindings in native_cpu_calls.inc do not cover). Same argument unpacking as those bindings.
void wrap_gx_SetVtxDesc_8016d3a4(CpuContext* cpu) {
    GX__SetVtxDesc_8016d3a4(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_SetVtxDescv_8016d608(CpuContext* cpu) {
    GX__SetVtxDescv_8016d608(cpu->gpr[3]);
}
void wrap_gx_GetVtxDesc_8016d9f0(CpuContext* cpu) {
    GX__GetVtxDesc_8016d9f0(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_GetVtxDescv_8016dba4(CpuContext* cpu) {
    GX__GetVtxDescv_8016dba4(cpu->gpr[3]);
}
void wrap_gx_ClearVtxDesc_8016dc34(CpuContext* cpu) {
    GX__ClearVtxDesc_8016dc34();
}
void wrap_gx_SetVtxAttrFmt_8016dc68(CpuContext* cpu) {
    GX__SetVtxAttrFmt_8016dc68(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
}
void wrap_gx_SetVtxAttrFmtv_8016de08(CpuContext* cpu) {
    GX__SetVtxAttrFmtv_8016de08(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_GetVtxAttrFmt_8016e04c(CpuContext* cpu) {
    GX__GetVtxAttrFmt_8016e04c(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
}
void wrap_gx_GetVtxAttrFmtv_8016e2b8(CpuContext* cpu) {
    GX__GetVtxAttrFmtv_8016e2b8(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_SetArray_8016e32c(CpuContext* cpu) {
    GX__SetArray_8016e32c(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}
void wrap_gx_InvalidateVtxCache_8016e36c(CpuContext* cpu) {
    GX__InvalidateVtxCache_8016e36c();
}
void wrap_gx_SetTexCoordGen2_8016e37c(CpuContext* cpu) {
    GX__SetTexCoordGen2_8016e37c(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
}
void wrap_gx_SetNumTexGens_8016e5a4(CpuContext* cpu) {
    GX__SetNumTexGens_8016e5a4(cpu->gpr[3]);
}
void wrap_gx_DrawDone_8016eab0(CpuContext* cpu) {
    GX__DrawDone_8016eab0();
}
void wrap_gx_PixModeSync_8016eb70(CpuContext* cpu) {
    GX__PixModeSync_8016eb70();
}
void wrap_gx_Begin_8016f0f0(CpuContext* cpu) {
    GX__Begin_8016f0f0(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}
void wrap_gx_SetLineWidth_8016f314(CpuContext* cpu) {
    GX__SetLineWidth_8016f314(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_SetPointSize_8016f348(CpuContext* cpu) {
    GX__SetPointSize_8016f348(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_EnableTexOffsets_8016f37c(CpuContext* cpu) {
    GX__EnableTexOffsets_8016f37c(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}
void wrap_gx_SetCullMode_8016f3b8(CpuContext* cpu) {
    GX__SetCullMode_8016f3b8(cpu->gpr[3]);
}
void wrap_gx_SetCoPlanar_8016f3e0(CpuContext* cpu) {
    GX__SetCoPlanar_8016f3e0(cpu->gpr[3]);
}
void wrap_gx_SetTexCopySrc_8016f478(CpuContext* cpu) {
    GX__SetTexCopySrc_8016f478(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
}
void wrap_gx_SetTexCopyDst_8016f4dc(CpuContext* cpu) {
    GX__SetTexCopyDst_8016f4dc(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
}
void wrap_gx_SetCopyClamp_8016f618(CpuContext* cpu) {
    GX__SetCopyClamp_8016f618(cpu->gpr[3]);
}
void wrap_gx_CopyTex_8016fd74(CpuContext* cpu) {
    GX__CopyTex_8016fd74(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_InitLightAttn_8016ff04(CpuContext* cpu) {
    GX__InitLightAttn_8016ff04(cpu->gpr[3], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d), static_cast<float>(cpu->fpr[4].d), static_cast<float>(cpu->fpr[5].d), static_cast<float>(cpu->fpr[6].d));
}
void wrap_gx_InitLightAttnA_8016ff20(CpuContext* cpu) {
    GX__InitLightAttnA_8016ff20(cpu->gpr[3], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d));
}
void wrap_gx_InitLightAttnK_8016ff30(CpuContext* cpu) {
    GX__InitLightAttnK_8016ff30(cpu->gpr[3], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d));
}
void wrap_gx_InitLightPos_80170198(CpuContext* cpu) {
    GX__InitLightPos_80170198(cpu->gpr[3], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d));
}
void wrap_gx_GetLightPos_801701a8(CpuContext* cpu) {
    GX__GetLightPos_801701a8(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
}
void wrap_gx_InitLightDir_801701c4(CpuContext* cpu) {
    GX__InitLightDir_801701c4(cpu->gpr[3], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d));
}
void wrap_gx_GetLightDir_801701e0(CpuContext* cpu) {
    GX__GetLightDir_801701e0(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
}
void wrap_gx_InitLightColor_80170314(CpuContext* cpu) {
    GX__InitLightColor_80170314(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_LoadLightObjImm_80170320(CpuContext* cpu) {
    GX__LoadLightObjImm_80170320(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_SetChanAmbColor_8017039c(CpuContext* cpu) {
    GX__SetChanAmbColor_8017039c(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_SetChanMatColor_80170474(CpuContext* cpu) {
    GX__SetChanMatColor_80170474(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_SetNumChans_8017054c(CpuContext* cpu) {
    GX__SetNumChans_8017054c(cpu->gpr[3]);
}
void wrap_gx_SetChanCtrl_80170570(CpuContext* cpu) {
    GX__SetChanCtrl_80170570(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9]);
}
void wrap_gx_GetImageTileCount_80170738(CpuContext* cpu) {
    GX__GetImageTileCount_80170738(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
}
void wrap_gx_InitTexObj_801707f8(CpuContext* cpu) {
    GX__InitTexObj_801707f8(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9], cpu->gpr[10]);
}
void wrap_gx_InitTexObjLOD_80170a4c(CpuContext* cpu) {
    GX__InitTexObjLOD_80170a4c(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d), cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
}
void wrap_gx_InitTexObjWrapMode_80170b50(CpuContext* cpu) {
    GX__InitTexObjWrapMode_80170b50(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}
void wrap_gx_InitTexObjTlut_80170b64(CpuContext* cpu) {
    GX__InitTexObjTlut_80170b64(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_InitTexObjFilter_80170b6c(CpuContext* cpu) {
    GX__InitTexObjFilter_80170b6c(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}
void wrap_gx_InitTexObjLODBias_80170b94(CpuContext* cpu) {
    GX__InitTexObjLODBias_80170b94(cpu->gpr[3], static_cast<float>(cpu->fpr[1].d));
}
void wrap_gx_InitTexObjUserData_80170be8(CpuContext* cpu) {
    GX__InitTexObjUserData_80170be8(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_GetTexObjAll_80170bf8(CpuContext* cpu) {
    GX__GetTexObjAll_80170bf8(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9], cpu->gpr[10]);
}
void wrap_gx_LoadTexObjPreLoaded_80170dc8(CpuContext* cpu) {
    GX__LoadTexObjPreLoaded_80170dc8(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}
void wrap_gx_LoadTexObj_80170f2c(CpuContext* cpu) {
    GX__LoadTexObj_80170f2c(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_InitTlutObj_80170f80(CpuContext* cpu) {
    GX__InitTlutObj_80170f80(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
}
void wrap_gx_LoadTlut_80170fa8(CpuContext* cpu) {
    GX__LoadTlut_80170fa8(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_InvalidateTexAll_80171110(CpuContext* cpu) {
    GX__InvalidateTexAll_80171110();
}
void wrap_gx_SetTexCoordScaleManually_80171180(CpuContext* cpu) {
    GX__SetTexCoordScaleManually_80171180(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
}
void wrap_gx_SetTexCoordBias_801711fc(CpuContext* cpu) {
    GX__SetTexCoordBias_801711fc(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}
void wrap_gx_SetTevIndirect_801717ac(CpuContext* cpu) {
    GX__SetTevIndirect_801717ac(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9], cpu->gpr[10], Memory::Read32(cpu->gpr[1] + 8u), Memory::Read32(cpu->gpr[1] + 12u));
}
void wrap_gx_SetIndTexMtx_80171814(CpuContext* cpu) {
    GX__SetIndTexMtx_80171814(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}
void wrap_gx_SetIndTexCoordScale_80171968(CpuContext* cpu) {
    GX__SetIndTexCoordScale_80171968(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}
void wrap_gx_SetIndTexOrder_80171a6c(CpuContext* cpu) {
    GX__SetIndTexOrder_80171a6c(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}
void wrap_gx_SetNumIndStages_80171b38(CpuContext* cpu) {
    GX__SetNumIndStages_80171b38(cpu->gpr[3]);
}
void wrap_gx_SetTevDirect_80171b58(CpuContext* cpu) {
    GX__SetTevDirect_80171b58(cpu->gpr[3]);
}
void wrap_gx_SetTevIndWarp_80171ba0(CpuContext* cpu) {
    GX__SetTevIndWarp_80171ba0(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
}
void wrap_gx_SetTevOp_80171c4c(CpuContext* cpu) {
    GX__SetTevOp_80171c4c(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_SetTevColorIn_80171ce0(CpuContext* cpu) {
    GX__SetTevColorIn_80171ce0(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
}
void wrap_gx_SetTevAlphaIn_80171d20(CpuContext* cpu) {
    GX__SetTevAlphaIn_80171d20(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
}
void wrap_gx_SetTevColorOp_80171d60(CpuContext* cpu) {
    GX__SetTevColorOp_80171d60(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
}
void wrap_gx_SetTevAlphaOp_80171db8(CpuContext* cpu) {
    GX__SetTevAlphaOp_80171db8(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
}
void wrap_gx_SetTevColor_80171e10(CpuContext* cpu) {
    GX__SetTevColor_80171e10(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_SetTevColorS10_80171e70(CpuContext* cpu) {
    GX__SetTevColorS10_80171e70(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_SetTevKColor_80171ed4(CpuContext* cpu) {
    GX__SetTevKColor_80171ed4(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_SetTevKColorSel_80171f30(CpuContext* cpu) {
    GX__SetTevKColorSel_80171f30(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_SetTevKAlphaSel_80171f80(CpuContext* cpu) {
    GX__SetTevKAlphaSel_80171f80(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_SetTevSwapMode_80171fd0(CpuContext* cpu) {
    GX__SetTevSwapMode_80171fd0(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}
void wrap_gx_SetTevSwapModeTable_8017200c(CpuContext* cpu) {
    GX__SetTevSwapModeTable_8017200c(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
}
void wrap_gx_SetAlphaCompare_80172088(CpuContext* cpu) {
    GX__SetAlphaCompare_80172088(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
}
void wrap_gx_SetZTexture_801720c0(CpuContext* cpu) {
    GX__SetZTexture_801720c0(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}
void wrap_gx_SetTevOrder_8017214c(CpuContext* cpu) {
    GX__SetTevOrder_8017214c(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
}
void wrap_gx_SetNumTevStages_801722a8(CpuContext* cpu) {
    GX__SetNumTevStages_801722a8(cpu->gpr[3]);
}
void wrap_gx_SetFog_801722cc(CpuContext* cpu) {
    GX__SetFog_801722cc(cpu->gpr[3], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d), static_cast<float>(cpu->fpr[4].d), cpu->gpr[4]);
}
void wrap_gx_InitFogAdjTable_801724f8(CpuContext* cpu) {
    GX__InitFogAdjTable_801724f8(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}
void wrap_gx_SetBlendMode_8017277c(CpuContext* cpu) {
    GX__SetBlendMode_8017277c(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
}
void wrap_gx_SetColorUpdate_801727cc(CpuContext* cpu) {
    GX__SetColorUpdate_801727cc(cpu->gpr[3]);
}
void wrap_gx_SetAlphaUpdate_801727f8(CpuContext* cpu) {
    GX__SetAlphaUpdate_801727f8(cpu->gpr[3]);
}
void wrap_gx_SetZMode_80172824(CpuContext* cpu) {
    GX__SetZMode_80172824(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}
void wrap_gx_SetZCompLoc_80172858(CpuContext* cpu) {
    GX__SetZCompLoc_80172858(cpu->gpr[3]);
}
void wrap_gx_SetPixelFmt_80172888(CpuContext* cpu) {
    GX__SetPixelFmt_80172888(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_SetDither_80172930(CpuContext* cpu) {
    GX__SetDither_80172930(cpu->gpr[3]);
}
void wrap_gx_SetDstAlpha_8017295c(CpuContext* cpu) {
    GX__SetDstAlpha_8017295c(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_SetFieldMask_8017298c(CpuContext* cpu) {
    GX__SetFieldMask_8017298c(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_DrawSphere_80172a30(CpuContext* cpu) {
    GX__DrawSphere_80172a30(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_BeginDisplayList_80172e00(CpuContext* cpu) {
    GX__BeginDisplayList_80172e00(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_CallDisplayList_80172f64(CpuContext* cpu) {
    GX__CallDisplayList_80172f64(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_SetProjection_8017301c(CpuContext* cpu) {
    GX__SetProjection_8017301c(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_SetProjectionv_80173080(CpuContext* cpu) {
    GX__SetProjectionv_80173080(cpu->gpr[3]);
}
void wrap_gx_GetProjectionv_801730cc(CpuContext* cpu) {
    GX__GetProjectionv_801730cc(cpu->gpr[3]);
}
void wrap_gx_LoadPosMtxImm_8017310c(CpuContext* cpu) {
    GX__LoadPosMtxImm_8017310c(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_LoadPosMtxIndx_8017315c(CpuContext* cpu) {
    GX__LoadPosMtxIndx_8017315c(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_LoadNrmMtxImm_80173188(CpuContext* cpu) {
    GX__LoadNrmMtxImm_80173188(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_LoadNrmMtxIndx3x3_801731e0(CpuContext* cpu) {
    GX__LoadNrmMtxIndx3x3_801731e0(cpu->gpr[3], cpu->gpr[4]);
}
void wrap_gx_SetCurrentMtx_80173214(CpuContext* cpu) {
    GX__SetCurrentMtx_80173214(cpu->gpr[3]);
}
void wrap_gx_LoadTexMtxImm_80173234(CpuContext* cpu) {
    GX__LoadTexMtxImm_80173234(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}
void wrap_gx_SetViewportJitter_80173378(CpuContext* cpu) {
    GX__SetViewportJitter_80173378(static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d), static_cast<float>(cpu->fpr[4].d), static_cast<float>(cpu->fpr[5].d), static_cast<float>(cpu->fpr[6].d), cpu->gpr[3]);
}
void wrap_gx_SetViewport_801733b4(CpuContext* cpu) {
    GX__SetViewport_801733b4(static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d), static_cast<float>(cpu->fpr[4].d), static_cast<float>(cpu->fpr[5].d), static_cast<float>(cpu->fpr[6].d));
}
void wrap_gx_GetViewportv_801733e0(CpuContext* cpu) {
    GX__GetViewportv_801733e0(cpu->gpr[3]);
}
void wrap_gx_SetZScaleOffset_80173400(CpuContext* cpu) {
    GX__SetZScaleOffset_80173400(static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d));
}
void wrap_gx_SetScissor_80173430(CpuContext* cpu) {
    GX__SetScissor_80173430(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
}
void wrap_gx_GetScissor_80173498(CpuContext* cpu) {
    GX__GetScissor_80173498(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
}
void wrap_gx_SetScissorBoxOffset_801734e0(CpuContext* cpu) {
    GX__SetScissorBoxOffset_801734e0(static_cast<int32_t>(cpu->gpr[3]), static_cast<int32_t>(cpu->gpr[4]));
}
void wrap_gx_SetClipMode_8017351c(CpuContext* cpu) {
    GX__SetClipMode_8017351c(cpu->gpr[3]);
}
void wrap_nand_check(CpuContext* ctx) {
    ctx->gpr[3] = static_cast<std::uint32_t>(NAND__Check_8019ead0(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]));
}

// EGG::DrawPathDOF::DrawImpl(this, proc index), reached only through DrawPathBase::Draw's
// virtual call. Upstream WiiCompiled never creates this path: its ScnRenderer::createPath hook
// (0x8023BD38, path mask in r4, called from ScnMgrRace::LoadPostEffects with 0x26 for one
// player) always clears bit 0x20. That call is static here (no hook without a shard rebuild),
// so the path is created but its draw does nothing. Its three procs are symmetric - proc 0
// allocates the blur buffers and composites, proc 2 frees them - so skipping all of them is
// the same as the path not existing. The DOF composite blends a blurred copy over the scene
// by depth: far geometry (most of the track) takes the blurred image, near geometry (the kart,
// nearby course pieces) stays sharp.
bool g_race_dof = false;

void DrawPathDOF_DrawImpl_8021eec0(CpuContext* ctx) {
    if (!g_race_dof) {
        return;
    }
    // The translated original, from the static dispatch table (the native shadows it).
    static TranslatedFn original = nullptr;
    if (original == nullptr) {
        const RawDispatchRecord* record = FindStaticIndirectDispatchEntry(
            g_publishedStaticIndirectDispatchTable.load(std::memory_order_acquire), 0x8021EEC0u);
        original = record != nullptr ? record->entry : nullptr;
    }
    if (original != nullptr) {
        original(ctx);
    }
}

} // namespace

void set_race_dof(bool enabled) {
    g_race_dof = enabled;
}
bool race_dof_enabled() {
    return g_race_dof;
}

namespace {

NativeEntry g_natives[] = {
    {0x80006348u, __init_hardware_80006348},
    {0x800074A4u, StrapScene_LinkModule_800074a4},
    {0x800077C8u, StrapScene_CheckInput_800077c8},
    {0x80015500u, ConsoleWrite_80015500},
    // The plain (no alpha) overload: window frames, and every pane drawn without a
    // modulating alpha. Implemented in hle_gx.cpp but was never registered, so those
    // quads were dropped as missing calls.
    {0x800847C0u, nw4r__lyt__detail__DrawQuad_800847c0},
    {0x80084D20u, nw4r__lyt__detail__DrawQuad_80084d20},
    {0x800895D0u, AxManager_AuxCallbackFunc_800895d0},
    {0x800A35E0u, SoundPlayerSetVolume_800a35e0},
    {0x800BD860u, RFLiIsWorking_HLE_800bd860},
    {0x80123F88u, AIRegisterDMACallback_80123f88},
    {0x80123FCCu, AIInitDMA_80123fcc},
    {0x80124048u, AIStartDMA_80124048},
    {0x8012405Cu, AIGetDMABytesLeft_8012405c},
    {0x8012406Cu, AIGetDMAStartAddr_8012406c},
    {0x80124084u, AIGetDMALength_80124084},
    {0x80124094u, AICheckInit_80124094},
    {0x8012409Cu, AIGetDSPSampleRate_8012409c},
    {0x801240B0u, AIInit_801240b0},
    {0x801269BCu, AXOutInitDSP_801269bc},
    {0x8012E59Cu, PPCSync_8012e59c},
    {0x8012E5A4u, PPCHalt_8012e5a4},
    {0x8012E5B8u, PPCMtmmcr0_8012e5b8},
    {0x8012E5C0u, PPCMtmmcr1_8012e5c0},
    {0x8012E5C8u, PPCMtpmc1_8012e5c8},
    {0x8012E5D0u, PPCMtpmc2_8012e5d0},
    {0x8012E5D8u, PPCMtpmc3_8012e5d8},
    {0x8012E5E0u, PPCMtpmc4_8012e5e0},
    {0x8012E630u, PPCMfhid2_HLE_8012e630},
    {0x8012E638u, PPCMthid2_8012e638},
    {0x8012E654u, PPCDisableSpeculation_8012e654},
    {0x8012E684u, PPCMthid4_8012e684},
    {0x8015D3FCu, DSPCheckMailToDSP_8015d3fc},
    {0x8015D40Cu, DSPCheckMailFromDSP_8015d40c},
    {0x8015D41Cu, DSPReadMailFromDSP_8015d41c},
    {0x8015D430u, DSPSendMailToDSP_8015d430},
    {0x8015D444u, DSPInit_8015d444},
    {0x8015D504u, DSPCheckInit_8015d504},
    {0x8015D50Cu, DSPAddTask_8015d50c},
    {0x8015D57Cu, DSPAssertTask_8015d57c},
    {0x8015DC60u, DSP_boot_task_8015dc60},
    {0x8015DF1Cu, DVDFSInit_8015df1c},
    {0x8015DF4Cu, DVDConvertPathToEntrynum_8015df4c},
    {0x8015E254u, DVDFastOpen_8015e254},
    {0x8015E2BCu, DVDOpen_8015e2bc},
    {0x8015E568u, DVDClose_8015e568},
    {0x8015E74Cu, DVDReadAsyncPrio_8015e74c},
    {0x8015E834u, DVDReadPrio_8015e834},
    {0x8015EA1Cu, DVDInit_8015ea1c},
    {0x801628CCu, DVDReadAbsAsyncPrio_801628cc},
    {0x801643FCu, DVDCheckDevice_801643fc},
    {0x80164848u, DVDLowInit_80164848},
    {0x80164AACu, DVDLowReadDiskID_80164aac},
    {0x80165708u, DVDLowUnencryptedRead_80165708},
    {0x80165A30u, DVDLowInquiry_80165a30},
    {0x80166330u, DVDLowRead_80166330},
    {0x80166964u, DVDLowClearCoverInterrupt_80166964},
    {0x801671D0u, ESP_InitLib_801671d0},
    {0x80167224u, ESP_CloseLib_80167224},
    {0x80167E78u, SetExiInterruptMask_80167e78},
    {0x80167F68u, EXIImm_80167f68},
    {0x80168288u, EXIDma_80168288},
    {0x80168380u, EXISync_80168380},
    {0x801689D0u, EXISelect_801689d0},
    {0x80168B00u, EXIDeselect_80168b00},
    {0x80168FA0u, EXIInit_80168fa0},
    {0x80169260u, EXIUnlock_80169260},
    {0x8016B850u, GX__Init_8016b850},
    {0x8016D3A4u, wrap_gx_SetVtxDesc_8016d3a4},
    {0x8016D608u, wrap_gx_SetVtxDescv_8016d608},
    {0x8016D9F0u, wrap_gx_GetVtxDesc_8016d9f0},
    {0x8016DBA4u, wrap_gx_GetVtxDescv_8016dba4},
    {0x8016DC34u, wrap_gx_ClearVtxDesc_8016dc34},
    {0x8016DC68u, wrap_gx_SetVtxAttrFmt_8016dc68},
    {0x8016DE08u, wrap_gx_SetVtxAttrFmtv_8016de08},
    {0x8016E04Cu, wrap_gx_GetVtxAttrFmt_8016e04c},
    {0x8016E2B8u, wrap_gx_GetVtxAttrFmtv_8016e2b8},
    {0x8016E32Cu, wrap_gx_SetArray_8016e32c},
    {0x8016E36Cu, wrap_gx_InvalidateVtxCache_8016e36c},
    {0x8016E37Cu, wrap_gx_SetTexCoordGen2_8016e37c},
    {0x8016E5A4u, wrap_gx_SetNumTexGens_8016e5a4},
    {0x8016E654u, GX__Flush_8016e654},
    {0x8016E6B0u, GX_NoOp},
    {0x8016E848u, GX_NoOp},
    {0x8016E9FCu, GX_NoOp},
    {0x8016EAB0u, wrap_gx_DrawDone_8016eab0},
    {0x8016EB70u, wrap_gx_PixModeSync_8016eb70},
    {0x8016EB94u, GX_NoOp},
    {0x8016EBA4u, GX_NoOp},
    {0x8016EBBCu, GX_NoOp},
    {0x8016EBD0u, GX_NoOp},
    {0x8016EC2Cu, GX_NoOp},
    {0x8016EC40u, GX_NoOp},
    {0x8016EC58u, GX_NoOp},
    {0x8016EC6Cu, GX_NoOp},
    {0x8016ED50u, GX__SetDrawDoneCallback_8016ed50},
    {0x8016EE78u, GX__SetDirtyState_8016ee78},
    {0x8016F0F0u, wrap_gx_Begin_8016f0f0},
    {0x8016F314u, wrap_gx_SetLineWidth_8016f314},
    {0x8016F348u, wrap_gx_SetPointSize_8016f348},
    {0x8016F37Cu, wrap_gx_EnableTexOffsets_8016f37c},
    {0x8016F3B8u, wrap_gx_SetCullMode_8016f3b8},
    {0x8016F3E0u, wrap_gx_SetCoPlanar_8016f3e0},
    {0x8016F414u, GX_NoOp},
    {0x8016F438u, GX__SetDispCopySrc_8016f438},
    {0x8016F478u, wrap_gx_SetTexCopySrc_8016f478},
    {0x8016F4B8u, GX__SetDispCopyDst_8016f4b8},
    {0x8016F4DCu, wrap_gx_SetTexCopyDst_8016f4dc},
    {0x8016F5F8u, GX_NoOp},
    {0x8016F618u, wrap_gx_SetCopyClamp_8016f618},
    {0x8016FA40u, GX__SetCopyFilter_8016fa40},
    {0x8016FC24u, GX_NoOp},
    {0x8016FC38u, GX__CopyDisp_8016fc38},
    {0x8016FD74u, wrap_gx_CopyTex_8016fd74},
    {0x8016FECCu, GX_NoOp},
    {0x8016FF04u, wrap_gx_InitLightAttn_8016ff04},
    {0x8016FF20u, wrap_gx_InitLightAttnA_8016ff20},
    {0x8016FF30u, wrap_gx_InitLightAttnK_8016ff30},
    {0x80170198u, wrap_gx_InitLightPos_80170198},
    {0x801701A8u, wrap_gx_GetLightPos_801701a8},
    {0x801701C4u, wrap_gx_InitLightDir_801701c4},
    {0x801701E0u, wrap_gx_GetLightDir_801701e0},
    {0x80170314u, wrap_gx_InitLightColor_80170314},
    {0x80170320u, wrap_gx_LoadLightObjImm_80170320},
    {0x8017039Cu, wrap_gx_SetChanAmbColor_8017039c},
    {0x80170474u, wrap_gx_SetChanMatColor_80170474},
    {0x8017054Cu, wrap_gx_SetNumChans_8017054c},
    {0x80170570u, wrap_gx_SetChanCtrl_80170570},
    {0x80170738u, wrap_gx_GetImageTileCount_80170738},
    {0x801707F8u, wrap_gx_InitTexObj_801707f8},
    {0x80170A04u, GX__InitTexObjCI_80170a04},
    {0x80170A4Cu, wrap_gx_InitTexObjLOD_80170a4c},
    {0x80170B50u, wrap_gx_InitTexObjWrapMode_80170b50},
    {0x80170B64u, wrap_gx_InitTexObjTlut_80170b64},
    {0x80170B6Cu, wrap_gx_InitTexObjFilter_80170b6c},
    {0x80170B94u, wrap_gx_InitTexObjLODBias_80170b94},
    {0x80170BE8u, wrap_gx_InitTexObjUserData_80170be8},
    {0x80170BF0u, GX__GetTexObjUserData_80170bf0},
    {0x80170BF8u, wrap_gx_GetTexObjAll_80170bf8},
    {0x80170C68u, GX__GetTexObjWidth_80170c68},
    {0x80170C7Cu, GX__GetTexObjHeight_80170c7c},
    {0x80170C90u, GX__GetTexObjFmt_80170c90},
    {0x80170C98u, GX__GetTexObjWrapS_80170c98},
    {0x80170CA4u, GX__GetTexObjWrapT_80170ca4},
    {0x80170CB0u, GX__GetTexObjMipMap_80170cb0},
    {0x80170CBCu, GX__GetTexObjLODAll_80170cbc},
    {0x80170DA0u, GX__GetTexObjMinFilt_80170da0},
    {0x80170DC0u, GX__GetTexObjTlut_80170dc0},
    {0x80170DC8u, wrap_gx_LoadTexObjPreLoaded_80170dc8},
    {0x80170F2Cu, wrap_gx_LoadTexObj_80170f2c},
    {0x80170F80u, wrap_gx_InitTlutObj_80170f80},
    {0x80170FA8u, wrap_gx_LoadTlut_80170fa8},
    {0x80171110u, wrap_gx_InvalidateTexAll_80171110},
    {0x80171158u, GX__SetTexRegionCallback_80171158},
    {0x8017116Cu, GX__SetTlutRegionCallback_8017116c},
    {0x80171180u, wrap_gx_SetTexCoordScaleManually_80171180},
    {0x801711FCu, wrap_gx_SetTexCoordBias_801711fc},
    {0x80171260u, GX_NoOp},
    {0x801712F0u, GX_NoOp},
    {0x80171458u, GX_NoOp},
    {0x801717ACu, wrap_gx_SetTevIndirect_801717ac},
    {0x80171814u, wrap_gx_SetIndTexMtx_80171814},
    {0x80171968u, wrap_gx_SetIndTexCoordScale_80171968},
    {0x80171A6Cu, wrap_gx_SetIndTexOrder_80171a6c},
    {0x80171B38u, wrap_gx_SetNumIndStages_80171b38},
    {0x80171B58u, wrap_gx_SetTevDirect_80171b58},
    {0x80171BA0u, wrap_gx_SetTevIndWarp_80171ba0},
    {0x80171BF4u, GX_NoOp},
    {0x80171BF8u, GX_NoOp},
    {0x80171C28u, GX_NoOp},
    {0x80171C4Cu, wrap_gx_SetTevOp_80171c4c},
    {0x80171CE0u, wrap_gx_SetTevColorIn_80171ce0},
    {0x80171D20u, wrap_gx_SetTevAlphaIn_80171d20},
    {0x80171D60u, wrap_gx_SetTevColorOp_80171d60},
    {0x80171DB8u, wrap_gx_SetTevAlphaOp_80171db8},
    {0x80171E10u, wrap_gx_SetTevColor_80171e10},
    {0x80171E70u, wrap_gx_SetTevColorS10_80171e70},
    {0x80171ED4u, wrap_gx_SetTevKColor_80171ed4},
    {0x80171F30u, wrap_gx_SetTevKColorSel_80171f30},
    {0x80171F80u, wrap_gx_SetTevKAlphaSel_80171f80},
    {0x80171FD0u, wrap_gx_SetTevSwapMode_80171fd0},
    {0x8017200Cu, wrap_gx_SetTevSwapModeTable_8017200c},
    {0x80172088u, wrap_gx_SetAlphaCompare_80172088},
    {0x801720C0u, wrap_gx_SetZTexture_801720c0},
    {0x8017214Cu, wrap_gx_SetTevOrder_8017214c},
    {0x801722A8u, wrap_gx_SetNumTevStages_801722a8},
    {0x801722CCu, wrap_gx_SetFog_801722cc},
    {0x801724F8u, wrap_gx_InitFogAdjTable_801724f8},
    {0x8017277Cu, wrap_gx_SetBlendMode_8017277c},
    {0x801727CCu, wrap_gx_SetColorUpdate_801727cc},
    {0x801727F8u, wrap_gx_SetAlphaUpdate_801727f8},
    {0x80172824u, wrap_gx_SetZMode_80172824},
    {0x80172858u, wrap_gx_SetZCompLoc_80172858},
    {0x80172888u, wrap_gx_SetPixelFmt_80172888},
    {0x80172930u, wrap_gx_SetDither_80172930},
    {0x8017295Cu, wrap_gx_SetDstAlpha_8017295c},
    {0x8017298Cu, wrap_gx_SetFieldMask_8017298c},
    {0x80172A30u, wrap_gx_DrawSphere_80172a30},
    {0x80172E00u, wrap_gx_BeginDisplayList_80172e00},
    {0x80172EB4u, GX__EndDisplayListCpu_80172eb4},
    {0x80172F64u, wrap_gx_CallDisplayList_80172f64},
    {0x80172FD8u, GX_NoOp},
    {0x8017301Cu, wrap_gx_SetProjection_8017301c},
    {0x80173080u, wrap_gx_SetProjectionv_80173080},
    {0x801730CCu, wrap_gx_GetProjectionv_801730cc},
    {0x8017310Cu, wrap_gx_LoadPosMtxImm_8017310c},
    {0x8017315Cu, wrap_gx_LoadPosMtxIndx_8017315c},
    {0x80173188u, wrap_gx_LoadNrmMtxImm_80173188},
    {0x801731E0u, wrap_gx_LoadNrmMtxIndx3x3_801731e0},
    {0x80173214u, wrap_gx_SetCurrentMtx_80173214},
    {0x80173234u, wrap_gx_LoadTexMtxImm_80173234},
    {0x801732E8u, GX_NoOp},
    {0x80173378u, wrap_gx_SetViewportJitter_80173378},
    {0x801733B4u, wrap_gx_SetViewport_801733b4},
    {0x801733E0u, wrap_gx_GetViewportv_801733e0},
    {0x80173400u, wrap_gx_SetZScaleOffset_80173400},
    {0x80173430u, wrap_gx_SetScissor_80173430},
    {0x80173498u, wrap_gx_GetScissor_80173498},
    {0x801734E0u, wrap_gx_SetScissorBoxOffset_801734e0},
    {0x8017351Cu, wrap_gx_SetClipMode_8017351c},
    {0x80173544u, GX_NoOp},
    {0x801735CCu, GX_NoOp},
    {0x80173DF8u, GX_NoOp},
    {0x80193478u, IPCCltInit_80193478},
    {0x801938F8u, IOS_Open_801938f8},
    {0x80193A18u, IOS_CloseAsync_80193a18},
    {0x80193AD8u, IOS_Close_80193ad8},
    {0x80193C80u, IOS_Read_80193c80},
    {0x80194158u, IOS_IoctlAsync_80194158},
    {0x80194290u, IOS_Ioctl_80194290},
    {0x8019B43Cu, NANDCreate_8019b43c},
    {0x8019B524u, NANDPrivateCreateAsync_8019b524},
    {0x8019B59Cu, NANDDelete_8019b59c},
    {0x8019B6E4u, NANDPrivateDeleteAsync_8019b6e4},
    {0x8019B7A4u, NANDRead_8019b7a4},
    {0x8019B80Cu, NANDReadAsync_8019b80c},
    {0x8019B884u, NANDWrite_8019b884},
    {0x8019B8ECu, NANDWriteAsync_8019b8ec},
    {0x8019B964u, NANDSeek_8019b964},
    {0x8019BA04u, NANDSeekAsync_8019ba04},
    {0x8019BBE0u, NANDCreateDir_8019bbe0},
    {0x8019BCC8u, NANDPrivateCreateDirAsync_8019bcc8},
    {0x8019BEE8u, NANDMove_8019bee8},
    {0x8019BF4Cu, NANDGetLength_8019bf4c},
    {0x8019C048u, NANDGetLengthAsync_8019c048},
    {0x8019C380u, NANDGetStatus_8019c380},
    {0x8019C448u, NANDPrivateGetStatusAsync_8019c448},
    {0x8019C800u, NANDOpen_8019c800},
    {0x8019C918u, NANDOpenAsync_8019c918},
    {0x8019C990u, NANDPrivateOpenAsync_8019c990},
    {0x8019CA80u, NANDClose_8019ca80},
    {0x8019CAECu, NANDCloseAsync_8019caec},
    {0x8019CB74u, NANDSafeOpen_8019cb74},
    {0x8019CF28u, NANDSafeClose_8019cf28},
    {0x8019D104u, NANDPrivateSafeOpenAsync_8019d104},
    {0x8019D720u, NANDSafeCloseAsync_8019d720},
    {0x8019E18Cu, NANDInit_8019e18c},
    {0x8019E390u, NANDGetCurrentDir_8019e390},
    {0x8019E770u, NANDGetType_8019e770},
    {0x8019E7B4u, NANDPrivateGetTypeAsync_8019e7b4},
    {0x8019EAD0u, wrap_nand_check},
    {0x8019F33Cu, OSGetConsoleType_8019f33c},
    {0x801A00E0u, OSExceptionInit_801a00e0},
    {0x801A0870u, OSSetAlarm_801a0870},
    {0x801A08E0u, OSSetPeriodicAlarm_801a08e0},
    {0x801A0964u, OSCancelAlarm_801a0964},
    {0x801A1138u, AIClockInit_801a1138},
    {0x801A1358u, OSInitAudioSystem_801a1358},
    {0x801A1520u, OSStopAudioSystem_801a1520},
    {0x801A1600u, DCInvalidateRange_801a1600},
    {0x801A162Cu, DCFlushRange_801a162c},
    {0x801A165Cu, DCStoreRange_801a165c},
    {0x801A168Cu, DCFlushRangeNoSync_801a168c},
    {0x801A16E4u, DCZeroRange_801a16e4},
    {0x801A1710u, ICInvalidateRange_801a1710},
    {0x801A1834u, LCEnable_801a1834},
    {0x801A186Cu, LCDisable_801a186c},
    {0x801A1894u, LCLoadBlocks_801a1894},
    {0x801A18B8u, LCStoreBlocks_801a18b8},
    {0x801A18DCu, LCStoreData_801a18dc},
    {0x801A197Cu, LCQueueLength_801a197c},
    {0x801A1988u, LCQueueWait_801a1988},
    {0x801A1AE4u, OSCacheInit_801a1ae4},
    {0x801A25D0u, OSReport_801a25d0},
    {0x801A2660u, OSPanic_801a2660},
    {0x801A65ACu, OSDisableInterrupts_801a65ac},
    {0x801A65C0u, OSEnableInterrupts_801a65c0},
    {0x801A65D4u, OSRestoreInterrupts_801a65d4},
    {0x801A661Cu, OSInterruptInit_801a661c},
    {0x801A72FCu, OSInitMessageQueue_801a72fc},
    {0x801A735Cu, OSSendMessage_801a735c},
    {0x801A7424u, OSReceiveMessage_801a7424},
    {0x801A7DFCu, OSInitMemoryProtection_801a7dfc},
    {0x801A7EE4u, OSLockMutex_801a7ee4},
    {0x801A7FC0u, OSUnlockMutex_801a7fc0},
    {0x801A8A50u, OSGetResetCode_801a8a50},
    {0x801A961Cu, OSThreadInit_801a961c},
    {0x801A98B0u, OSGetCurrentThread_801a98b0},
    {0x801A9E84u, OSCreateThread_801a9e84},
    {0x801AA1D4u, OSCancelThread_801aa1d4},
    {0x801AA4ECu, OSDetachThread_801aa4ec},
    {0x801AA58Cu, OSResumeThread_801aa58c},
    {0x801AA9B8u, OSSleepThread_HLE_801aa9b8},
    {0x801AAAA4u, OSWakeupThread_HLE_801aaaa4},
    {0x801AACA8u, OSSleepTicks_801aaca8},
    {0x801AAD5Cu, OSGetTime_801aad5c},
    {0x801AAD7Cu, OSGetSystemTime_801aad7c},
    {0x801AB75Cu, OSSetPowerCallback_801ab75c},
    {0x801AB848u, OSInitSTM_801ab848},
    {0x801AD1D4u, OSCheckInstall_801ad1d4},
    {0x801AE4A0u, OSIsTitleInstalled_801ae4a0},
    {0x801AF0DCu, PADReset_801af0dc},
    {0x801AF2F0u, PADInit_801af2f0},
    {0x801AF908u, PADControlMotor_801af908},
    {0x801B0220u, SCCheckStatus_801b0220},
    {0x801B1BE4u, SCGetAspectRatio_801b1be4},
    {0x801B1CACu, SCGetEuRgb60Mode_801b1cac},
    {0x801B1D0Cu, SCGetLanguage_801b1d0c},
    {0x801B23A0u, SCGetProductArea_801b23a0},
    {0x801B2DE0u, SIInit_801b2de0},
    {0x801B90F4u, VISetPreRetraceCallback_801b90f4},
    {0x801B9138u, VISetPostRetraceCallback_801b9138},
    {0x801B94A4u, VIInit_801b94a4},
    {0x801B99ECu, VIWaitForRetrace_801b99ec},
    {0x801B9F6Cu, VIConfigure_801b9f6c},
    {0x801BA9A4u, VIFlush_801ba9a4},
    {0x801BAB2Cu, VISetBlack_801bab2c},
    {0x801BABA4u, VIGetRetraceCount_801baba4},
    {0x801BAD38u, VIGetDTVStatus_801bad38},
    {0x801BF5C4u, WPADInit_801bf5c4},
    {0x801BF640u, WPADSetSyncDeviceCallback_801bf640},
    {0x801BF64Cu, WPADGetStatus_801bf64c},
    {0x801C0EC4u, WPADControlMotor_801c0ec4},
    {0x801C329Cu, WPADGetDpdSensitivity_801c329c},
    {0x8020FF9Cu, EGG__AsyncDisplay__endRender_HLE_8020ff9c},
    {0x80218C2Cu, EGG_Decomp_decodeSZS_80218c2c},
    {0x8021EEC0u, DrawPathDOF_DrawImpl_8021eec0},
    {0x80226C04u, ExpHeap_Alloc_80226c04},
    {0x80226C78u, ExpHeap_Free_80226c78},
    {0x8052C7E4u, NandMgrDeleteRKSYS_8052c7e4},
    {0x8055531Cu, RelProlog_8055531c},
    {0x80672CC8u, NWC24OfflineSaveTask_80672cc8},
};

} // namespace

void register_hle_natives() {
    const std::size_t count = sizeof(g_natives) / sizeof(g_natives[0]);
    for (std::size_t i = 1; i < count; ++i) {
        if (g_natives[i].address <= g_natives[i - 1].address) {
            std::printf("mkwv: native table unsorted at 0x%08X\n", g_natives[i].address);
        }
    }
    std::printf("mkwv: %u natives, %u extra translated functions\n", static_cast<unsigned>(count),
                extra_native_count());
}

// The decoder's window onto guest memory, for display lists and indexed matrix loads.
// A range inside one CompactMemory page is returned in place. One that crosses a 1 MB
// page cannot be - lookup() refuses it even though both pages are valid - so it is staged
// into a contiguous buffer through CompactMemory::read, which walks pages itself. Each
// nesting level gets its own buffer, because a display list can call another while its
// own bytes are still being decoded; indexed loads use the levels above the display-list
// ones for the same reason.
// Why the resolver last refused: 1 size 0, 2 nested too deep, 3 larger than a staging
// buffer, 4 no guest memory bound, 5 CompactMemory::read failed.
std::uint32_t g_resolve_why = 0;
std::uint32_t g_resolve_addr = 0;
std::uint32_t g_resolve_size = 0;

const std::uint8_t* resolve_fail(std::uint32_t why, std::uint32_t addr, std::uint32_t size) {
    g_resolve_why = why;
    g_resolve_addr = addr;
    g_resolve_size = size;
    return nullptr;
}

const std::uint8_t* resolve_guest_for_gx(std::uint32_t addr, std::uint32_t size, unsigned depth) {
    if (size == 0) {
        return resolve_fail(1, addr, size);
    }
    if (const std::uint8_t* p = Memory::GetPointer(addr, size)) {
        return p;
    }
    constexpr unsigned kLevels = 8;
    constexpr std::uint32_t kStage = 128u * 1024u;
    static std::uint8_t stage[kLevels][kStage];
    if (depth >= kLevels) {
        return resolve_fail(2, addr, size);
    }
    if (size > kStage) {
        return resolve_fail(3, addr, size);
    }
    mkwv::runtime::CompactMemory* memory = mkwv::runtime::guest_memory();
    if (memory == nullptr) {
        return resolve_fail(4, addr, size);
    }
    if (!memory->read(addr, stage[depth], size)) {
        return resolve_fail(5, addr, size);
    }
    return stage[depth];
}

// Vertex arrays: the 1 MiB page holding `addr`, when the page is backed by one host buffer
// (the fast page table only marks such pages). The decoder indexes elements inside it.
const std::uint8_t* resolve_window_for_gx(std::uint32_t addr, std::uint32_t* base, std::uint32_t* size) {
    const std::uint32_t page = addr & ~0xFFFFFu;
    if (const std::uint8_t* p = guest_fast_ptr(page, 0x100000u)) {
        *base = page;
        *size = 0x100000u;
        return p;
    }
    return nullptr;
}

// Texture images and TLUT sources: MEM1 and MEM2 are each one contiguous host allocation,
// so the pointer for the first byte covers the whole image.
const std::uint8_t* resolve_direct_for_gx(std::uint32_t addr) {
    return Memory::GetPointer(addr, 1);
}

// The FIFO buffer filled mid-frame: decode its complete commands into the live Processor now,
// as the GP would have been doing all along, and let the buffer take the rest of the frame.
std::uint32_t g_fifo_midframe_drains = 0;
std::uint32_t g_fifo_midframe_refused = 0;

std::uint32_t hle_fifo_full(gx::Fifo& fifo, const std::uint8_t* data, std::uint32_t size) {
    if (g_processor == nullptr || g_fifo == nullptr || &fifo != &g_fifo->gx() || hle_gx_in_display_list()) {
        ++g_fifo_midframe_refused;
        return 0;
    }
    ++g_fifo_midframe_drains;
    perf::count(perf::kMidDrains);
    if (g_offload != nullptr) {
        // The render thread keeps any partial command at the end; the whole buffer goes.
        g_offload->chunk(data, size);
        return size;
    }
    perf::Scope timing(perf::kDecode);
    return gx::process_prefix(*g_processor, data, size, true);
}

void unbind_hle_natives() {
    gx::set_fifo_full_handler(nullptr);
    g_fifo = nullptr;
    g_processor = nullptr;
    g_present = nullptr;
    gx::set_guest_resolver(nullptr);
    gx::set_direct_resolver(nullptr);
    gx::set_window_resolver(nullptr);
}

// Game functions the base translation leaves out of the shards (upstream implements them
// natively on PC) whose original code is what we want here. The build compiles their
// translated sources (scripts/extract_translated.py) and each registers itself before
// main(), so the runtime needs no link-time reference to them.
namespace {
struct ExtraNative {
    std::uint32_t address;
    TranslatedFn fn;
};
ExtraNative g_extra_natives[64];
unsigned g_extra_native_count = 0;

TranslatedFn find_extra_native(std::uint32_t address) {
    for (unsigned i = 0; i < g_extra_native_count; ++i) {
        if (g_extra_natives[i].address == address) {
            return g_extra_natives[i].fn;
        }
    }
    return nullptr;
}
} // namespace

extern "C" void mkwv_register_extra_native(std::uint32_t address, TranslatedFn fn) {
    if (fn != nullptr && g_extra_native_count < sizeof(g_extra_natives) / sizeof(g_extra_natives[0])) {
        g_extra_natives[g_extra_native_count++] = {address, fn};
    }
}

unsigned extra_native_count() {
    return g_extra_native_count;
}

TranslatedFn find_hle_native(std::uint32_t address) {
    std::size_t lo = 0;
    std::size_t hi = sizeof(g_natives) / sizeof(g_natives[0]);
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (g_natives[mid].address < address) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < sizeof(g_natives) / sizeof(g_natives[0]) && g_natives[lo].address == address) {
        return g_natives[lo].fn;
    }
    return find_extra_native(address);
}

void bind_hle_frame(HleFifo* fifo, gx::Processor* processor, HlePresentFn present) {
    g_fifo = fifo;
    g_processor = processor;
    g_present = present;
    g_frames = 0;
    gx::set_guest_resolver(&resolve_guest_for_gx);
    gx::set_direct_resolver(&resolve_direct_for_gx);
    gx::set_window_resolver(&resolve_window_for_gx);
    gx::set_fifo_full_handler(&hle_fifo_full);
}

void hle_frame_stats(std::uint32_t* frames, std::uint32_t* draws, std::uint32_t* verts, std::uint32_t* fifo,
                     std::uint32_t* max_draws, std::uint32_t* skipped) {
    *skipped = g_skipped;
    *frames = g_frames;
    *draws = g_last_draws;
    *verts = g_last_verts;
    *fifo = g_last_fifo;
    *max_draws = g_max_draws;
}

const char* hle_take_draw_dump() {
    if (!g_draw_dump_ready) {
        return nullptr;
    }
    g_draw_dump_ready = false;
    return g_draw_dump;
}

// The whole guest command stream for one frame, so the GX programming can be decoded by hand
// instead of inferred from this runtime's own decode of it.
std::uint32_t hle_fifo_bytes(const std::uint8_t** out) {
    *out = g_fifo_bytes;
    return g_fifo_bytes_len;
}

void unbind_hle_frame() {
    unbind_hle_natives();
}


void build_draw_dump(const gx::Processor& pr, const std::uint8_t* bytes, std::uint32_t size) {
    int n = std::snprintf(g_draw_dump, sizeof(g_draw_dump),
                          "mkwv: DRAW frame=%u draws=%u verts=%u rvp=%u[%.0f %.0f %.0f %.0f %.2f %.2f] scis=%u/%u[%d %d %d %d] proj00=%.4f proj11=%.4f proj03=%.4f proj13=%.4f "
                          "proj22=%.4f proj23=%.4f ptype=%d depth=%d/%d cull=%d color_upd=%d tex0=%ux%u fmt=%u data=%d",
                          g_frames, pr.draw_count, pr.vert_count, pr.state.has_render_vp ? 1u : 0u,
                          pr.state.render_vp[0], pr.state.render_vp[1], pr.state.render_vp[2], pr.state.render_vp[3],
                          pr.state.render_vp[4], pr.state.render_vp[5], pr.state.has_render_scissor ? 1u : 0u,
                          pr.state.has_scissor ? 1u : 0u, pr.state.scissor_x, pr.state.scissor_y, pr.state.scissor_w,
                          pr.state.scissor_h,
                          pr.state.proj[0][0], pr.state.proj[1][1], pr.state.proj[0][3], pr.state.proj[1][3],
                          pr.state.proj[2][2], pr.state.proj[2][3], static_cast<int>(pr.state.proj_type),
                          pr.state.depth_test ? 1 : 0, pr.state.depth_write ? 1 : 0, static_cast<int>(pr.state.cull_mode),
                          pr.state.color_update ? 1 : 0, pr.state.texmaps[0].width, pr.state.texmaps[0].height,
                          pr.state.texmaps[0].format, pr.state.texmaps[0].data != nullptr ? 1 : 0);
    n += std::snprintf(g_draw_dump + n, sizeof(g_draw_dump) - static_cast<std::size_t>(n),
                       " desc(pos=%u clr0=%u tex0=%u) ntexgen=%u tg0(t=%u src=%u mtx=%u pmtx=%u nrm=%u) nchan=%u "
                       "ch0(lit=%u mat=%u amb=%u) mat0=%08X amb0=%08X tevcol=%08X/%08X/%08X konst0=%08X alpha(op=%u r0=%u c0=%u)",
                       pr.state.vtx_desc[gx::AttrPos], pr.state.vtx_desc[gx::AttrClr0], pr.state.vtx_desc[gx::AttrTex0],
                       pr.state.num_tex_gens, static_cast<unsigned>(pr.state.texgens[0].type),
                       static_cast<unsigned>(pr.state.texgens[0].src), pr.state.texgens[0].mtx,
                       pr.state.texgens[0].post_mtx, pr.state.texgens[0].normalize ? 1u : 0u, pr.state.num_chans,
                       pr.state.chans[0].lighting ? 1u : 0u, static_cast<unsigned>(pr.state.chans[0].mat_src),
                       static_cast<unsigned>(pr.state.chans[0].amb_src), pr.state.chan_mat[0], pr.state.ambient[0],
                       pr.state.tev_color[0], pr.state.tev_color[1], pr.state.tev_color[2], pr.state.konst[0],
                       pr.state.alpha_op, pr.state.alpha_ref0, static_cast<unsigned>(pr.state.alpha_comp0));
    // The modelview actually used, plus the decode-skip counters. Geometry landing outside NDC is
    // almost always either an identity pn_mtx (the matrix never reached XF) or indexed XF loads being
    // dropped, and skipped_indexed_xf is the counter that distinguishes those two.
    const unsigned pn = pr.state.current_pn_mtx < gx::kMaxPnMtx ? pr.state.current_pn_mtx : 0u;
    const float* mv = pr.state.pn_mtx[pn];
    bool ident = true;
    for (unsigned r = 0; r < 3u && ident; ++r) {
        for (unsigned c = 0; c < 4u; ++c) {
            const float want = (r == c) ? 1.0f : 0.0f;
            const float d = mv[r * 4u + c] - want;
            if (d > 0.0001f || d < -0.0001f) {
                ident = false;
                break;
            }
        }
    }
    // Where Processor::process() gave up, if it did. err= alone only says it happened; without the
    // site and offset the rest of the frame's FIFO is silently truncated with no clue why.
    std::uint32_t err_site = 0, err_cmd = 0, err_offset = 0, err_size = 0, err_depth = 0;
    gx::last_fifo_error(&err_site, &err_cmd, &err_offset, &err_size, &err_depth);
    n += std::snprintf(g_draw_dump + n, sizeof(g_draw_dump) - static_cast<std::size_t>(n),
                       " pn=%u ident=%d mv=[%.3f %.3f %.3f %.1f | %.3f %.3f %.3f %.1f | %.3f %.3f %.3f %.1f] "
                       "projraw=[%.4f %.4f %.4f %.4f %.4f %.4f] skip(ixf=%u dl=%u aur=%u err=%u)"
                       " firsterr(site=%u cmd=%02X off=%u/%u depth=%u)",
                       pn, ident ? 1 : 0, mv[0], mv[1], mv[2], mv[3], mv[4], mv[5], mv[6], mv[7], mv[8], mv[9], mv[10],
                       mv[11], pr.state.xf_proj_raw[0], pr.state.xf_proj_raw[1], pr.state.xf_proj_raw[2],
                       pr.state.xf_proj_raw[3], pr.state.xf_proj_raw[4], pr.state.xf_proj_raw[5],
                       pr.skipped_indexed_xf, pr.skipped_dl, pr.skipped_aurora, pr.error_count, err_site, err_cmd,
                       err_offset, err_size, err_depth);
    // The dump only ever shows the first three of a hundred-odd draws, which says nothing about
    // whether the rest could draw anything at all. Classify the whole frame: a draw whose TEV
    // resolves to the rasterised colour with the ras channel set to zero outputs black with zero
    // alpha, and under src-alpha blending that writes nothing - it is submitted successfully and is
    // invisible by construction. "vis" counts draws that are not in that state.
    std::uint32_t n_passclr = 0, n_raszero = 0, n_tex = 0, n_vis = 0, n_alpha0 = 0, n_uvzero = 0;
    for (std::uint32_t d = 0; d < pr.draw_count; ++d) {
        const gx::DrawCall& dc = pr.draws[d];
        const bool passclr = dc.tev == gx::TevPassClr;
        const bool raszero = dc.tev_stages[0].ras_chan == gx::kRasZero;
        if (passclr) ++n_passclr;
        if (raszero) ++n_raszero;
        if (dc.tex0) ++n_tex;
        bool any_alpha = false;
        for (std::uint32_t v = dc.first_vertex; v < dc.first_vertex + dc.vertex_count && v < gx::kMaxDecodedVerts;
             ++v) {
            if ((pr.verts[v].color >> 24) != 0u) {
                any_alpha = true;
                break;
            }
        }
        if (!any_alpha) ++n_alpha0;
        if (!(passclr && raszero) && any_alpha) ++n_vis;
        if (dc.tex0) {
            bool any_uv = false;
            for (std::uint32_t v = dc.first_vertex; v < dc.first_vertex + dc.vertex_count && v < gx::kMaxDecodedVerts;
                 ++v) {
                if (pr.verts[v].tc[0][0] != 0.0f || pr.verts[v].tc[0][1] != 0.0f) {
                    any_uv = true;
                    break;
                }
            }
            if (!any_uv) ++n_uvzero;
        }
    }
    n += std::snprintf(g_draw_dump + n, sizeof(g_draw_dump) - static_cast<std::size_t>(n),
                       " tally(passclr=%u raszero=%u tex=%u alpha0=%u uvzero=%u vis=%u/%u)", n_passclr, n_raszero,
                       n_tex, n_alpha0, n_uvzero, n_vis, pr.draw_count);
    // uvzero=27 of 245 textured draws, and the panel quad is one of them: all four of its
    // corners come out (0, 0), so it samples a single texel. apply_texgen multiplies the
    // raw texcoord by state.xf_mem[gen.mtx * 4] - GX_TEXMTX0 is XF slot 30, and the ten
    // matrices are contiguous 12-float blocks from there. An unloaded (all-zero) matrix
    // sends every texcoord to the origin, which is exactly the symptom. This says which
    // of the ten were actually written, so it stops being a guess.
    std::uint32_t texmtx_zero = 0, texmtx_ident = 0;
    for (unsigned m = 0; m < 10u; ++m) {
        const unsigned base = (30u + m * 3u) * 4u;
        if (base + 12u > gx::kXfMemSize) {
            break;
        }
        bool all_zero = true, is_ident = true;
        for (unsigned k = 0; k < 12u; ++k) {
            const float f = pr.state.xf_mem[base + k];
            if (f != 0.0f) {
                all_zero = false;
            }
            const float want = (k / 4u) == (k % 4u) ? 1.0f : 0.0f;
            const float d = f - want;
            if (d > 0.0001f || d < -0.0001f) {
                is_ident = false;
            }
        }
        if (all_zero) {
            texmtx_zero |= 1u << m;
        }
        if (is_ident) {
            texmtx_ident |= 1u << m;
        }
    }
    std::uint32_t xf_pn = 0, xf_tex = 0, xf_ident = 0, xf_nrm = 0, xf_dtt = 0, xf_light = 0, xf_reg = 0,
                  xf_drop = 0, xf_tex_addr = 0, xf_tex_count = 0;
    gx::xf_load_stats(&xf_pn, &xf_tex, &xf_ident, &xf_nrm, &xf_dtt, &xf_light, &xf_reg, &xf_drop, &xf_tex_addr,
                      &xf_tex_count);
    n += std::snprintf(g_draw_dump + n, sizeof(g_draw_dump) - static_cast<std::size_t>(n),
                       " orphancopies=%u xf(pn=%u tex=%u ident=%u nrm=%u dtt=%u lit=%u reg=%u drop=%u first_tex=%u/%u)",
                       g_orphan_copy_drains, xf_pn,
                       xf_tex, xf_ident, xf_nrm, xf_dtt, xf_light, xf_reg, xf_drop, xf_tex_addr, xf_tex_count);
    {
        const gx::GuestFetchFailure ff = gx::last_guest_fetch_failure();
        n += std::snprintf(g_draw_dump + n, sizeof(g_draw_dump) - static_cast<std::size_t>(n),
                           " fetchfail(dl=%08X/%u d%u ixf=k%u base=%08X stride=%u idx=%u dst=%03X/%u why=%u"
                           " resolver=%u@%08X/%u)",
                           ff.dl_addr, ff.dl_bytes, ff.dl_depth, ff.ixf_kind, ff.ixf_base, ff.ixf_stride,
                           ff.ixf_index, ff.ixf_words >> 8, ff.ixf_words & 0xFFu, ff.ixf_why, g_resolve_why,
                           g_resolve_addr, g_resolve_size);
    }
    const unsigned tg_mtx = pr.state.texgens[0].mtx;
    const unsigned tg_base = static_cast<unsigned>(tg_mtx) * 4u;
    const bool tg_in_range = tg_base + 12u <= gx::kXfMemSize;
    n += std::snprintf(g_draw_dump + n, sizeof(g_draw_dump) - static_cast<std::size_t>(n),
                       " texmtx(zero=%03X ident=%03X) tg0mtx=%u tm=[%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
                       texmtx_zero, texmtx_ident, tg_mtx,
                       tg_in_range ? pr.state.xf_mem[tg_base + 0] : 0.0f,
                       tg_in_range ? pr.state.xf_mem[tg_base + 1] : 0.0f,
                       tg_in_range ? pr.state.xf_mem[tg_base + 2] : 0.0f,
                       tg_in_range ? pr.state.xf_mem[tg_base + 3] : 0.0f,
                       tg_in_range ? pr.state.xf_mem[tg_base + 4] : 0.0f,
                       tg_in_range ? pr.state.xf_mem[tg_base + 5] : 0.0f,
                       tg_in_range ? pr.state.xf_mem[tg_base + 6] : 0.0f,
                       tg_in_range ? pr.state.xf_mem[tg_base + 7] : 0.0f);

    // The renderer still collapses the whole TEV to one fixed-function stage
    // (GL_MODULATE or GL_REPLACE picked from DrawCall::tev). That approximation has now been
    // blamed twice without ever being measured, so classify every draw in the frame by its
    // stage-0 combiner signature and report the hottest ones. Anything whose signature is not
    // "d = TEXC modulated by RASC" is a draw the renderer is guessing at, and the counts say
    // whether a GL_COMBINE extension covers it or a real shader path is required.
    struct TevSig {
        std::uint64_t key;
        std::uint32_t count;
        std::uint32_t rep;
    };
    TevSig sigs[16]{};
    std::uint32_t n_sigs = 0, n_sig_over = 0;
    std::uint32_t n_multi = 0, n_konst = 0, n_prev = 0, n_notex = 0;
    std::uint32_t tevop_hist[6]{};
    // The white panel covers most of the screen, so the draw with the largest NDC footprint is
    // almost certainly it. Describing that one draw completely beats describing d0..d2, which are
    // whatever the guest happened to emit first.
    std::uint32_t big_draw = 0;
    float big_area = -1.0f;
    for (std::uint32_t d = 0; d < pr.draw_count; ++d) {
        const gx::DrawCall& dc = pr.draws[d];
        if (dc.tev < 6u) {
            ++tevop_hist[dc.tev];
        }
        if (dc.num_tev_stages > 1u) {
            ++n_multi;
        }
        // Stage 0 is a PASSCLR dummy on every draw this game emits - keying the
        // histogram on it reported three signatures for a frame of 262 draws and said
        // nothing at all about the material. Key on the stage that actually produces
        // the colour, which is the last one.
        const unsigned eff = (dc.num_tev_stages == 0 ? 1u
                              : (dc.num_tev_stages < gx::kTevStages ? dc.num_tev_stages : gx::kTevStages)) - 1u;
        const gx::TevStage& s0 = dc.tev_stages[eff];
        if (!s0.tex_enabled) {
            ++n_notex;
        }
        bool uses_konst = false, uses_prev = false;
        const unsigned ns = dc.num_tev_stages < gx::kTevStages ? dc.num_tev_stages : gx::kTevStages;
        for (unsigned i = 0; i < ns; ++i) {
            const gx::TevStage& t = dc.tev_stages[i];
            const std::uint8_t ca[4] = {t.color_a, t.color_b, t.color_c, t.color_d};
            const std::uint8_t aa[4] = {t.alpha_a, t.alpha_b, t.alpha_c, t.alpha_d};
            for (unsigned k = 0; k < 4u; ++k) {
                if (ca[k] == gx::CcKonst || aa[k] == gx::CaKonst) {
                    uses_konst = true;
                }
                if (i > 0u && (ca[k] == gx::CcCPrev || ca[k] == gx::CcAPrev || aa[k] == gx::CaAPrev)) {
                    uses_prev = true;
                }
            }
        }
        if (uses_konst) {
            ++n_konst;
        }
        if (uses_prev) {
            ++n_prev;
        }
        const std::uint64_t key = static_cast<std::uint64_t>(dc.num_tev_stages & 0xFu) |
                                  (static_cast<std::uint64_t>(s0.color_a & 0xFu) << 4) |
                                  (static_cast<std::uint64_t>(s0.color_b & 0xFu) << 8) |
                                  (static_cast<std::uint64_t>(s0.color_c & 0xFu) << 12) |
                                  (static_cast<std::uint64_t>(s0.color_d & 0xFu) << 16) |
                                  (static_cast<std::uint64_t>(s0.alpha_a & 0x7u) << 20) |
                                  (static_cast<std::uint64_t>(s0.alpha_b & 0x7u) << 23) |
                                  (static_cast<std::uint64_t>(s0.alpha_c & 0x7u) << 26) |
                                  (static_cast<std::uint64_t>(s0.alpha_d & 0x7u) << 29) |
                                  (static_cast<std::uint64_t>(s0.ras_chan & 0x7u) << 32) |
                                  (static_cast<std::uint64_t>(s0.tex_enabled ? 1u : 0u) << 35) |
                                  (static_cast<std::uint64_t>(s0.color_op & 0xFu) << 36) |
                                  (static_cast<std::uint64_t>(s0.alpha_op & 0xFu) << 40) |
                                  (static_cast<std::uint64_t>(s0.kcsel & 0x1Fu) << 44) |
                                  (static_cast<std::uint64_t>(s0.kasel & 0x1Fu) << 49) |
                                  (static_cast<std::uint64_t>(dc.tev & 0x7u) << 54) |
                                  (static_cast<std::uint64_t>(uses_konst ? 1u : 0u) << 57) |
                                  (static_cast<std::uint64_t>(dc.tex0 ? 1u : 0u) << 58);
        std::uint32_t slot = 0;
        for (; slot < n_sigs; ++slot) {
            if (sigs[slot].key == key) {
                break;
            }
        }
        if (slot == n_sigs) {
            if (n_sigs < 16u) {
                sigs[n_sigs].key = key;
                sigs[n_sigs].count = 0;
                sigs[n_sigs].rep = d;
                ++n_sigs;
            } else {
                ++n_sig_over;
                slot = 0xFFFFFFFFu;
            }
        }
        if (slot != 0xFFFFFFFFu) {
            ++sigs[slot].count;
        }
        float min_x = 1.0e30f, max_x = -1.0e30f, min_y = 1.0e30f, max_y = -1.0e30f;
        for (std::uint32_t v = dc.first_vertex; v < dc.first_vertex + dc.vertex_count && v < gx::kMaxDecodedVerts;
             ++v) {
            const gx::DecodedVertex& dv = pr.verts[v];
            if (dv.x < min_x) min_x = dv.x;
            if (dv.x > max_x) max_x = dv.x;
            if (dv.y < min_y) min_y = dv.y;
            if (dv.y > max_y) max_y = dv.y;
        }
        // Only draws the renderer issues. The first run of this picked a quad that
        // tev_output_is_null_ras drops, so it described something that was never on
        // screen - exactly the draw we already knew about.
        const bool dropped = dc.tev == gx::TevPassClr && dc.tev_stages[0].ras_chan == gx::kRasZero;
        if (dc.vertex_count != 0 && !dropped) {
            const float area = (max_x - min_x) * (max_y - min_y);
            if (area > big_area) {
                big_area = area;
                big_draw = d;
            }
        }
    }
    // Hottest signature first; 16 slots sort in a handful of swaps.
    for (std::uint32_t i = 1; i < n_sigs; ++i) {
        const TevSig cur = sigs[i];
        std::uint32_t j = i;
        while (j > 0 && sigs[j - 1].count < cur.count) {
            sigs[j] = sigs[j - 1];
            --j;
        }
        sigs[j] = cur;
    }
    n += std::snprintf(g_draw_dump + n, sizeof(g_draw_dump) - static_cast<std::size_t>(n),
                       " tev(mod=%u dec=%u bln=%u rep=%u pas=%u oth=%u multi=%u konst=%u prev=%u notex=%u "
                       "distinct=%u+%u)",
                       tevop_hist[0], tevop_hist[1], tevop_hist[2], tevop_hist[3], tevop_hist[4], tevop_hist[5],
                       n_multi, n_konst, n_prev, n_notex, n_sigs, n_sig_over);
    const std::uint32_t nsig = n_sigs < 5u ? n_sigs : 5u;
    for (std::uint32_t i = 0; i < nsig && n > 0 && n < 3400; ++i) {
        const gx::DrawCall& dc = pr.draws[sigs[i].rep];
        // Read the SAME stage the key and the notex counter use. Reading stage 0 here
        // while keying on the effective stage is how the last round reported te=1 in
        // every bucket and notex=247 in the same line.
        const unsigned eff = (dc.num_tev_stages == 0 ? 1u
                              : (dc.num_tev_stages < gx::kTevStages ? dc.num_tev_stages : gx::kTevStages)) - 1u;
        const gx::TevStage& s0 = dc.tev_stages[eff];
        n += std::snprintf(g_draw_dump + n, sizeof(g_draw_dump) - static_cast<std::size_t>(n),
                           " |s%u x%u op=%u n=%u e=%u c=%u,%u,%u,%u a=%u,%u,%u,%u cop=%u/%u ras=%u te=%u map=%u k=%u/%u kn=%u",
                           i, sigs[i].count, static_cast<unsigned>(dc.tev),
                           static_cast<unsigned>(dc.num_tev_stages), eff, s0.color_a, s0.color_b, s0.color_c,
                           s0.color_d, s0.alpha_a, s0.alpha_b, s0.alpha_c, s0.alpha_d, s0.color_op, s0.alpha_op,
                           s0.ras_chan, s0.tex_enabled ? 1u : 0u, static_cast<unsigned>(s0.texmap), s0.kcsel,
                           s0.kasel, (sigs[i].key >> 57) & 1u ? 1u : 0u);
    }
    // The biggest draw in the frame, described in full - this is the one that paints the panel.
    if (pr.draw_count != 0 && n > 0 && n < 3700) {
        const gx::DrawCall& dc = pr.draws[big_draw];
        const unsigned eff = (dc.num_tev_stages == 0 ? 1u
                              : (dc.num_tev_stages < gx::kTevStages ? dc.num_tev_stages : gx::kTevStages)) - 1u;
        const gx::TevStage& s0 = dc.tev_stages[eff];
        n += std::snprintf(g_draw_dump + n, sizeof(g_draw_dump) - static_cast<std::size_t>(n),
                           " || BIG d%u area=%.3f prim=%d n=%u tex0=%d map=%u te=%d op=%u stages=%u "
                           "c=%u,%u,%u,%u a=%u,%u,%u,%u ras=%u k=%u/%u kon=%08X/%08X tevreg=%08X/%08X/%08X "
                           "blend=%d/%d/%d alpha=%u/%u/%u",
                           big_draw, static_cast<double>(big_area), static_cast<int>(dc.prim),
                           static_cast<unsigned>(dc.vertex_count), dc.tex0 ? 1 : 0,
                           static_cast<unsigned>(s0.texmap), s0.tex_enabled ? 1 : 0, static_cast<unsigned>(dc.tev),
                           static_cast<unsigned>(dc.num_tev_stages), s0.color_a, s0.color_b, s0.color_c, s0.color_d,
                           s0.alpha_a, s0.alpha_b, s0.alpha_c, s0.alpha_d, s0.ras_chan, s0.kcsel, s0.kasel,
                           dc.konst[0], dc.konst[1], dc.tev_color[0], dc.tev_color[1], dc.tev_color[2],
                           static_cast<int>(dc.blend_mode), static_cast<int>(dc.blend_src),
                           static_cast<int>(dc.blend_dst), dc.alpha_op, static_cast<unsigned>(dc.alpha_comp0),
                           dc.alpha_ref0);
        const unsigned nst = dc.num_tev_stages == 0
                                 ? 1u
                                 : (dc.num_tev_stages < gx::kTevStages ? dc.num_tev_stages : gx::kTevStages);
        for (unsigned st = 0; st < nst && n > 0 && n < 4100; ++st) {
            const gx::TevStage& ts = dc.tev_stages[st];
            n += std::snprintf(g_draw_dump + n, sizeof(g_draw_dump) - static_cast<std::size_t>(n),
                               " st%u(c=%u,%u,%u,%u a=%u,%u,%u,%u op=%u/%u bias=%u/%u sh=%u/%u dst=%u/%u "
                               "ras=%u map=%u coord=%u te=%d)",
                               st, ts.color_a, ts.color_b, ts.color_c, ts.color_d, ts.alpha_a, ts.alpha_b, ts.alpha_c,
                               ts.alpha_d, ts.color_op, ts.alpha_op, ts.color_bias, ts.alpha_bias, ts.color_shift,
                               ts.alpha_shift, ts.color_dest, ts.alpha_dest, ts.ras_chan,
                               static_cast<unsigned>(ts.texmap), static_cast<unsigned>(ts.texcoord),
                               ts.tex_enabled ? 1 : 0);
        }
        const gx::DrawExtra* bx = (n > 0 && n < 4300) ? gx::draw_extra(pr, big_draw) : nullptr;
        if (bx != nullptr) {
            const gx::TexObj& bt = bx->tex[s0.texmap < gx::kMaxTexMaps ? s0.texmap : 0];
            n += std::snprintf(g_draw_dump + n, sizeof(g_draw_dump) - static_cast<std::size_t>(n),
                               " btex=%ux%u f=%u tg0m=%u[%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]", bt.width,
                               bt.height, bt.format, bx->tg0_mtx, bx->tg0_rows[0], bx->tg0_rows[1], bx->tg0_rows[2],
                               bx->tg0_rows[3], bx->tg0_rows[4], bx->tg0_rows[5], bx->tg0_rows[6], bx->tg0_rows[7]);
        }
        const std::uint32_t nv = dc.vertex_count < 4u ? dc.vertex_count : 4u;
        for (std::uint32_t v = 0; v < nv && n > 0 && n < 4300; ++v) {
            const gx::DecodedVertex& dv = pr.verts[dc.first_vertex + v];
            n += std::snprintf(g_draw_dump + n, sizeof(g_draw_dump) - static_cast<std::size_t>(n),
                               " v(%.2f,%.2f,%.2f,%.2f uv=%.2f,%.2f c=%08X)", dv.x, dv.y, dv.z, dv.w, dv.tc[0][0], dv.tc[0][1], dv.color);
        }
    }
    // snprintf returns what it WOULD have written, so a truncated write leaves n past the end and
    // the next (sizeof - n) underflows to a huge size_t. Clamp once before the per-draw section.
    if (n < 0 || static_cast<std::size_t>(n) >= sizeof(g_draw_dump)) {
        n = static_cast<int>(sizeof(g_draw_dump)) - 1;
    }
    const std::uint32_t nd = pr.draw_count < 3u ? pr.draw_count : 3u;
    for (std::uint32_t d = 0; d < nd && n > 0 && n < 4000; ++d) {
        const gx::DrawCall& dc = pr.draws[d];
        n += std::snprintf(g_draw_dump + n, sizeof(g_draw_dump) - static_cast<std::size_t>(n),
                           " || d%u prim=%d n=%u tex0=%d blend=%d/%d/%d stages=%u s0(c=%u%u%u%u a=%u%u%u%u ras=%u map=%u tex=%d)",
                           d, static_cast<int>(dc.prim), static_cast<unsigned>(dc.vertex_count), dc.tex0 ? 1 : 0,
                           static_cast<int>(dc.blend_mode), static_cast<int>(dc.blend_src), static_cast<int>(dc.blend_dst),
                           static_cast<unsigned>(dc.num_tev_stages), dc.tev_stages[0].color_a, dc.tev_stages[0].color_b,
                           dc.tev_stages[0].color_c, dc.tev_stages[0].color_d, dc.tev_stages[0].alpha_a,
                           dc.tev_stages[0].alpha_b, dc.tev_stages[0].alpha_c, dc.tev_stages[0].alpha_d,
                           dc.tev_stages[0].ras_chan, dc.tev_stages[0].texmap, dc.tev_stages[0].tex_enabled ? 1 : 0);
        const std::uint32_t nv = dc.vertex_count < 4u ? dc.vertex_count : 4u;
        for (std::uint32_t v = 0; v < nv && n > 0 && n < 4300; ++v) {
            const gx::DecodedVertex& dv = pr.verts[dc.first_vertex + v];
            n += std::snprintf(g_draw_dump + n, sizeof(g_draw_dump) - static_cast<std::size_t>(n),
                               " v(%.2f,%.2f,%.2f,%.2f uv=%.2f,%.2f c=%08X)", dv.x, dv.y, dv.z, dv.w, dv.tc[0][0], dv.tc[0][1], dv.color);
        }
    }
    g_fifo_bytes_len = size;
    if (g_fifo_bytes_len > sizeof(g_fifo_bytes)) {
        g_fifo_bytes_len = sizeof(g_fifo_bytes);
    }
    if (bytes != nullptr) {
        std::memcpy(g_fifo_bytes, bytes, g_fifo_bytes_len);
    }
    g_draw_dump_ready = true;
}

// A guest frame reaches this from three places - GXCopyDisp, GXDrawDone and VIWaitForRetrace - but
// only the first carries the frame's draws. Presenting on all three swapped in two empty buffers per
// frame, and an empty buffer is just the initialized clear colour, so the strap image was overwritten
// by navy almost as soon as it appeared. A drain that produced no draws now leaves the previous image
// on screen instead of swapping, which makes the last of the three calls a no-op rather than a wipe.
// Frame capture trigger (see mkwv/gx/capture.hpp): hold L + R + TRIANGLE, or create an empty
// file named capture_now in the data folder. The frame after the trigger is written to
// <data>/capture_NN/ and the log says where.
static void poll_capture_trigger() {
    static unsigned s_captures = 0;
    static bool s_combo_held = false;
    static unsigned s_poll = 0;
    if (gx::capture_armed() || gx::capture_active()) {
        return;
    }
    bool want = false;
#if defined(__vita__)
    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    constexpr unsigned kCombo = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_TRIANGLE;
    const bool held = (pad.buttons & kCombo) == kCombo;
    if (held && !s_combo_held) {
        want = true;
    }
    s_combo_held = held;
    if (!want && (++s_poll % 30u) == 0) {
        const SceUID fd = sceIoOpen("ux0:data/viicompiled/capture_now", SCE_O_RDONLY, 0);
        if (fd >= 0) {
            sceIoClose(fd);
            sceIoRemove("ux0:data/viicompiled/capture_now");
            want = true;
        }
    }
#else
    (void)s_combo_held;
    (void)s_poll;
#endif
    if (!want) {
        return;
    }
    char dir[96];
    std::snprintf(dir, sizeof(dir), "ux0:data/viicompiled/capture_%02u", s_captures++);
    gx::capture_arm(dir);
    char line[160];
    std::snprintf(line, sizeof(line), "mkwv: CAPTURE armed -> %s (next frame)\n", dir);
    watchdog_log_line(line);
}

// The part of a present that follows decoding: skip empty drains, hand the frame to the
// renderer, statistics. `bytes` is the frame's final chunk of the command stream (for the dump).
// Runs on whichever thread decodes (the guest thread, or the render thread when offloaded).
static void frame_perf_mark() {
    char extra[320];
    static std::uint32_t s_disp = 0, s_miss = 0, s_slow = 0;
    const std::uint32_t disp = dispatch_count(), miss = dispatch_cache_misses(), slow = guest_slow_accesses();
    int n = std::snprintf(extra, sizeof(extra), " | window: disp=%u dmiss=%u slowmem=%u", disp - s_disp, miss - s_miss,
                          slow - s_slow);
    if (perf::frame_mark_due() && n > 0 && static_cast<std::size_t>(n) < sizeof(extra)) {
        audio_perf_text(extra + n, sizeof(extra) - static_cast<std::size_t>(n));
        const auto engine = take_native_engine_stats();
        const std::size_t used = std::strlen(extra);
        std::snprintf(extra + used, sizeof(extra) - used, " | eng: native=%u ppc=%u", engine.calls, engine.fallback);
    }
    if (const char* line = perf::frame_mark(extra)) {
        s_disp = disp;
        s_miss = miss;
        s_slow = slow;
        watchdog_log_line(line);
    }
}

// Frame skipping (markers frameskip / frameskip2): after each rendered frame, this many
// frames only have their state decoded (gx::set_skip_draws) and are not drawn or swapped.
// The game still runs every frame, so it runs faster while the screen updates less often.
static unsigned g_frameskip = 0;
static unsigned g_skip_run = 0;

// Decides whether the next frame is drawn. Runs on the decoding thread at a frame end.
static void decide_frame_skip() {
    bool skip = false;
    if (g_frameskip != 0 && !gx::capture_armed() && !gx::capture_active()) {
        if (gx::skip_draws()) {
            ++g_skip_run;
            skip = g_skip_run < g_frameskip;
        } else {
            skip = true;
        }
    }
    if (!skip) {
        g_skip_run = 0;
    }
    gx::set_skip_draws(skip);
}

static void finish_present(const std::uint8_t* bytes, std::uint32_t size) {
    // Everything below reads the processed vertices (submission, the draw dump).
    gx::vertex_worker_drain();
    if (gx::skip_draws() && gx::skipped_primitives() != 0 && g_processor->draw_count == 0 &&
        gx::batches_flushed() == 0) {
        // A skipped frame: its state is decoded, nothing was drawn. EFB copies still run
        // (from the last drawn frame, without a clear or a swap); the screen keeps showing
        // the last drawn frame.
        if (gx::efb_copy_events(nullptr) != 0 && g_present != nullptr) {
            g_present(*g_processor);
        }
        perf::count(perf::kSkippedFrames);
        ++g_frames;
        frame_perf_mark();
        decide_frame_skip();
        g_processor->end_frame();
        return;
    }
    // A frame larger than one Processor batch has already rendered its earlier batches; it
    // must still be presented even if the last batch happens to hold no draws.
    if (g_processor->draw_count == 0 && gx::batches_flushed() == 0 && g_frames != 0) {
        // An empty drain is normally just the second or third of the three frame-end calls
        // and is dropped. But one carrying an EFB copy is the dialog snapshot, issued after
        // the scene's GXDrawDone already presented: dropping it silently discarded the copy,
        // which is why the backdrop behind every dialog was blank.
        if (gx::efb_copy_events(nullptr) != 0 && g_present != nullptr) {
            ++g_orphan_copy_drains;
            g_present(*g_processor);
        } else {
            ++g_skipped;
        }
        g_processor->end_frame();
        return;
    }
    poll_capture_trigger();
    {
        // How often frames outgrow the FIFO buffer (and, when a display list is being
        // recorded, how often that could not be absorbed).
        static std::uint32_t s_logged_drains = 0;
        static std::uint32_t s_logged_refused = 0;
        if ((g_frames % 120u) == 0 &&
            (g_fifo_midframe_drains != s_logged_drains || g_fifo_midframe_refused != s_logged_refused)) {
            char line[160];
            std::snprintf(line, sizeof(line), "mkwv: FIFO midframe drains=%u refused=%u overflow=%d\n",
                          g_fifo_midframe_drains, g_fifo_midframe_refused, g_fifo->overflow() ? 1 : 0);
            watchdog_log_line(line);
            s_logged_drains = g_fifo_midframe_drains;
            s_logged_refused = g_fifo_midframe_refused;
        }
    }
    const bool capturing = gx::capture_armed() || gx::capture_active();
    if (capturing || (g_processor->draw_count > 0 && !g_draw_dump_ready)) {
        // Both read processor.verts, where a vertex equal to an earlier one is only a remap.
        gx::resolve_vertex_copies(*g_processor);
    }
    g_last_draws = g_processor->draw_count;
    g_last_verts = g_processor->vert_count;
    g_last_fifo = size;
    if (g_last_draws > 0 && !g_draw_dump_ready) {
        build_draw_dump(*g_processor, bytes, size);
    }
    if (g_last_draws > g_max_draws) {
        g_max_draws = g_last_draws;
    }
    if (g_present != nullptr) {
        g_present(*g_processor);
    }
    if (capturing && !gx::capture_armed() && !gx::capture_active()) {
        char line[320];
        std::snprintf(line, sizeof(line), "mkwv: %s\n", gx::capture_last_summary());
        watchdog_log_line(line);
    }
    if (g_frames == 0) {
        std::printf("mkwv: first guest frame draws=%u verts=%u fifo=%u\n", g_processor->draw_count,
                    g_processor->vert_count, size);
    }
    ++g_frames;
    frame_perf_mark();
    decide_frame_skip();
    // Per-frame: keep the register shadows (they persist like GXData).
    g_processor->end_frame();
}

void set_frameskip(unsigned frames) {
    g_frameskip = frames;
    g_skip_run = 0;
}

// ---------------------------------------------------------------------------------------------
// Render offload (render thread). The guest thread only hands over command bytes; decoding,
// GL submission and the swap run on the render thread, which keeps its own copy of whatever
// tail of the stream ends in the middle of a command.
constexpr std::uint32_t kRenderStreamCap = 256u * 1024u;
static std::uint8_t g_rstream[kRenderStreamCap];
static std::uint32_t g_rlen = 0;

static bool append_render_stream(const std::uint8_t* data, std::uint32_t size) {
    if (size > kRenderStreamCap - g_rlen) {
        // A single command longer than the stream buffer: cannot happen with a 64 KiB FIFO
        // (the tail is always shorter than one FIFO buffer). Drop rather than overrun.
        static bool reported = false;
        if (!reported) {
            reported = true;
            watchdog_log_line("mkwv: render stream overflow, frame bytes dropped\n");
        }
        g_rlen = 0;
        return false;
    }
    std::memcpy(g_rstream + g_rlen, data, size);
    g_rlen += size;
    return true;
}

void hle_present_frame() {
    if (g_fifo == nullptr || g_processor == nullptr) {
        return;
    }
    // Bytes being recorded into a display list are still in the live FIFO buffer; draining
    // them now would execute the list early (VIWaitForRetrace can land mid-recording).
    if (hle_gx_in_display_list()) {
        return;
    }
    if (g_offload != nullptr) {
        g_offload->present(g_fifo->data(), g_fifo->size());
        gx::fifo_reset_buffer_only(g_fifo->gx());
        return;
    }
    {
        perf::Scope timing(perf::kDecode);
        g_fifo->drain(*g_processor);
    }
    finish_present(g_fifo->data(), g_fifo->size());
    gx::fifo_reset_buffer_only(g_fifo->gx());
}

void hle_frame_boundary() {
    if (g_offload != nullptr && g_offload->frame_boundary != nullptr) {
        g_offload->frame_boundary();
    }
}

void set_render_offload(const RenderOffload* offload) {
    g_offload = offload;
    g_rlen = 0;
}

static std::uint32_t g_handoff_threshold = 8u * 1024u;

void set_render_handoff_threshold(std::uint32_t threshold) {
    g_handoff_threshold = threshold;
}

void hle_fifo_early_handoff() {
    if (g_offload == nullptr || g_fifo == nullptr || g_handoff_threshold == 0 ||
        g_fifo->size() < g_handoff_threshold || g_fifo->overflow()) {
        return;
    }
    // Bytes being recorded into a display list are not commands yet (see hle_present_frame).
    if (hle_gx_in_display_list()) {
        return;
    }
    g_offload->chunk(g_fifo->data(), g_fifo->size());
    gx::fifo_reset_buffer_only(g_fifo->gx());
}

void render_consume_chunk(const std::uint8_t* data, std::uint32_t size) {
    if (g_processor == nullptr || !append_render_stream(data, size)) {
        return;
    }
    std::uint32_t consumed = 0;
    {
        perf::Scope timing(perf::kDecode);
        consumed = gx::process_prefix(*g_processor, g_rstream, g_rlen, true);
    }
    if (consumed != 0 && consumed <= g_rlen) {
        std::memmove(g_rstream, g_rstream + consumed, g_rlen - consumed);
        g_rlen -= consumed;
    }
}

void render_consume_present(const std::uint8_t* data, std::uint32_t size) {
    if (g_processor == nullptr) {
        return;
    }
    append_render_stream(data, size);
    {
        perf::Scope timing(perf::kDecode);
        if (g_rlen != 0) {
            g_processor->process(g_rstream, g_rlen, true);
        }
    }
    finish_present(g_rstream, g_rlen);
    g_rlen = 0;
}

} // namespace mkwv::runtime

extern "C" void OSSleepThread_HLE_801aa9b8(CpuContext* ctx) {
    // A cooperative guest thread (hle_audio.hpp) really sleeps on the queue until OSWakeupThread.
    if (ctx != nullptr && mkwv::runtime::fiber_sleep_on(ctx->gpr[3])) {
        return;
    }
#if defined(__vita__)
    // Guest threads are not scheduled here (the HLE runs them inline), so nothing the caller
    // waits for can happen *during* this sleep; it only yields the core. 1 ms instead of a
    // frame: a polling loop still makes progress in wall time, without losing 16 ms a call.
    {
        mkwv::perf::Scope timing(mkwv::perf::kSleep);
        mkwv::runtime::prof::ActivityScope activity(mkwv::runtime::prof::kSleep);
        const unsigned held = mkwv::runtime::irq_release_all();  // a sleeping thread holds nothing
        sceKernelDelayThread(1000);
        mkwv::runtime::irq_reacquire(held);
    }
#endif
    // A wait loop is also where the audio interrupt gets its chance.
    mkwv::runtime::audio_poll(ctx);
}

extern "C" void EGG__AsyncDisplay__endRender_HLE_8020ff9c(CpuContext* ctx) {
    InvokeIndirectCpu(0x80219FB4u, ctx);
    GX__DrawDone_8016eab0();
}
