// Native call catalog used by InvokeDirectCpu. Vita subset of the GX/OS/EGG
// addresses the translator skipped (PPC_NATIVE_OVERRIDE). Indirect bctrl still
// needs the runtime table in hle_native.cpp.

// ---- GX SDK HLE (vita/src/runtime/hle_gx.cpp, hle_os.cpp). Every non-translated GX
// function has a compile-time binding here and a runtime entry in hle_native.cpp g_natives.
// By-value GXColor / GXColorS10 arguments arrive as a pointer to the caller's copy.
extern "C" void GX__SetVtxDesc_8016d3a4(uint32_t a0, uint32_t a1);
extern "C" void GX__SetVtxDescv_8016d608(uint32_t a0);
extern "C" void GX__GetVtxDesc_8016d9f0(uint32_t a0, uint32_t a1);
extern "C" void GX__GetVtxDescv_8016dba4(uint32_t a0);
extern "C" void GX__ClearVtxDesc_8016dc34();
extern "C" void GX__SetVtxAttrFmt_8016dc68(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4);
extern "C" void GX__SetVtxAttrFmtv_8016de08(uint32_t a0, uint32_t a1);
extern "C" void GX__GetVtxAttrFmt_8016e04c(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4);
extern "C" void GX__GetVtxAttrFmtv_8016e2b8(uint32_t a0, uint32_t a1);
extern "C" void GX__SetArray_8016e32c(uint32_t a0, uint32_t a1, uint32_t a2);
extern "C" void GX__InvalidateVtxCache_8016e36c();
extern "C" void GX__SetTexCoordGen2_8016e37c(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);
extern "C" void GX__SetNumTexGens_8016e5a4(uint32_t a0);
extern "C" void GX__DrawDone_8016eab0();
extern "C" void GX__PixModeSync_8016eb70();
extern "C" void GX__Begin_8016f0f0(uint32_t a0, uint32_t a1, uint32_t a2);
extern "C" void GX__SetLineWidth_8016f314(uint32_t a0, uint32_t a1);
extern "C" void GX__SetPointSize_8016f348(uint32_t a0, uint32_t a1);
extern "C" void GX__EnableTexOffsets_8016f37c(uint32_t a0, uint32_t a1, uint32_t a2);
extern "C" void GX__SetCullMode_8016f3b8(uint32_t a0);
extern "C" void GX__SetCoPlanar_8016f3e0(uint32_t a0);
extern "C" void GX__SetTexCopySrc_8016f478(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
extern "C" void GX__SetTexCopyDst_8016f4dc(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
extern "C" void GX__SetCopyClamp_8016f618(uint32_t a0);
extern "C" void GX__CopyTex_8016fd74(uint32_t a0, uint32_t a1);
extern "C" void GX__InitLightAttn_8016ff04(uint32_t a0, float a1, float a2, float a3, float a4, float a5, float a6);
extern "C" void GX__InitLightAttnA_8016ff20(uint32_t a0, float a1, float a2, float a3);
extern "C" void GX__InitLightAttnK_8016ff30(uint32_t a0, float a1, float a2, float a3);
extern "C" void GX__InitLightPos_80170198(uint32_t a0, float a1, float a2, float a3);
extern "C" void GX__GetLightPos_801701a8(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
extern "C" void GX__InitLightDir_801701c4(uint32_t a0, float a1, float a2, float a3);
extern "C" void GX__GetLightDir_801701e0(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
extern "C" void GX__InitLightColor_80170314(uint32_t a0, uint32_t a1);
extern "C" void GX__LoadLightObjImm_80170320(uint32_t a0, uint32_t a1);
extern "C" void GX__SetChanAmbColor_8017039c(uint32_t a0, uint32_t a1);
extern "C" void GX__SetChanMatColor_80170474(uint32_t a0, uint32_t a1);
extern "C" void GX__SetNumChans_8017054c(uint32_t a0);
extern "C" void GX__SetChanCtrl_80170570(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6);
extern "C" void GX__GetImageTileCount_80170738(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);
extern "C" void GX__InitTexObj_801707f8(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7);
extern "C" void GX__InitTexObjLOD_80170a4c(uint32_t a0, uint32_t a1, uint32_t a2, float a3, float a4, float a5, uint32_t a6, uint32_t a7, uint32_t a8);
extern "C" void GX__InitTexObjWrapMode_80170b50(uint32_t a0, uint32_t a1, uint32_t a2);
extern "C" void GX__InitTexObjTlut_80170b64(uint32_t a0, uint32_t a1);
extern "C" void GX__InitTexObjFilter_80170b6c(uint32_t a0, uint32_t a1, uint32_t a2);
extern "C" void GX__InitTexObjLODBias_80170b94(uint32_t a0, float a1);
extern "C" void GX__InitTexObjUserData_80170be8(uint32_t a0, uint32_t a1);
extern "C" void GX__GetTexObjAll_80170bf8(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7);
extern "C" void GX__LoadTexObjPreLoaded_80170dc8(uint32_t a0, uint32_t a1, uint32_t a2);
extern "C" void GX__LoadTexObj_80170f2c(uint32_t a0, uint32_t a1);
extern "C" void GX__InitTlutObj_80170f80(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
extern "C" void GX__LoadTlut_80170fa8(uint32_t a0, uint32_t a1);
extern "C" void GX__InvalidateTexAll_80171110();
extern "C" void GX__SetTexCoordScaleManually_80171180(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
extern "C" void GX__SetTexCoordBias_801711fc(uint32_t a0, uint32_t a1, uint32_t a2);
extern "C" void GX__SetTevIndirect_801717ac(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8, uint32_t a9);
extern "C" void GX__SetIndTexMtx_80171814(uint32_t a0, uint32_t a1, uint32_t a2);
extern "C" void GX__SetIndTexCoordScale_80171968(uint32_t a0, uint32_t a1, uint32_t a2);
extern "C" void GX__SetIndTexOrder_80171a6c(uint32_t a0, uint32_t a1, uint32_t a2);
extern "C" void GX__SetNumIndStages_80171b38(uint32_t a0);
extern "C" void GX__SetTevDirect_80171b58(uint32_t a0);
extern "C" void GX__SetTevIndWarp_80171ba0(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4);
extern "C" void GX__SetTevOp_80171c4c(uint32_t a0, uint32_t a1);
extern "C" void GX__SetTevColorIn_80171ce0(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4);
extern "C" void GX__SetTevAlphaIn_80171d20(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4);
extern "C" void GX__SetTevColorOp_80171d60(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);
extern "C" void GX__SetTevAlphaOp_80171db8(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);
extern "C" void GX__SetTevColor_80171e10(uint32_t a0, uint32_t a1);
extern "C" void GX__SetTevColorS10_80171e70(uint32_t a0, uint32_t a1);
extern "C" void GX__SetTevKColor_80171ed4(uint32_t a0, uint32_t a1);
extern "C" void GX__SetTevKColorSel_80171f30(uint32_t a0, uint32_t a1);
extern "C" void GX__SetTevKAlphaSel_80171f80(uint32_t a0, uint32_t a1);
extern "C" void GX__SetTevSwapMode_80171fd0(uint32_t a0, uint32_t a1, uint32_t a2);
extern "C" void GX__SetTevSwapModeTable_8017200c(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4);
extern "C" void GX__SetAlphaCompare_80172088(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4);
extern "C" void GX__SetZTexture_801720c0(uint32_t a0, uint32_t a1, uint32_t a2);
extern "C" void GX__SetTevOrder_8017214c(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
extern "C" void GX__SetNumTevStages_801722a8(uint32_t a0);
extern "C" void GX__SetFog_801722cc(uint32_t a0, float a1, float a2, float a3, float a4, uint32_t a5);
extern "C" void GX__InitFogAdjTable_801724f8(uint32_t a0, uint32_t a1, uint32_t a2);
extern "C" void GX__SetBlendMode_8017277c(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
extern "C" void GX__SetColorUpdate_801727cc(uint32_t a0);
extern "C" void GX__SetAlphaUpdate_801727f8(uint32_t a0);
extern "C" void GX__SetZMode_80172824(uint32_t a0, uint32_t a1, uint32_t a2);
extern "C" void GX__SetZCompLoc_80172858(uint32_t a0);
extern "C" void GX__SetPixelFmt_80172888(uint32_t a0, uint32_t a1);
extern "C" void GX__SetDither_80172930(uint32_t a0);
extern "C" void GX__SetDstAlpha_8017295c(uint32_t a0, uint32_t a1);
extern "C" void GX__SetFieldMask_8017298c(uint32_t a0, uint32_t a1);
extern "C" void GX__DrawSphere_80172a30(uint32_t a0, uint32_t a1);
extern "C" void GX__BeginDisplayList_80172e00(uint32_t a0, uint32_t a1);
extern "C" void GX__CallDisplayList_80172f64(uint32_t a0, uint32_t a1);
extern "C" void GX__SetProjection_8017301c(uint32_t a0, uint32_t a1);
extern "C" void GX__SetProjectionv_80173080(uint32_t a0);
extern "C" void GX__GetProjectionv_801730cc(uint32_t a0);
extern "C" void GX__LoadPosMtxImm_8017310c(uint32_t a0, uint32_t a1);
extern "C" void GX__LoadPosMtxIndx_8017315c(uint32_t a0, uint32_t a1);
extern "C" void GX__LoadNrmMtxImm_80173188(uint32_t a0, uint32_t a1);
extern "C" void GX__LoadNrmMtxIndx3x3_801731e0(uint32_t a0, uint32_t a1);
extern "C" void GX__SetCurrentMtx_80173214(uint32_t a0);
extern "C" void GX__LoadTexMtxImm_80173234(uint32_t a0, uint32_t a1, uint32_t a2);
extern "C" void GX__SetViewportJitter_80173378(float a0, float a1, float a2, float a3, float a4, float a5, uint32_t a6);
extern "C" void GX__SetViewport_801733b4(float a0, float a1, float a2, float a3, float a4, float a5);
extern "C" void GX__GetViewportv_801733e0(uint32_t a0);
extern "C" void GX__SetZScaleOffset_80173400(float a0, float a1);
extern "C" void GX__SetScissor_80173430(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
extern "C" void GX__GetScissor_80173498(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
extern "C" void GX__SetScissorBoxOffset_801734e0(int32_t a0, int32_t a1);
extern "C" void GX__SetClipMode_8017351c(uint32_t a0);
extern "C" void GX__SetDrawDoneCallback_8016ed50(CpuContext* ctx);
extern "C" void GX__SetDirtyState_8016ee78(CpuContext* ctx);
extern "C" void GX__InitTexObjCI_80170a04(CpuContext* ctx);
extern "C" void GX__GetTexObjUserData_80170bf0(CpuContext* ctx);
extern "C" void GX__GetTexObjWidth_80170c68(CpuContext* ctx);
extern "C" void GX__GetTexObjHeight_80170c7c(CpuContext* ctx);
extern "C" void GX__GetTexObjFmt_80170c90(CpuContext* ctx);
extern "C" void GX__GetTexObjWrapS_80170c98(CpuContext* ctx);
extern "C" void GX__GetTexObjWrapT_80170ca4(CpuContext* ctx);
extern "C" void GX__GetTexObjMipMap_80170cb0(CpuContext* ctx);
extern "C" void GX__GetTexObjLODAll_80170cbc(CpuContext* ctx);
extern "C" void GX__GetTexObjMinFilt_80170da0(CpuContext* ctx);
extern "C" void GX__GetTexObjTlut_80170dc0(CpuContext* ctx);
extern "C" void GX__SetTexRegionCallback_80171158(CpuContext* ctx);
extern "C" void GX__SetTlutRegionCallback_8017116c(CpuContext* ctx);
extern "C" void GX__EndDisplayListCpu_80172eb4(CpuContext* ctx);
extern "C" std::uint32_t GX__EndDisplayList_80172eb4();
extern "C" void GX_NoOp(CpuContext* ctx);
extern "C" void StrapScene_CheckInput_800077c8(CpuContext* ctx);
extern "C" void SoundPlayerSetVolume_800a35e0(CpuContext* ctx);
extern "C" void AIRegisterDMACallback_80123f88(CpuContext* ctx);
extern "C" void AIInitDMA_80123fcc(CpuContext* ctx);
extern "C" void AIStartDMA_80124048(CpuContext* ctx);
extern "C" void AICheckInit_80124094(CpuContext* ctx);
extern "C" void AIInit_801240b0(CpuContext* ctx);
extern "C" void AXOutInitDSP_801269bc(CpuContext* ctx);
extern "C" void DCStoreRange_801a165c(CpuContext* ctx);
extern "C" void DCStoreRangeNoSync_801a16b8(CpuContext* ctx);
extern "C" void KPADGetUnifiedWpadStatus_8019812c(CpuContext* ctx);
extern "C" void PADRead_801af44c(CpuContext* ctx);
extern "C" void VIGetRetraceCount_801baba4(CpuContext* ctx);
extern "C" void WPADSetSyncDeviceCallback_801bf640(CpuContext* ctx);
extern "C" void WPADProbe_801c0990(CpuContext* ctx);
extern "C" void NandMgrDeleteRKSYS_8052c7e4(CpuContext* ctx);
extern "C" void NWC24OfflineSaveTask_80672cc8(CpuContext* ctx);
extern "C" void OSSetAlarm_801a0870(CpuContext* ctx);
extern "C" void OSSetPeriodicAlarm_801a08e0(CpuContext* ctx);
// MEM::AllocFromExpHeapEx is still called directly by portions of the
// translated SDK.  Its lock/unlock calls must use the same HLE mutex path as
// ExpHeap_Alloc, otherwise the translated OS mutex code corrupts the heap
// operation before the allocation result is returned.
extern "C" void OSLockMutex_801a7ee4(CpuContext* ctx);
extern "C" void OSUnlockMutex_801a7fc0(CpuContext* ctx);
extern "C" void ExpHeap_Alloc_80226c04(CpuContext* ctx);
extern "C" void ExpHeap_Free_80226c78(CpuContext* ctx);
extern "C" void OSSleepThread_HLE_801aa9b8(CpuContext* ctx);
extern "C" void EGG__AsyncDisplay__endRender_HLE_8020ff9c(CpuContext* ctx);
extern "C" void EGG_Decomp_decodeSZS_80218c2c(CpuContext* ctx);

#define MKW_KNOWN_TYPED_NATIVE(addr, fn, ...)          \
    template <>                                        \
    struct KnownTypedNativeCpuCall<addr> {             \
        static constexpr bool kAvailable = true;       \
        static void Invoke(CpuContext* cpu) {          \
            (void)cpu;                                 \
            fn(__VA_ARGS__);                           \
        }                                              \
    }

// GX typed natives. Stack arguments (9th+ integer argument) are read from 8(r1), 12(r1)...
MKW_KNOWN_TYPED_NATIVE(0x8016D3A4u, GX__SetVtxDesc_8016d3a4, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x8016D608u, GX__SetVtxDescv_8016d608, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x8016D9F0u, GX__GetVtxDesc_8016d9f0, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x8016DBA4u, GX__GetVtxDescv_8016dba4, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x8016DC34u, GX__ClearVtxDesc_8016dc34);
MKW_KNOWN_TYPED_NATIVE(0x8016DC68u, GX__SetVtxAttrFmt_8016dc68, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_TYPED_NATIVE(0x8016DE08u, GX__SetVtxAttrFmtv_8016de08, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x8016E04Cu, GX__GetVtxAttrFmt_8016e04c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_TYPED_NATIVE(0x8016E2B8u, GX__GetVtxAttrFmtv_8016e2b8, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x8016E32Cu, GX__SetArray_8016e32c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(0x8016E36Cu, GX__InvalidateVtxCache_8016e36c);
MKW_KNOWN_TYPED_NATIVE(0x8016E37Cu, GX__SetTexCoordGen2_8016e37c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_TYPED_NATIVE(0x8016E5A4u, GX__SetNumTexGens_8016e5a4, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x8016EAB0u, GX__DrawDone_8016eab0);
MKW_KNOWN_TYPED_NATIVE(0x8016EB70u, GX__PixModeSync_8016eb70);
MKW_KNOWN_TYPED_NATIVE(0x8016F0F0u, GX__Begin_8016f0f0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(0x8016F314u, GX__SetLineWidth_8016f314, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x8016F348u, GX__SetPointSize_8016f348, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x8016F37Cu, GX__EnableTexOffsets_8016f37c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(0x8016F3B8u, GX__SetCullMode_8016f3b8, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x8016F3E0u, GX__SetCoPlanar_8016f3e0, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x8016F478u, GX__SetTexCopySrc_8016f478, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_TYPED_NATIVE(0x8016F4DCu, GX__SetTexCopyDst_8016f4dc, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_TYPED_NATIVE(0x8016F618u, GX__SetCopyClamp_8016f618, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x8016FD74u, GX__CopyTex_8016fd74, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x8016FF04u, GX__InitLightAttn_8016ff04, cpu->gpr[3], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d), static_cast<float>(cpu->fpr[4].d), static_cast<float>(cpu->fpr[5].d), static_cast<float>(cpu->fpr[6].d));
MKW_KNOWN_TYPED_NATIVE(0x8016FF20u, GX__InitLightAttnA_8016ff20, cpu->gpr[3], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d));
MKW_KNOWN_TYPED_NATIVE(0x8016FF30u, GX__InitLightAttnK_8016ff30, cpu->gpr[3], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d));
MKW_KNOWN_TYPED_NATIVE(0x80170198u, GX__InitLightPos_80170198, cpu->gpr[3], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d));
MKW_KNOWN_TYPED_NATIVE(0x801701A8u, GX__GetLightPos_801701a8, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_TYPED_NATIVE(0x801701C4u, GX__InitLightDir_801701c4, cpu->gpr[3], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d));
MKW_KNOWN_TYPED_NATIVE(0x801701E0u, GX__GetLightDir_801701e0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_TYPED_NATIVE(0x80170314u, GX__InitLightColor_80170314, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x80170320u, GX__LoadLightObjImm_80170320, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x8017039Cu, GX__SetChanAmbColor_8017039c, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x80170474u, GX__SetChanMatColor_80170474, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x8017054Cu, GX__SetNumChans_8017054c, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x80170570u, GX__SetChanCtrl_80170570, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9]);
MKW_KNOWN_TYPED_NATIVE(0x80170738u, GX__GetImageTileCount_80170738, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_TYPED_NATIVE(0x801707F8u, GX__InitTexObj_801707f8, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9], cpu->gpr[10]);
MKW_KNOWN_TYPED_NATIVE(0x80170A4Cu, GX__InitTexObjLOD_80170a4c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d), cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_TYPED_NATIVE(0x80170B50u, GX__InitTexObjWrapMode_80170b50, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(0x80170B64u, GX__InitTexObjTlut_80170b64, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x80170B6Cu, GX__InitTexObjFilter_80170b6c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(0x80170B94u, GX__InitTexObjLODBias_80170b94, cpu->gpr[3], static_cast<float>(cpu->fpr[1].d));
MKW_KNOWN_TYPED_NATIVE(0x80170BE8u, GX__InitTexObjUserData_80170be8, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x80170BF8u, GX__GetTexObjAll_80170bf8, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9], cpu->gpr[10]);
MKW_KNOWN_TYPED_NATIVE(0x80170DC8u, GX__LoadTexObjPreLoaded_80170dc8, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(0x80170F2Cu, GX__LoadTexObj_80170f2c, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x80170F80u, GX__InitTlutObj_80170f80, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_TYPED_NATIVE(0x80170FA8u, GX__LoadTlut_80170fa8, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x80171110u, GX__InvalidateTexAll_80171110);
MKW_KNOWN_TYPED_NATIVE(0x80171180u, GX__SetTexCoordScaleManually_80171180, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_TYPED_NATIVE(0x801711FCu, GX__SetTexCoordBias_801711fc, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(0x801717ACu, GX__SetTevIndirect_801717ac, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9], cpu->gpr[10], Memory::Read32(cpu->gpr[1] + 8u), Memory::Read32(cpu->gpr[1] + 12u));
MKW_KNOWN_TYPED_NATIVE(0x80171814u, GX__SetIndTexMtx_80171814, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(0x80171968u, GX__SetIndTexCoordScale_80171968, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(0x80171A6Cu, GX__SetIndTexOrder_80171a6c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(0x80171B38u, GX__SetNumIndStages_80171b38, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x80171B58u, GX__SetTevDirect_80171b58, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x80171BA0u, GX__SetTevIndWarp_80171ba0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_TYPED_NATIVE(0x80171C4Cu, GX__SetTevOp_80171c4c, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x80171CE0u, GX__SetTevColorIn_80171ce0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_TYPED_NATIVE(0x80171D20u, GX__SetTevAlphaIn_80171d20, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_TYPED_NATIVE(0x80171D60u, GX__SetTevColorOp_80171d60, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_TYPED_NATIVE(0x80171DB8u, GX__SetTevAlphaOp_80171db8, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_TYPED_NATIVE(0x80171E10u, GX__SetTevColor_80171e10, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x80171E70u, GX__SetTevColorS10_80171e70, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x80171ED4u, GX__SetTevKColor_80171ed4, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x80171F30u, GX__SetTevKColorSel_80171f30, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x80171F80u, GX__SetTevKAlphaSel_80171f80, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x80171FD0u, GX__SetTevSwapMode_80171fd0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(0x8017200Cu, GX__SetTevSwapModeTable_8017200c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_TYPED_NATIVE(0x80172088u, GX__SetAlphaCompare_80172088, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_TYPED_NATIVE(0x801720C0u, GX__SetZTexture_801720c0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(0x8017214Cu, GX__SetTevOrder_8017214c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_TYPED_NATIVE(0x801722A8u, GX__SetNumTevStages_801722a8, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x801722CCu, GX__SetFog_801722cc, cpu->gpr[3], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d), static_cast<float>(cpu->fpr[4].d), cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x801724F8u, GX__InitFogAdjTable_801724f8, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(0x8017277Cu, GX__SetBlendMode_8017277c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_TYPED_NATIVE(0x801727CCu, GX__SetColorUpdate_801727cc, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x801727F8u, GX__SetAlphaUpdate_801727f8, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x80172824u, GX__SetZMode_80172824, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(0x80172858u, GX__SetZCompLoc_80172858, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x80172888u, GX__SetPixelFmt_80172888, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x80172930u, GX__SetDither_80172930, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x8017295Cu, GX__SetDstAlpha_8017295c, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x8017298Cu, GX__SetFieldMask_8017298c, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x80172A30u, GX__DrawSphere_80172a30, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x80172E00u, GX__BeginDisplayList_80172e00, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x80172F64u, GX__CallDisplayList_80172f64, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x8017301Cu, GX__SetProjection_8017301c, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x80173080u, GX__SetProjectionv_80173080, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x801730CCu, GX__GetProjectionv_801730cc, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x8017310Cu, GX__LoadPosMtxImm_8017310c, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x8017315Cu, GX__LoadPosMtxIndx_8017315c, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x80173188u, GX__LoadNrmMtxImm_80173188, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x801731E0u, GX__LoadNrmMtxIndx3x3_801731e0, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x80173214u, GX__SetCurrentMtx_80173214, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x80173234u, GX__LoadTexMtxImm_80173234, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(0x80173378u, GX__SetViewportJitter_80173378, static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d), static_cast<float>(cpu->fpr[4].d), static_cast<float>(cpu->fpr[5].d), static_cast<float>(cpu->fpr[6].d), cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x801733B4u, GX__SetViewport_801733b4, static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d), static_cast<float>(cpu->fpr[4].d), static_cast<float>(cpu->fpr[5].d), static_cast<float>(cpu->fpr[6].d));
MKW_KNOWN_TYPED_NATIVE(0x801733E0u, GX__GetViewportv_801733e0, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x80173400u, GX__SetZScaleOffset_80173400, static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d));
MKW_KNOWN_TYPED_NATIVE(0x80173430u, GX__SetScissor_80173430, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_TYPED_NATIVE(0x80173498u, GX__GetScissor_80173498, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_TYPED_NATIVE(0x801734E0u, GX__SetScissorBoxOffset_801734e0, static_cast<int32_t>(cpu->gpr[3]), static_cast<int32_t>(cpu->gpr[4]));
MKW_KNOWN_TYPED_NATIVE(0x8017351Cu, GX__SetClipMode_8017351c, cpu->gpr[3]);

#undef MKW_KNOWN_TYPED_NATIVE

#define MKW_KNOWN_NATIVE_CPU_CALL(addr, fn)                                    \
    template <>                                                                \
    struct KnownNativeCpuCall<addr> {                                          \
        static constexpr bool kAvailable = true;                               \
        static constexpr uint32_t kNonvolatileFprWriteMask = 0;                \
        static constexpr void (*Entry)(CpuContext*) = &fn;                     \
    }

MKW_KNOWN_NATIVE_CPU_CALL(0x800077C8u, StrapScene_CheckInput_800077c8);
MKW_KNOWN_NATIVE_CPU_CALL(0x800A35E0u, SoundPlayerSetVolume_800a35e0);
MKW_KNOWN_NATIVE_CPU_CALL(0x80123F88u, AIRegisterDMACallback_80123f88);
MKW_KNOWN_NATIVE_CPU_CALL(0x80123FCCu, AIInitDMA_80123fcc);
MKW_KNOWN_NATIVE_CPU_CALL(0x80124048u, AIStartDMA_80124048);
MKW_KNOWN_NATIVE_CPU_CALL(0x80124094u, AICheckInit_80124094);
MKW_KNOWN_NATIVE_CPU_CALL(0x801240B0u, AIInit_801240b0);
MKW_KNOWN_NATIVE_CPU_CALL(0x801269BCu, AXOutInitDSP_801269bc);
MKW_KNOWN_NATIVE_CPU_CALL(0x8019812Cu, KPADGetUnifiedWpadStatus_8019812c);
MKW_KNOWN_NATIVE_CPU_CALL(0x801A165Cu, DCStoreRange_801a165c);
MKW_KNOWN_NATIVE_CPU_CALL(0x801A16B8u, DCStoreRangeNoSync_801a16b8);
MKW_KNOWN_NATIVE_CPU_CALL(0x801AF44Cu, PADRead_801af44c);
MKW_KNOWN_NATIVE_CPU_CALL(0x801BABA4u, VIGetRetraceCount_801baba4);
MKW_KNOWN_NATIVE_CPU_CALL(0x801BF640u, WPADSetSyncDeviceCallback_801bf640);
MKW_KNOWN_NATIVE_CPU_CALL(0x801C0990u, WPADProbe_801c0990);
MKW_KNOWN_NATIVE_CPU_CALL(0x801A0870u, OSSetAlarm_801a0870);
MKW_KNOWN_NATIVE_CPU_CALL(0x801A08E0u, OSSetPeriodicAlarm_801a08e0);
MKW_KNOWN_NATIVE_CPU_CALL(0x801A7EE4u, OSLockMutex_801a7ee4);
MKW_KNOWN_NATIVE_CPU_CALL(0x801A7FC0u, OSUnlockMutex_801a7fc0);
MKW_KNOWN_NATIVE_CPU_CALL(0x80226C04u, ExpHeap_Alloc_80226c04);
MKW_KNOWN_NATIVE_CPU_CALL(0x80226C78u, ExpHeap_Free_80226c78);
MKW_KNOWN_NATIVE_CPU_CALL(0x801AA9B8u, OSSleepThread_HLE_801aa9b8);
MKW_KNOWN_NATIVE_CPU_CALL(0x8020FF9Cu, EGG__AsyncDisplay__endRender_HLE_8020ff9c);
MKW_KNOWN_NATIVE_CPU_CALL(0x80218C2Cu, EGG_Decomp_decodeSZS_80218c2c);
MKW_KNOWN_NATIVE_CPU_CALL(0x8052C7E4u, NandMgrDeleteRKSYS_8052c7e4);
MKW_KNOWN_NATIVE_CPU_CALL(0x80672CC8u, NWC24OfflineSaveTask_80672cc8);

// GX natives that return a value, read stack arguments, or are deliberate no-ops.
MKW_KNOWN_NATIVE_CPU_CALL(0x8016ED50u, GX__SetDrawDoneCallback_8016ed50);
MKW_KNOWN_NATIVE_CPU_CALL(0x8016EE78u, GX__SetDirtyState_8016ee78);
MKW_KNOWN_NATIVE_CPU_CALL(0x80170A04u, GX__InitTexObjCI_80170a04);
MKW_KNOWN_NATIVE_CPU_CALL(0x80170BF0u, GX__GetTexObjUserData_80170bf0);
MKW_KNOWN_NATIVE_CPU_CALL(0x80170C68u, GX__GetTexObjWidth_80170c68);
MKW_KNOWN_NATIVE_CPU_CALL(0x80170C7Cu, GX__GetTexObjHeight_80170c7c);
MKW_KNOWN_NATIVE_CPU_CALL(0x80170C90u, GX__GetTexObjFmt_80170c90);
MKW_KNOWN_NATIVE_CPU_CALL(0x80170C98u, GX__GetTexObjWrapS_80170c98);
MKW_KNOWN_NATIVE_CPU_CALL(0x80170CA4u, GX__GetTexObjWrapT_80170ca4);
MKW_KNOWN_NATIVE_CPU_CALL(0x80170CB0u, GX__GetTexObjMipMap_80170cb0);
MKW_KNOWN_NATIVE_CPU_CALL(0x80170CBCu, GX__GetTexObjLODAll_80170cbc);
MKW_KNOWN_NATIVE_CPU_CALL(0x80170DA0u, GX__GetTexObjMinFilt_80170da0);
MKW_KNOWN_NATIVE_CPU_CALL(0x80170DC0u, GX__GetTexObjTlut_80170dc0);
MKW_KNOWN_NATIVE_CPU_CALL(0x80171158u, GX__SetTexRegionCallback_80171158);
MKW_KNOWN_NATIVE_CPU_CALL(0x8017116Cu, GX__SetTlutRegionCallback_8017116c);
MKW_KNOWN_NATIVE_CPU_CALL(0x80172EB4u, GX__EndDisplayListCpu_80172eb4);
MKW_KNOWN_NATIVE_CPU_CALL(0x8016E6B0u, GX_NoOp);  // GXResetWriteGatherPipe
MKW_KNOWN_NATIVE_CPU_CALL(0x8016E848u, GX_NoOp);  // GXAbortFrame
MKW_KNOWN_NATIVE_CPU_CALL(0x8016E9FCu, GX_NoOp);  // GXSetDrawSync
MKW_KNOWN_NATIVE_CPU_CALL(0x8016EB94u, GX_NoOp);  // GXPokeAlphaMode
MKW_KNOWN_NATIVE_CPU_CALL(0x8016EBA4u, GX_NoOp);  // GXPokeAlphaRead
MKW_KNOWN_NATIVE_CPU_CALL(0x8016EBBCu, GX_NoOp);  // GXPokeAlphaUpdate
MKW_KNOWN_NATIVE_CPU_CALL(0x8016EBD0u, GX_NoOp);  // GXPokeBlendMode
MKW_KNOWN_NATIVE_CPU_CALL(0x8016EC2Cu, GX_NoOp);  // GXPokeColorUpdate
MKW_KNOWN_NATIVE_CPU_CALL(0x8016EC40u, GX_NoOp);  // GXPokeDstAlpha
MKW_KNOWN_NATIVE_CPU_CALL(0x8016EC58u, GX_NoOp);  // GXPokeDither
MKW_KNOWN_NATIVE_CPU_CALL(0x8016EC6Cu, GX_NoOp);  // GXPokeZMode
MKW_KNOWN_NATIVE_CPU_CALL(0x8016F414u, GX_NoOp);  // __GXSetGenMode
MKW_KNOWN_NATIVE_CPU_CALL(0x8016F5F8u, GX_NoOp);  // GXSetDispCopyFrame2Field
MKW_KNOWN_NATIVE_CPU_CALL(0x8016FC24u, GX_NoOp);  // GXSetDispCopyGamma
MKW_KNOWN_NATIVE_CPU_CALL(0x8016FECCu, GX_NoOp);  // GXClearBoundingBox
MKW_KNOWN_NATIVE_CPU_CALL(0x80171260u, GX_NoOp);  // __SetSURegs
MKW_KNOWN_NATIVE_CPU_CALL(0x801712F0u, GX_NoOp);  // __GXSetSUTexRegs
MKW_KNOWN_NATIVE_CPU_CALL(0x80171458u, GX_NoOp);  // __GXSetTmemConfig
MKW_KNOWN_NATIVE_CPU_CALL(0x80171BF4u, GX_NoOp);  // __GXUpdateBPMask
MKW_KNOWN_NATIVE_CPU_CALL(0x80171BF8u, GX_NoOp);  // __GXSetIndirectMask
MKW_KNOWN_NATIVE_CPU_CALL(0x80171C28u, GX_NoOp);  // __GXFlushTextureState
MKW_KNOWN_NATIVE_CPU_CALL(0x80172FD8u, GX_NoOp);  // __GXSetProjection
MKW_KNOWN_NATIVE_CPU_CALL(0x801732E8u, GX_NoOp);  // __GXSetViewport
MKW_KNOWN_NATIVE_CPU_CALL(0x80173544u, GX_NoOp);  // __GXSetMatrixIndex
MKW_KNOWN_NATIVE_CPU_CALL(0x801735CCu, GX_NoOp);  // GXSetGPMetric
MKW_KNOWN_NATIVE_CPU_CALL(0x80173DF8u, GX_NoOp);  // GXClearGPMetric

#undef MKW_KNOWN_NATIVE_CPU_CALL
