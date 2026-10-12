#pragma once

#include <cstdint>

struct CpuContext;

namespace AxDspHle {

void Init();
void InitForAXOut(CpuContext* ctx);
void Stop();

uint32_t CheckInit();
uint32_t AddTask(uint32_t taskPtr);
void SendMailToDSP(uint32_t mail);
uint32_t CheckMailToDSP();
uint32_t CheckMailFromDSP();
uint32_t ReadMailFromDSP();
uint32_t AssertTask(uint32_t taskPtr);
void ServiceDeferredCallbacks();

// INVARIANT: no guest code may observe mix output before this returns. Called from
// Audio_HLE_Tick before the AI DMA block reaches the backend and before the AI callback
// runs __AXOutNewFrame, the two places guest code reads the mix's PB/output/aux data.
// Cheap when the mix already finished (the normal case).
void JoinMixWorker();

// Joins and tears down the mix worker thread. Call before the guest memory map
// is destroyed or re-created.
void ShutdownMixWorker();

void SetMixWorkerEnabled(bool enabled);

// Sound thread: the mix a DSP mail starts is recorded and run by RunDeferredMix() (outside the
// guest lock); JoinMixWorker() runs it too if it is still pending. RunDeferredMix returns true
// when it mixed.
void SetDeferredMix(bool enabled);
// Off: voices' aux-bus sends are not mixed (the aux return is silence with effects off).
void SetAuxSends(bool enabled);
bool RunDeferredMix();

void InitAram();

// Voices mixed and PB lists (DSP frames) processed since the last call.
void TakeVoiceStats(uint32_t* voices, uint32_t* lists);

// Path of Dolphin's free DSP coefficient ROM (dsp_coef.bin); read at the first init.
void SetCoefficientRomPath(const char* path);

}
