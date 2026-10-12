#include "ppc_runtime.h"

extern "C" void PPCMfhid2_HLE_8012e630(CpuContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    if (ctx->hid2 == 0) {
        ctx->hid2 = 0x10000000u;
    }
    ctx->gpr[3] = ctx->hid2;
}

extern "C" void PPCHalt_8012e5a4(CpuContext*) {}
