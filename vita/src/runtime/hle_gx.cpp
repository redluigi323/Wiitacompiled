// GX SDK high-level emulation.
//
// Almost none of the RVL GX library (0x8016B49C..0x80173DF8) has a translated body, so every GX
// call the game makes lands here. Each native updates a file-static shadow of the SDK's GXData
// and writes the SAME hardware register stream the real SDK writes (BP 0x61, CP 0x08, XF 0x10,
// indexed loads 0x20/0x28, 0x48, draws) into the HLE FIFO, which the decoder in vita/src/gx
// then interprets like the GP would. Register layouts follow the RVL SDK as reimplemented by
// Aurora (upstream/aurora-main/lib/dolphin/gx/*.cpp) - cited per function below.
//
// The only non-hardware things written are the two Aurora escapes the decoder already depends
// on (viewport and scissor), the EFB-copy escape, and one private marker bit in the SU texture
// coordinate scale registers (see GXSetTexCoordScaleManually). Everything is persistent across
// frames like GXData is; hle_present_frame() only empties the byte buffer.

#include "mkwv/runtime/hle_native.hpp"

#include "mkwv/gx/draw_state.hpp"
#include "mkwv/gx/efb_copy.hpp"
#include "mkwv/gx/fifo.hpp"
#include "mkwv/runtime/compact_memory.hpp"
#include "mkwv/runtime/guest_access.hpp"
#include "mkwv/runtime/hle_fifo.hpp"

#include "isa/big_endian.h"
#include "memory.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr std::uint32_t kGxDrawDoneFlagAddr = 0x803867d8u;
constexpr std::uint32_t kDlFifoAddr = 0x80344090u;
constexpr std::uint32_t kGXDataPtrAddr = 0x803886C8u;
constexpr std::uint32_t kGXDataInDispList = 0x5F8u;
constexpr std::uint32_t kGXDataDlSaveContext = 0x5F9u;

// ---------------------------------------------------------------------------------------------
// SDK enum values (dolphin/gx/GXEnum.h). Deliberately numeric rather than the decoder's own
// mkwv::gx enums: the natives speak the guest's API, not the decoder's.
// ---------------------------------------------------------------------------------------------
constexpr std::uint32_t kVaPnMtxIdx = 0;
constexpr std::uint32_t kVaTex7MtxIdx = 8;
constexpr std::uint32_t kVaPos = 9;
constexpr std::uint32_t kVaNrm = 10;
constexpr std::uint32_t kVaClr0 = 11;
constexpr std::uint32_t kVaClr1 = 12;
constexpr std::uint32_t kVaTex0 = 13;
constexpr std::uint32_t kVaTex7 = 20;
constexpr std::uint32_t kVaPosMtxArray = 21;
constexpr std::uint32_t kVaLightArray = 24;
constexpr std::uint32_t kVaNbt = 25;
constexpr std::uint32_t kVaNull = 0xFF;

constexpr std::uint32_t kNrmNbt3 = 2;  // GX_NRM_NBT3

constexpr std::uint32_t kTexMapNull = 0xFF;
constexpr std::uint32_t kTexMapDisable = 0x100;
constexpr std::uint32_t kColorNull = 0xFF;

constexpr std::uint32_t kAfSpec = 0;  // GX_AF_SPEC
constexpr std::uint32_t kAfNone = 2;  // GX_AF_NONE

constexpr std::uint32_t kTexMtxIdentity = 60;   // GX_IDENTITY
constexpr std::uint32_t kPtTexMtxIdentity = 125;  // GX_PTIDENTITY

constexpr unsigned kTevStages = 16;
constexpr unsigned kTlutNames = 20;  // GX_TLUT0..15, GX_BIGTLUT0..3

// __GXData_struct::dirtyState bits (Aurora __gx.h).
constexpr std::uint32_t kDirtySuTex = 1u;
constexpr std::uint32_t kDirtyBpMask = 2u;
constexpr std::uint32_t kDirtyGenMode = 4u;
constexpr std::uint32_t kDirtyVcd = 8u;
constexpr std::uint32_t kDirtyVat = 0x10u;

inline void set_field(std::uint32_t& reg, unsigned size, unsigned shift, std::uint32_t value) {
    const std::uint32_t mask = (size >= 32u ? 0xFFFFFFFFu : ((1u << size) - 1u)) << shift;
    reg = (reg & ~mask) | ((value << shift) & mask);
}

inline std::uint32_t get_field(std::uint32_t reg, unsigned size, unsigned shift) {
    return (reg >> shift) & ((1u << size) - 1u);
}

inline bool gx_bool(std::uint32_t v) {
    return (v & 0xFFu) != 0u;  // GXBool is a u8; the upper register bits are not defined
}

float bits_to_f32(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::uint32_t f32_to_bits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float read_guest_f32(std::uint32_t addr) {
    return bits_to_f32(Memory::Read32(addr));
}

void write_guest_f32(std::uint32_t addr, float value) {
    Memory::Write32(addr, f32_to_bits(value));
}

void read_be_floats(std::uint32_t addr, float* dst, std::uint32_t count) {
    const std::uint8_t* host = Memory::GetPointer(addr, count * 4u);
    if (host != nullptr) {
        for (std::uint32_t i = 0; i < count; ++i) {
            dst[i] = BigEndian::ReadFloat32(host + i * 4u);
        }
        return;
    }
    // A range crossing a CompactMemory page: read it word by word.
    for (std::uint32_t i = 0; i < count; ++i) {
        dst[i] = read_guest_f32(addr + i * 4u);
    }
}

void copy_guest_bytes(std::uint32_t addr, const std::uint8_t* src, std::uint32_t bytes) {
    while (bytes > 0 && src != nullptr) {
        const std::uint32_t into = addr & (mkwv::runtime::kPageSize - 1u);
        std::uint32_t chunk = mkwv::runtime::kPageSize - into;
        if (chunk > bytes) {
            chunk = bytes;
        }
        std::uint8_t* host = Memory::GetPointer(addr, chunk);
        if (host != nullptr) {
            std::memcpy(host, src, chunk);
        } else {
            for (std::uint32_t i = 0; i < chunk; ++i) {
                Memory::Write8(addr + i, src[i]);
            }
        }
        addr += chunk;
        src += chunk;
        bytes -= chunk;
    }
}

// Physical address as the GP sees it (GX_PHY_ADDR).
inline std::uint32_t phys(std::uint32_t addr) {
    return addr & 0x3FFFFFFFu;
}

// ---------------------------------------------------------------------------------------------
// The GXData shadow. Everything the SDK keeps in __GXData that a later call patches a field of
// lives here, so a display list with dlSaveContext can snapshot and restore it wholesale
// (Aurora GXDispList.cpp memcpy's __gx the same way). Register values are held WITHOUT their
// BP id byte; the id is supplied when the register is written.
// ---------------------------------------------------------------------------------------------
struct HleGxState {
    std::uint32_t gen_mode = 0;   // BP 0x00
    std::uint32_t bp_mask = 0xFF; // BP 0x0F
    std::uint32_t lp_size = 0;    // BP 0x22
    std::uint32_t su_ts0[8]{};    // BP 0x30 + 2n (size bits only meaningful while manual)
    std::uint32_t su_ts1[8]{};    // BP 0x31 + 2n
    std::uint8_t tcs_man_enab = 0;
    std::uint32_t tref[8]{};      // BP 0x28..0x2F
    std::uint32_t iref = 0;       // BP 0x27
    std::uint32_t ind_scale[2]{}; // BP 0x25, 0x26
    std::uint32_t tevc[kTevStages]{}; // BP 0xC0 + 2n
    std::uint32_t teva[kTevStages]{}; // BP 0xC1 + 2n
    std::uint32_t ksel[8]{};      // BP 0xF6..0xFD
    std::uint32_t cmode0 = 0;     // BP 0x41
    std::uint32_t cmode1 = 0;     // BP 0x42
    std::uint32_t zmode = 0;      // BP 0x40
    std::uint32_t pe_ctrl = 0;    // BP 0x43
    std::uint32_t vcd_lo = 0;     // CP 0x50
    std::uint32_t vcd_hi = 0;     // CP 0x60
    bool has_nrms = false;
    bool has_binrms = false;
    std::uint32_t nrm_type = 0;
    std::uint32_t vat_a[8]{};     // CP 0x70 + fmt
    std::uint32_t vat_b[8]{};     // CP 0x80 + fmt
    std::uint32_t vat_c[8]{};     // CP 0x90 + fmt
    // CP 0x30/0x40 (XF 0x1018/0x1019). GXInit points every texgen at GX_IDENTITY; start there
    // so a texgen used before GXInit's own calls reach the FIFO still reads identity.
    std::uint32_t mat_idx_a = (kTexMtxIdentity << 6) | (kTexMtxIdentity << 12) | (kTexMtxIdentity << 18) |
                              (kTexMtxIdentity << 24);
    std::uint32_t mat_idx_b = kTexMtxIdentity | (kTexMtxIdentity << 6) | (kTexMtxIdentity << 12) |
                              (kTexMtxIdentity << 18);
    std::uint32_t amb_color[2]{}; // XF 0x100A/0x100B, RGBA
    std::uint32_t mat_color[2]{}; // XF 0x100C/0x100D, RGBA
    // TLUT regions (GXInit defaults, see tlut_tmem_field): the BP 0x98-layout TLUT register a
    // texture using this name will be given - fmt << 10 | TMEM offset - as GXLoadTlut left it.
    std::uint32_t tlut_word[kTlutNames]{};
    // GXGetProjectionv / GXSetProjectionv save and restore the projection as a 7-float vector:
    // [type, m00, m02|m03, m11, m12|m13, m22, m23]. nw4r::lyt brackets its 2D passes with exactly
    // that pair, so dropping either one leaves the previous pass's 3D perspective in XF.
    float proj_vec[7] = {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f};
    float viewport[6] = {0.0f, 0.0f, 640.0f, 528.0f, 0.0f, 1.0f};
    std::uint32_t scissor[4] = {0, 0, 640, 528};
    std::int32_t scissor_off[2] = {0, 0};
    std::uint32_t dirty = 0;
    std::uint8_t dirty_vat = 0;
};

HleGxState g_gx{};

// GXInit ran before the HLE FIFO was bound: its register stream is emitted on first FIFO use.
bool g_init_pending = false;
bool g_in_init_sequence = false;

// Callbacks the game installs. Stored so the setters return the previous one, as the SDK does.
std::uint32_t g_draw_done_cb = 0;
std::uint32_t g_tex_region_cb = 0;
std::uint32_t g_tlut_region_cb = 0;

struct DisplayListState {
    std::uint32_t base = 0;
    std::uint32_t size = 0;
    bool active = false;
};

DisplayListState g_dl{};
// The live FIFO and GXData shadow as they were at GXBeginDisplayList. The live FIFO is moved
// aside so the list is recorded from offset 0 with the whole buffer available, then put back.
mkwv::gx::Fifo g_dl_saved_fifo{};
HleGxState g_dl_saved_gx{};

void run_gx_init_sequence();

mkwv::gx::Fifo* raw_fifo() {
    mkwv::runtime::HleFifo* hle = mkwv::runtime::guest_fifo();
    return hle != nullptr ? &hle->gx() : nullptr;
}

mkwv::gx::Fifo* fifo() {
    mkwv::gx::Fifo* f = raw_fifo();
    if (f != nullptr && g_init_pending && !g_in_init_sequence) {
        g_init_pending = false;
        run_gx_init_sequence();
    }
    return f;
}

void emit_bp(std::uint32_t reg, std::uint32_t value) {
    if (mkwv::gx::Fifo* f = fifo()) {
        f->load_bp(static_cast<std::uint8_t>(reg), value & 0x00FFFFFFu);
    }
}

void emit_cp(std::uint32_t addr, std::uint32_t value) {
    if (mkwv::gx::Fifo* f = fifo()) {
        f->load_cp(static_cast<std::uint8_t>(addr), value);
    }
}

void emit_xf(std::uint32_t addr, std::uint32_t value) {
    if (mkwv::gx::Fifo* f = fifo()) {
        f->load_xf(static_cast<std::uint16_t>(addr), &value, 1);
    }
}

void emit_xf_f32(std::uint32_t addr, const float* values, std::uint32_t count) {
    if (mkwv::gx::Fifo* f = fifo()) {
        f->load_xf_f32(static_cast<std::uint16_t>(addr), values, count);
    }
}

// __GXFlushTextureState (Aurora GXTexture.cpp): rewrites the indirect texture mask.
void flush_texture_state() {
    emit_bp(0x0F, g_gx.bp_mask);
}

// __GXUpdateBPMask (Aurora GXManage.cpp): the texmaps the active indirect stages read.
void update_bp_mask() {
    const std::uint32_t n_ind = get_field(g_gx.gen_mode, 3, 16);
    std::uint32_t mask = 0;
    for (std::uint32_t i = 0; i < n_ind; ++i) {
        mask |= 1u << get_field(g_gx.iref, 3, i * 6u);
    }
    if ((g_gx.bp_mask & 0xFFu) != mask) {
        set_field(g_gx.bp_mask, 8, 0, mask);
        emit_bp(0x0F, g_gx.bp_mask);
    }
}

// __GXSetVCD (Aurora GXManage.cpp): CP VCD lo/hi plus the XF vertex spec (XF 0x1008).
void emit_vcd() {
    emit_cp(0x50, g_gx.vcd_lo);
    emit_cp(0x60, g_gx.vcd_hi);
    std::uint32_t n_cols = (get_field(g_gx.vcd_lo, 2, 13) != 0 ? 1u : 0u) + (get_field(g_gx.vcd_lo, 2, 15) != 0 ? 1u : 0u);
    const std::uint32_t n_nrm = g_gx.has_binrms ? 2u : (g_gx.has_nrms ? 1u : 0u);
    std::uint32_t n_tex = 0;
    for (unsigned i = 0; i < 8u; ++i) {
        if (get_field(g_gx.vcd_hi, 2, i * 2u) != 0) {
            ++n_tex;
        }
    }
    emit_xf(0x1008, n_cols | (n_nrm << 2) | (n_tex << 4));
}

// __GXSetDirtyState (Aurora GXManage.cpp). The SDK defers genMode, VCD and VAT to the next
// GXBegin / GXCallDisplayList / display-list boundary; so does this. SU texture scale (bit 0)
// is derived by the decoder from the bound textures and is not emitted.
void flush_dirty() {
    if (g_gx.dirty == 0 || fifo() == nullptr) {
        return;
    }
    const std::uint32_t dirty = g_gx.dirty;
    g_gx.dirty = 0;
    if ((dirty & kDirtyBpMask) != 0) {
        update_bp_mask();
    }
    if ((dirty & kDirtyGenMode) != 0) {
        emit_bp(0x00, g_gx.gen_mode);
    }
    if ((dirty & kDirtyVcd) != 0) {
        emit_vcd();
    }
    if ((dirty & kDirtyVat) != 0) {
        for (unsigned i = 0; i < 8u; ++i) {
            if ((g_gx.dirty_vat & (1u << i)) != 0) {
                emit_cp(0x70u + i, g_gx.vat_a[i]);
                emit_cp(0x80u + i, g_gx.vat_b[i]);
                emit_cp(0x90u + i, g_gx.vat_c[i]);
            }
        }
        g_gx.dirty_vat = 0;
    }
}

// __GXSetMatrixIndex (Aurora GXManage.cpp): CP MATINDEX then its XF copy.
void emit_matrix_index(bool reg_b) {
    if (!reg_b) {
        emit_cp(0x30, g_gx.mat_idx_a);
        emit_xf(0x1018, g_gx.mat_idx_a);
    } else {
        emit_cp(0x40, g_gx.mat_idx_b);
        emit_xf(0x1019, g_gx.mat_idx_b);
    }
}

// GXInitTlutRegion defaults from GXInit: GX_TLUT0..15 at TMEM 0xC0000 + 0x2000 * n (256
// entries), GX_BIGTLUT0..3 at 0xE0000 + 0x8000 * n (1024 entries). The register field is the
// TMEM address relative to 0x80000 in 512-byte units.
std::uint32_t tlut_tmem_field(std::uint32_t name) {
    const std::uint32_t tmem = name < 16u ? 0xC0000u + 0x2000u * name : 0xE0000u + 0x8000u * (name - 16u);
    return ((tmem - 0x80000u) >> 9) & 0x3FFu;
}

void reset_shadows() {
    g_gx = HleGxState{};
    for (unsigned i = 0; i < 8u; ++i) {
        g_gx.vat_a[i] = 1u << 30;  // BYTEDEQUANT (Aurora GXInit)
        g_gx.vat_b[i] = 1u << 31;  // VCACHE_ENHANCE
    }
    for (unsigned n = 0; n < kTlutNames; ++n) {
        g_gx.tlut_word[n] = tlut_tmem_field(n);
    }
}

} // namespace

// All GX natives are extern "C" so native_cpu_calls.inc and the g_natives table can name them.
extern "C" {

// Bound to the SDK-internal helpers (__GXSetGenMode, __SetSURegs, ...) that only SDK code we
// replace ever calls, plus the GP-performance and sync functions this backend has no use for.
void GX_NoOp(CpuContext*) {}

// =============================================================================================
// Vertex format (Aurora GXGeometry.cpp / GXGet.cpp, SDK GXAttr.c)
// =============================================================================================

} // extern "C"

namespace {

void set_vcd_attr(std::uint32_t attr, std::uint32_t type) {
    type &= 3u;
    if (attr <= kVaTex7MtxIdx) {
        set_field(g_gx.vcd_lo, 1, attr, type & 1u);
    } else if (attr == kVaPos) {
        set_field(g_gx.vcd_lo, 2, 9, type);
    } else if (attr == kVaNrm) {
        if (type != 0) {
            g_gx.has_nrms = true;
            g_gx.has_binrms = false;
            g_gx.nrm_type = type;
        } else {
            g_gx.has_nrms = false;
        }
    } else if (attr == kVaNbt) {
        if (type != 0) {
            g_gx.has_binrms = true;
            g_gx.has_nrms = false;
            g_gx.nrm_type = type;
        } else {
            g_gx.has_binrms = false;
        }
    } else if (attr == kVaClr0) {
        set_field(g_gx.vcd_lo, 2, 13, type);
    } else if (attr == kVaClr1) {
        set_field(g_gx.vcd_lo, 2, 15, type);
    } else if (attr >= kVaTex0 && attr <= kVaTex7) {
        set_field(g_gx.vcd_hi, 2, (attr - kVaTex0) * 2u, type);
    }
}

void finish_vcd() {
    set_field(g_gx.vcd_lo, 2, 11, (g_gx.has_nrms || g_gx.has_binrms) ? g_gx.nrm_type : 0u);
    g_gx.dirty |= kDirtyVcd;
}

std::uint32_t get_vcd_attr(std::uint32_t attr) {
    if (attr <= kVaTex7MtxIdx) {
        return get_field(g_gx.vcd_lo, 1, attr);
    }
    if (attr == kVaPos) {
        return get_field(g_gx.vcd_lo, 2, 9);
    }
    if (attr == kVaNrm) {
        return g_gx.has_nrms ? get_field(g_gx.vcd_lo, 2, 11) : 0u;
    }
    if (attr == kVaNbt) {
        return g_gx.has_binrms ? get_field(g_gx.vcd_lo, 2, 11) : 0u;
    }
    if (attr == kVaClr0) {
        return get_field(g_gx.vcd_lo, 2, 13);
    }
    if (attr == kVaClr1) {
        return get_field(g_gx.vcd_lo, 2, 15);
    }
    if (attr >= kVaTex0 && attr <= kVaTex7) {
        return get_field(g_gx.vcd_hi, 2, (attr - kVaTex0) * 2u);
    }
    return 0;
}

void set_vat(unsigned fmt, std::uint32_t attr, std::uint32_t cnt, std::uint32_t type, std::uint32_t frac) {
    std::uint32_t& va = g_gx.vat_a[fmt];
    std::uint32_t& vb = g_gx.vat_b[fmt];
    std::uint32_t& vc = g_gx.vat_c[fmt];
    frac &= 0xFFu;
    switch (attr) {
    case kVaPos:
        set_field(va, 1, 0, cnt);
        set_field(va, 3, 1, type);
        set_field(va, 5, 4, frac);
        break;
    case kVaNrm:
    case kVaNbt:
        set_field(va, 3, 10, type);
        if (cnt == kNrmNbt3) {
            set_field(va, 1, 9, 1);
            set_field(va, 1, 31, 1);
        } else {
            set_field(va, 1, 9, cnt);
            set_field(va, 1, 31, 0);
        }
        break;
    case kVaClr0:
        set_field(va, 1, 13, cnt);
        set_field(va, 3, 14, type);
        break;
    case kVaClr1:
        set_field(va, 1, 17, cnt);
        set_field(va, 3, 18, type);
        break;
    case kVaTex0:
        set_field(va, 1, 21, cnt);
        set_field(va, 3, 22, type);
        set_field(va, 5, 25, frac);
        break;
    case kVaTex0 + 1:
        set_field(vb, 1, 0, cnt);
        set_field(vb, 3, 1, type);
        set_field(vb, 5, 4, frac);
        break;
    case kVaTex0 + 2:
        set_field(vb, 1, 9, cnt);
        set_field(vb, 3, 10, type);
        set_field(vb, 5, 13, frac);
        break;
    case kVaTex0 + 3:
        set_field(vb, 1, 18, cnt);
        set_field(vb, 3, 19, type);
        set_field(vb, 5, 22, frac);
        break;
    case kVaTex0 + 4:
        set_field(vb, 1, 27, cnt);
        set_field(vb, 3, 28, type);
        set_field(vc, 5, 0, frac);
        break;
    case kVaTex0 + 5:
        set_field(vc, 1, 5, cnt);
        set_field(vc, 3, 6, type);
        set_field(vc, 5, 9, frac);
        break;
    case kVaTex0 + 6:
        set_field(vc, 1, 14, cnt);
        set_field(vc, 3, 15, type);
        set_field(vc, 5, 18, frac);
        break;
    case kVaTex0 + 7:
        set_field(vc, 1, 23, cnt);
        set_field(vc, 3, 24, type);
        set_field(vc, 5, 27, frac);
        break;
    default:
        return;
    }
    g_gx.dirty |= kDirtyVat;
    g_gx.dirty_vat |= static_cast<std::uint8_t>(1u << fmt);
}

void get_vat(unsigned fmt, std::uint32_t attr, std::uint32_t* cnt, std::uint32_t* type, std::uint32_t* frac) {
    const std::uint32_t va = g_gx.vat_a[fmt];
    const std::uint32_t vb = g_gx.vat_b[fmt];
    const std::uint32_t vc = g_gx.vat_c[fmt];
    *frac = 0;
    switch (attr) {
    case kVaPos:
        *cnt = get_field(va, 1, 0);
        *type = get_field(va, 3, 1);
        *frac = get_field(va, 5, 4);
        return;
    case kVaNrm:
    case kVaNbt:
        *cnt = get_field(va, 1, 9);
        if (*cnt == 1u && get_field(va, 1, 31) != 0) {
            *cnt = kNrmNbt3;
        }
        *type = get_field(va, 3, 10);
        return;
    case kVaClr0:
        *cnt = get_field(va, 1, 13);
        *type = get_field(va, 3, 14);
        return;
    case kVaClr1:
        *cnt = get_field(va, 1, 17);
        *type = get_field(va, 3, 18);
        return;
    case kVaTex0:
        *cnt = get_field(va, 1, 21);
        *type = get_field(va, 3, 22);
        *frac = get_field(va, 5, 25);
        return;
    case kVaTex0 + 1:
        *cnt = get_field(vb, 1, 0);
        *type = get_field(vb, 3, 1);
        *frac = get_field(vb, 5, 4);
        return;
    case kVaTex0 + 2:
        *cnt = get_field(vb, 1, 9);
        *type = get_field(vb, 3, 10);
        *frac = get_field(vb, 5, 13);
        return;
    case kVaTex0 + 3:
        *cnt = get_field(vb, 1, 18);
        *type = get_field(vb, 3, 19);
        *frac = get_field(vb, 5, 22);
        return;
    case kVaTex0 + 4:
        *cnt = get_field(vb, 1, 27);
        *type = get_field(vb, 3, 28);
        *frac = get_field(vc, 5, 0);
        return;
    case kVaTex0 + 5:
        *cnt = get_field(vc, 1, 5);
        *type = get_field(vc, 3, 6);
        *frac = get_field(vc, 5, 9);
        return;
    case kVaTex0 + 6:
        *cnt = get_field(vc, 1, 14);
        *type = get_field(vc, 3, 15);
        *frac = get_field(vc, 5, 18);
        return;
    case kVaTex0 + 7:
        *cnt = get_field(vc, 1, 23);
        *type = get_field(vc, 3, 24);
        *frac = get_field(vc, 5, 27);
        return;
    default:
        *cnt = 1;   // GX_TEX_ST
        *type = 0;  // GX_RGB565
        return;
    }
}

constexpr unsigned kMaxListEntries = 64;  // a runaway guard for unterminated guest lists

} // namespace

extern "C" {

void GX__SetVtxDesc_8016d3a4(uint32_t a, uint32_t t) {
    set_vcd_attr(a, t);
    finish_vcd();
}

// GXSetVtxDescv(const GXVtxDescList*): {u32 attr, u32 type} pairs ending at GX_VA_NULL.
void GX__SetVtxDescv_8016d608(uint32_t list) {
    if (list == 0) {
        return;
    }
    for (unsigned i = 0; i < kMaxListEntries; ++i, list += 8u) {
        const std::uint32_t attr = Memory::Read32(list);
        if (attr == kVaNull) {
            break;
        }
        set_vcd_attr(attr, Memory::Read32(list + 4u));
    }
    finish_vcd();
}

// GXClearVtxDesc: every attribute off except GX_VA_POS = GX_DIRECT, as the SDK does.
void GX__ClearVtxDesc_8016dc34() {
    g_gx.vcd_lo = 1u << 9;
    g_gx.vcd_hi = 0;
    g_gx.has_nrms = false;
    g_gx.has_binrms = false;
    g_gx.dirty |= kDirtyVcd;
}

void GX__SetVtxAttrFmt_8016dc68(uint32_t vf, uint32_t a, uint32_t c, uint32_t t, uint32_t fr) {
    if (vf >= 8u) {
        return;
    }
    set_vat(vf, a, c, t, fr);
}

// GXSetVtxAttrFmtv(fmt, const GXVtxAttrFmtList*): 16-byte {u32 attr, u32 cnt, u32 type, u8 frac}.
void GX__SetVtxAttrFmtv_8016de08(uint32_t vf, uint32_t list) {
    if (vf >= 8u || list == 0) {
        return;
    }
    for (unsigned i = 0; i < kMaxListEntries; ++i, list += 16u) {
        const std::uint32_t attr = Memory::Read32(list);
        if (attr == kVaNull) {
            break;
        }
        set_vat(vf, attr, Memory::Read32(list + 4u), Memory::Read32(list + 8u), Memory::Read8(list + 12u));
    }
}

// GXGetVtxDesc(attr, GXAttrType* type)
void GX__GetVtxDesc_8016d9f0(uint32_t attr, uint32_t out) {
    if (out != 0) {
        Memory::Write32(out, get_vcd_attr(attr));
    }
}

// GXGetVtxDescv(GXVtxDescList*): PNMTXIDX..TEX7, then NBT, then GX_VA_NULL (SDK / Aurora).
void GX__GetVtxDescv_8016dba4(uint32_t out) {
    if (out == 0) {
        return;
    }
    std::uint32_t attr = kVaPnMtxIdx;
    for (; attr <= kVaTex7; ++attr, out += 8u) {
        Memory::Write32(out, attr);
        Memory::Write32(out + 4u, get_vcd_attr(attr));
    }
    Memory::Write32(out, kVaNbt);
    Memory::Write32(out + 4u, get_vcd_attr(kVaNbt));
    Memory::Write32(out + 8u, kVaNull);
}

// GXGetVtxAttrFmt(fmt, attr, GXCompCnt*, GXCompType*, u8* frac)
void GX__GetVtxAttrFmt_8016e04c(uint32_t vf, uint32_t attr, uint32_t pcnt, uint32_t ptype, uint32_t pfrac) {
    std::uint32_t cnt = 1, type = 0, frac = 0;
    if (vf < 8u) {
        get_vat(vf, attr, &cnt, &type, &frac);
    }
    if (pcnt != 0) {
        Memory::Write32(pcnt, cnt);
    }
    if (ptype != 0) {
        Memory::Write32(ptype, type);
    }
    if (pfrac != 0) {
        Memory::Write8(pfrac, static_cast<std::uint8_t>(frac));
    }
}

// GXGetVtxAttrFmtv(fmt, GXVtxAttrFmtList*): POS..TEX7 then GX_VA_NULL.
void GX__GetVtxAttrFmtv_8016e2b8(uint32_t vf, uint32_t out) {
    if (vf >= 8u || out == 0) {
        return;
    }
    for (std::uint32_t attr = kVaPos; attr <= kVaTex7; ++attr, out += 16u) {
        std::uint32_t cnt = 0, type = 0, frac = 0;
        get_vat(vf, attr, &cnt, &type, &frac);
        Memory::Write32(out, attr);
        Memory::Write32(out + 4u, cnt);
        Memory::Write32(out + 8u, type);
        Memory::Write8(out + 12u, static_cast<std::uint8_t>(frac));
    }
    Memory::Write32(out, kVaNull);
}

// GXSetArray(attr, base, stride): CP 0xA0 + slot = physical base, CP 0xB0 + slot = stride.
// NBT shares the normal array; GX_POS_MTX_ARRAY..GX_LIGHT_ARRAY are slots 12..15.
void GX__SetArray_8016e32c(uint32_t attr, uint32_t base, uint32_t stride) {
    const std::uint32_t cp_attr = attr == kVaNbt ? kVaNrm : attr;
    if (cp_attr < kVaPos || cp_attr > kVaLightArray) {
        return;
    }
    const std::uint32_t slot = cp_attr - kVaPos;
    emit_cp(0xA0u + slot, phys(base));
    emit_cp(0xB0u + slot, stride & 0xFFu);
}

// GXInvalidateVtxCache: the single-byte CP command 0x48.
void GX__InvalidateVtxCache_8016e36c() {
    if (mkwv::gx::Fifo* f = fifo()) {
        f->write_u8(0x48);
    }
}

// =============================================================================================
// Texture coordinate generation (Aurora GXGeometry.cpp GXSetTexCoordGen2)
// XF 0x1040+n: proj bit1, input form bit2, type bits4-6 (0 regular, 1 emboss, 2/3 colour SRTG),
// source row bits7-11, emboss source bits12-14, emboss light bits15-17.
// XF 0x1050+n: post matrix (id - 64) bits0-5, normalize bit8.
// =============================================================================================

void GX__SetTexCoordGen2_8016e37c(uint32_t dst, uint32_t type, uint32_t src, uint32_t mtx, uint32_t normalize,
                                  uint32_t post) {
    if (dst >= 8u) {
        return;
    }
    std::uint32_t row = 5;
    std::uint32_t form = 0;
    std::uint32_t bump_row = 0;
    switch (src) {
    case 0: row = 0; form = 1; break;   // GX_TG_POS
    case 1: row = 1; form = 1; break;   // GX_TG_NRM
    case 2: row = 3; form = 1; break;   // GX_TG_BINRM
    case 3: row = 4; form = 1; break;   // GX_TG_TANGENT
    case 19:                            // GX_TG_COLOR0
    case 20: row = 2; break;            // GX_TG_COLOR1
    default:
        if (src >= 4u && src <= 11u) {  // GX_TG_TEX0..7
            row = 5u + (src - 4u);
        } else if (src >= 12u && src <= 18u) {  // GX_TG_TEXCOORD0..6 (bump sources)
            bump_row = src - 12u;
        }
        break;
    }
    std::uint32_t reg = 0;
    if (type == 1u) {  // GX_TG_MTX2x4
        set_field(reg, 1, 2, form);
        set_field(reg, 5, 7, row);
    } else if (type == 0u) {  // GX_TG_MTX3x4
        set_field(reg, 1, 1, 1);
        set_field(reg, 1, 2, form);
        set_field(reg, 5, 7, row);
    } else if (type >= 2u && type <= 9u) {  // GX_TG_BUMP0..7
        set_field(reg, 1, 2, form);
        set_field(reg, 3, 4, 1);
        set_field(reg, 5, 7, row);
        set_field(reg, 3, 12, bump_row);
        set_field(reg, 3, 15, type - 2u);
    } else if (type == 10u) {  // GX_TG_SRTG
        set_field(reg, 1, 2, form);
        set_field(reg, 3, 4, src == 19u ? 2u : 3u);
        set_field(reg, 5, 7, 2);
    }
    emit_xf(0x1040u + dst, reg);

    // The SDK takes GX_PTTEXMTX0..GX_PTIDENTITY (64..125). A zero here has only ever meant
    // "no post transform", so read it as GX_PTIDENTITY rather than wrapping to slot 0.
    const std::uint32_t post_id = post < 64u ? kPtTexMtxIdentity : post;
    std::uint32_t post_reg = 0;
    set_field(post_reg, 6, 0, post_id - 64u);
    set_field(post_reg, 1, 8, gx_bool(normalize) ? 1u : 0u);
    emit_xf(0x1050u + dst, post_reg);

    if (dst < 4u) {
        set_field(g_gx.mat_idx_a, 6, 6u + dst * 6u, mtx);
        emit_matrix_index(false);
    } else {
        set_field(g_gx.mat_idx_b, 6, (dst - 4u) * 6u, mtx);
        emit_matrix_index(true);
    }
}

void GX__SetNumTexGens_8016e5a4(uint32_t n) {
    set_field(g_gx.gen_mode, 4, 0, n);
    emit_xf(0x103F, n & 0xFFu);
    g_gx.dirty |= kDirtyGenMode;
}

// =============================================================================================
// Flush, sync and draw
// =============================================================================================

// GXDrawDone waits for the GP; here it is also the frame boundary. A display list being recorded
// must not be presented: its bytes are sitting in the live FIFO buffer until GXEndDisplayList.
void GX__DrawDone_8016eab0() {
    if (!g_dl.active) {
        mkwv::runtime::hle_present_frame();
    }
    Memory::Write8(kGxDrawDoneFlagAddr, 1);
}

// GXSetDrawDoneCallback(cb) -> previous. The callback is not invoked: GXDrawDone completes
// synchronously here and nothing in the game has been seen to depend on the interrupt.
void GX__SetDrawDoneCallback_8016ed50(CpuContext* ctx) {
    const std::uint32_t previous = g_draw_done_cb;
    g_draw_done_cb = ctx->gpr[3];
    ctx->gpr[3] = previous;
}

// __GXSetDirtyState: only SDK code calls it, but flushing is what it means.
void GX__SetDirtyState_8016ee78(CpuContext*) {
    flush_dirty();
}

// GXPixModeSync (Aurora GXManage.cpp): rewrites peCtrl so later PE state takes effect in order.
void GX__PixModeSync_8016eb70() {
    emit_bp(0x43, g_gx.pe_ctrl);
}

void GX__Begin_8016f0f0(uint32_t t, uint32_t vf, uint32_t nv) {
    // A command boundary: let the render thread start on what the frame has so far.
    mkwv::runtime::hle_fifo_early_handoff();
    flush_dirty();
    mkwv::gx::Fifo* f = fifo();
    if (f == nullptr) {
        return;
    }
    f->write_u8(static_cast<std::uint8_t>((t & 0xF8u) | (vf & 7u)));
    f->write_u16(static_cast<std::uint16_t>(nv));
}

// =============================================================================================
// Setup unit: line/point size, texture offsets, cull, coplanar (Aurora GXGeometry.cpp, GXCull.cpp)
// BP 0x22 SU_LPSIZE: line width 0-7, point size 8-15, line tex offset 16-18, point tex offset
// 19-21, line half-aspect 22.
// =============================================================================================

void GX__SetLineWidth_8016f314(uint32_t width, uint32_t tex_offsets) {
    set_field(g_gx.lp_size, 8, 0, width);
    set_field(g_gx.lp_size, 3, 16, tex_offsets);
    emit_bp(0x22, g_gx.lp_size);
}

void GX__SetPointSize_8016f348(uint32_t size, uint32_t tex_offsets) {
    set_field(g_gx.lp_size, 8, 8, size);
    set_field(g_gx.lp_size, 3, 19, tex_offsets);
    emit_bp(0x22, g_gx.lp_size);
}

} // extern "C"

namespace {

// The SU texture-coordinate scale registers as this backend writes them. On hardware
// __GXSetSUTexRegs refills the size fields from the bound texture before every draw; the decoder
// derives that itself, so the size fields are only sent for a manually scaled coordinate, and
// bit 20 (unused by the hardware) marks those: "use bits 0-15 as (scale - 1)". Bits 16-19 (bias,
// cylindrical wrap, line/point offsets) are always the shadow's.
constexpr std::uint32_t kSuManualMarker = 1u << 20;

std::uint32_t su_reg_value(const std::uint32_t& shadow, unsigned coord) {
    if ((g_gx.tcs_man_enab & (1u << coord)) != 0) {
        return (shadow & 0xFFFFFu) | kSuManualMarker;
    }
    return shadow & 0xF0000u;
}

void emit_su_ts0(unsigned coord) {
    emit_bp(0x30u + coord * 2u, su_reg_value(g_gx.su_ts0[coord], coord));
}

void emit_su_ts1(unsigned coord) {
    emit_bp(0x31u + coord * 2u, su_reg_value(g_gx.su_ts1[coord], coord));
}

} // namespace

extern "C" {

// GXEnableTexOffsets(coord, line_enable, point_enable): suTs0 bits 18/19.
void GX__EnableTexOffsets_8016f37c(uint32_t coord, uint32_t line_enable, uint32_t point_enable) {
    if (coord >= 8u) {
        return;
    }
    set_field(g_gx.su_ts0[coord], 1, 18, gx_bool(line_enable) ? 1u : 0u);
    set_field(g_gx.su_ts0[coord], 1, 19, gx_bool(point_enable) ? 1u : 0u);
    emit_su_ts0(coord);
}

// GXSetCullMode: genMode bits 14-15, with front/back swapped into the hardware's convention.
void GX__SetCullMode_8016f3b8(uint32_t m) {
    std::uint32_t hw = m & 3u;
    if (hw == 1u) {
        hw = 2u;
    } else if (hw == 2u) {
        hw = 1u;
    }
    set_field(g_gx.gen_mode, 2, 14, hw);
    g_gx.dirty |= kDirtyGenMode;
}

// GXSetCoPlanar: genMode bit 19, sent at once behind a BP write mask (0xFE) selecting that bit.
void GX__SetCoPlanar_8016f3e0(uint32_t enable) {
    set_field(g_gx.gen_mode, 1, 19, gx_bool(enable) ? 1u : 0u);
    emit_bp(0xFE, 1u << 19);
    emit_bp(0x00, g_gx.gen_mode);
}

// =============================================================================================
// EFB -> texture copies.
//
// MKW snapshots the rendered menu into a texture with GXCopyTex and draws the dialog over it.
// GXCopyTex writes an escape into the FIFO so the copy keeps its position among the draws
// ("texture copies observe all prior draws"); the renderer performs it when it reaches it.
// =============================================================================================

} // extern "C"

namespace {
mkwv::gx::TexCopySetup g_tex_copy;
}

extern "C" {

void GX__SetTexCopySrc_8016f478(uint32_t l, uint32_t t, uint32_t w, uint32_t h) {
    g_tex_copy.src_x = static_cast<std::uint16_t>(l);
    g_tex_copy.src_y = static_cast<std::uint16_t>(t);
    g_tex_copy.src_w = static_cast<std::uint16_t>(w);
    g_tex_copy.src_h = static_cast<std::uint16_t>(h);
}

void GX__SetTexCopyDst_8016f4dc(uint32_t w, uint32_t h, uint32_t fmt, uint32_t mipmap) {
    g_tex_copy.dst_w = static_cast<std::uint16_t>(w);
    g_tex_copy.dst_h = static_cast<std::uint16_t>(h);
    g_tex_copy.format = fmt;
    g_tex_copy.mipmap = gx_bool(mipmap);
}

void GX__SetCopyClamp_8016f618(uint32_t clamp) {
    g_tex_copy.clamp_top = (clamp & 1u) != 0;
    g_tex_copy.clamp_bottom = (clamp & 2u) != 0;
}

void GX__CopyTex_8016fd74(uint32_t dest, uint32_t clear) {
    mkwv::gx::Fifo* f = fifo();
    if (f == nullptr || g_tex_copy.dst_w == 0 || g_tex_copy.dst_h == 0) {
        return;
    }
    // Resolve only the first byte: a full-screen copy is over a megabyte and CompactMemory
    // refuses any range crossing its 1 MB page; MEM1/MEM2 backing is contiguous anyway.
    const std::uint8_t* host = Memory::GetPointer(dest, 1);
    mkwv::gx::write_efb_copy_escape(*f, g_tex_copy, gx_bool(clear), dest, host);
}

// =============================================================================================
// Lighting (Aurora GXLighting.cpp). GXLightObj: +0x0C colour RGBA, +0x10 a0..a2, +0x1C k0..k2,
// +0x28 position, +0x34 direction (stored negated, as XF wants it).
// =============================================================================================

void GX__InitLightAttn_8016ff04(uint32_t obj, float a0, float a1, float a2, float k0, float k1, float k2) {
    if (obj == 0) {
        return;
    }
    write_guest_f32(obj + 0x10u, a0);
    write_guest_f32(obj + 0x14u, a1);
    write_guest_f32(obj + 0x18u, a2);
    write_guest_f32(obj + 0x1Cu, k0);
    write_guest_f32(obj + 0x20u, k1);
    write_guest_f32(obj + 0x24u, k2);
}

void GX__InitLightAttnA_8016ff20(uint32_t obj, float a0, float a1, float a2) {
    if (obj == 0) {
        return;
    }
    write_guest_f32(obj + 0x10u, a0);
    write_guest_f32(obj + 0x14u, a1);
    write_guest_f32(obj + 0x18u, a2);
}

void GX__InitLightAttnK_8016ff30(uint32_t obj, float k0, float k1, float k2) {
    if (obj == 0) {
        return;
    }
    write_guest_f32(obj + 0x1Cu, k0);
    write_guest_f32(obj + 0x20u, k1);
    write_guest_f32(obj + 0x24u, k2);
}

void GX__InitLightPos_80170198(uint32_t obj, float x, float y, float z) {
    if (obj == 0) {
        return;
    }
    write_guest_f32(obj + 0x28u, x);
    write_guest_f32(obj + 0x2Cu, y);
    write_guest_f32(obj + 0x30u, z);
}

void GX__GetLightPos_801701a8(uint32_t obj, uint32_t px, uint32_t py, uint32_t pz) {
    if (obj == 0) {
        return;
    }
    if (px != 0) Memory::Write32(px, Memory::Read32(obj + 0x28u));
    if (py != 0) Memory::Write32(py, Memory::Read32(obj + 0x2Cu));
    if (pz != 0) Memory::Write32(pz, Memory::Read32(obj + 0x30u));
}

void GX__InitLightDir_801701c4(uint32_t obj, float nx, float ny, float nz) {
    if (obj == 0) {
        return;
    }
    write_guest_f32(obj + 0x34u, -nx);
    write_guest_f32(obj + 0x38u, -ny);
    write_guest_f32(obj + 0x3Cu, -nz);
}

void GX__GetLightDir_801701e0(uint32_t obj, uint32_t px, uint32_t py, uint32_t pz) {
    if (obj == 0) {
        return;
    }
    if (px != 0) write_guest_f32(px, -read_guest_f32(obj + 0x34u));
    if (py != 0) write_guest_f32(py, -read_guest_f32(obj + 0x38u));
    if (pz != 0) write_guest_f32(pz, -read_guest_f32(obj + 0x3Cu));
}

// GXInitLightColor(obj, GXColor): the by-value struct arrives as a pointer to the caller's copy.
void GX__InitLightColor_80170314(uint32_t obj, uint32_t color_ptr) {
    if (obj == 0 || color_ptr == 0) {
        return;
    }
    Memory::Write32(obj + 0x0Cu, Memory::Read32(color_ptr));
}

// GXLoadLightObjImm(obj, GXLightID): one 16-word XF write at 0x600 + 16 * index - the object is
// copied verbatim (3 reserved words, colour, a0-a2, k0-k2, pos, dir). The index is the light
// mask's highest set bit (31 - cntlzw, as the SDK computes it).
void GX__LoadLightObjImm_80170320(uint32_t la, uint32_t lid) {
    mkwv::gx::Fifo* f = fifo();
    if (f == nullptr || la == 0 || (lid & 0xFFu) == 0) {
        return;
    }
    unsigned idx = 7;
    while ((lid & (1u << idx)) == 0u) {
        --idx;
    }
    std::uint32_t words[16]{};
    for (unsigned i = 0; i < 13u; ++i) {
        words[3u + i] = Memory::Read32(la + 0x0Cu + i * 4u);
    }
    f->load_xf(static_cast<std::uint16_t>(0x600u + idx * 16u), words, 16);
}

// =============================================================================================
// Colour channels (SDK GXLight.c; Aurora GXLighting.cpp)
// XF 0x100A/B ambient, 0x100C/D material (RGBA). GX_COLORn replaces RGB and keeps the shadow's
// alpha, GX_ALPHAn replaces only alpha, GX_COLORnAn replaces the whole word.
// =============================================================================================

} // extern "C"

namespace {

void set_chan_color(std::uint32_t* shadow, std::uint32_t xf_base, std::uint32_t chan, std::uint32_t rgba) {
    std::uint32_t idx = 0;
    std::uint32_t reg = 0;
    switch (chan) {
    case 0:  // GX_COLOR0
    case 1:  // GX_COLOR1
        idx = chan;
        reg = (shadow[idx] & 0xFFu) | (rgba & 0xFFFFFF00u);
        break;
    case 2:  // GX_ALPHA0
    case 3:  // GX_ALPHA1
        idx = chan - 2u;
        reg = (shadow[idx] & 0xFFFFFF00u) | (rgba & 0xFFu);
        break;
    case 4:  // GX_COLOR0A0
    case 5:  // GX_COLOR1A1
        idx = chan - 4u;
        reg = rgba;
        break;
    default:
        return;
    }
    shadow[idx] = reg;
    emit_xf(xf_base + idx, reg);
}

} // namespace

extern "C" {

// GXColor is a struct passed by value: the register holds a pointer to the caller's copy.
void GX__SetChanAmbColor_8017039c(uint32_t c, uint32_t cp) {
    if (cp != 0) {
        set_chan_color(g_gx.amb_color, 0x100A, c, Memory::Read32(cp));
    }
}

void GX__SetChanMatColor_80170474(uint32_t c, uint32_t cp) {
    if (cp != 0) {
        set_chan_color(g_gx.mat_color, 0x100C, c, Memory::Read32(cp));
    }
}

void GX__SetNumChans_8017054c(uint32_t n) {
    set_field(g_gx.gen_mode, 3, 4, n);
    emit_xf(0x1009, n & 0xFFu);
    g_gx.dirty |= kDirtyGenMode;
}

// GXSetChanCtrl(chan, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn) -> XF 0x100E + n:
// mat src 0, enable 1, lights 0-3 in 2-5, amb src 6, diffuse 7-8 (none for GX_AF_SPEC),
// attenuation enable 9 (!= GX_AF_NONE), attenuation select 10 (!= GX_AF_SPEC), lights 4-7 in 11-14.
void GX__SetChanCtrl_80170570(uint32_t ch, uint32_t en, uint32_t as, uint32_t ms, uint32_t lm, uint32_t df,
                              uint32_t af) {
    std::uint32_t reg = 0;
    set_field(reg, 1, 0, ms & 1u);
    set_field(reg, 1, 1, gx_bool(en) ? 1u : 0u);
    set_field(reg, 4, 2, lm & 0xFu);
    set_field(reg, 1, 6, as & 1u);
    set_field(reg, 2, 7, af == kAfSpec ? 0u : (df & 3u));
    set_field(reg, 1, 9, af != kAfNone ? 1u : 0u);
    set_field(reg, 1, 10, af != kAfSpec ? 1u : 0u);
    set_field(reg, 4, 11, (lm >> 4) & 0xFu);
    if (ch <= 3u) {
        emit_xf(0x100Eu + ch, reg);
    } else if (ch == 4u) {  // GX_COLOR0A0
        emit_xf(0x100E, reg);
        emit_xf(0x1010, reg);
    } else if (ch == 5u) {  // GX_COLOR1A1
        emit_xf(0x100F, reg);
        emit_xf(0x1011, reg);
    }
}

// =============================================================================================
// Texture objects (SDK GXTexture.c; Aurora GXTexture.cpp)
//
// GXTexObj (0x20 bytes, read directly by translated code):
//   +0x00 mode0  (BP 0x80 layout) wrap_s 0-1, wrap_t 2-3, mag 4, min 5-7 (hw), !edge_lod 8,
//                 lod bias 9-16 (s8, x32), max aniso 19-20, bias clamp 21
//   +0x04 mode1  (BP 0x84 layout) min lod 0-7, max lod 8-15 (x16)
//   +0x08 image0 (BP 0x88 layout) w-1 0-9, h-1 10-19, fmt 20-23
//   +0x0C image3 (BP 0x94 layout) physical address >> 5
//   +0x10 userData, +0x14 GXTexFmt, +0x18 tlut name, +0x1C loadCnt u16, +0x1E loadFmt u8,
//   +0x1F flags (bit0 mipmap, bit1 "not colour-indexed" - no TLUT register is loaded)
// GXLoadTexObj writes the BP id into the top byte of mode0/mode1/image0/image3 in the object,
// exactly as the SDK does.
// =============================================================================================

} // extern "C"

namespace {

constexpr std::uint8_t kTexMode0Ids[8] = {0x80, 0x81, 0x82, 0x83, 0xA0, 0xA1, 0xA2, 0xA3};
constexpr std::uint8_t kTexMode1Ids[8] = {0x84, 0x85, 0x86, 0x87, 0xA4, 0xA5, 0xA6, 0xA7};
constexpr std::uint8_t kTexImage0Ids[8] = {0x88, 0x89, 0x8A, 0x8B, 0xA8, 0xA9, 0xAA, 0xAB};
constexpr std::uint8_t kTexImage1Ids[8] = {0x8C, 0x8D, 0x8E, 0x8F, 0xAC, 0xAD, 0xAE, 0xAF};
constexpr std::uint8_t kTexImage2Ids[8] = {0x90, 0x91, 0x92, 0x93, 0xB0, 0xB1, 0xB2, 0xB3};
constexpr std::uint8_t kTexImage3Ids[8] = {0x94, 0x95, 0x96, 0x97, 0xB4, 0xB5, 0xB6, 0xB7};
constexpr std::uint8_t kTexTlutIds[8] = {0x98, 0x99, 0x9A, 0x9B, 0xB8, 0xB9, 0xBA, 0xBB};
// GXTexFilter -> hardware: GX_NEAR, GX_LINEAR, GX_NEAR_MIP_NEAR, GX_LIN_MIP_NEAR, GX_NEAR_MIP_LIN,
// GX_LIN_MIP_LIN.
constexpr std::uint32_t kGx2HwFilt[6] = {0, 4, 1, 5, 2, 6};
constexpr std::uint32_t kHw2GxFilt[8] = {0, 2, 4, 0, 1, 3, 5, 0};

bool is_ci_format(std::uint32_t fmt) {
    return fmt == 8u || fmt == 9u || fmt == 10u;  // GX_TF_C4, GX_TF_C8, GX_TF_C14X2
}

// __GXGetTexTileShift: log2 of the tile width/height in texels for a (copy) texture format.
void tex_tile_shift(std::uint32_t fmt, std::uint32_t* sx, std::uint32_t* sy) {
    switch (fmt) {
    case 0x00: case 0x08: case 0x0E: case 0x20: case 0x30:  // I4 C4 CMPR CTF_R4 CTF_Z4
        *sx = 3; *sy = 3;
        return;
    case 0x01: case 0x02: case 0x09: case 0x11: case 0x22: case 0x27:  // I8 IA4 C8 Z8 RA4 A8
    case 0x28: case 0x29: case 0x2A: case 0x39: case 0x3A:            // R8 G8 B8 Z8M Z8L
        *sx = 3; *sy = 2;
        return;
    case 0x03: case 0x04: case 0x05: case 0x06: case 0x0A: case 0x13:  // IA8 565 5A3 RGBA8 C14X2 Z16
    case 0x16: case 0x23: case 0x2B: case 0x2C: case 0x3C:             // Z24X8 RA8 RG8 GB8 Z16L
        *sx = 2; *sy = 2;
        return;
    default:
        *sx = 0; *sy = 0;
        return;
    }
}

std::uint32_t floor_log2(std::uint32_t v) {
    std::uint32_t r = 0;
    while (v > 1u) {
        v >>= 1;
        ++r;
    }
    return r;
}

// s8 LOD bias field: 32 * bias clamped to [-4, 3.99] (SDK / Aurora GXInitTexObjLOD).
std::uint32_t lod_bias_field(float bias) {
    if (!(bias >= -4.0f)) {
        bias = -4.0f;
    } else if (bias >= 4.0f) {
        bias = 3.99f;
    }
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(32.0f * bias)) & 0xFFu;
}

std::uint32_t lod_field(float lod) {
    if (!(lod >= 0.0f)) {
        lod = 0.0f;
    } else if (lod > 10.0f) {
        lod = 10.0f;
    }
    return static_cast<std::uint32_t>(16.0f * lod) & 0xFFu;
}

void init_tex_obj(std::uint32_t oa, std::uint32_t data, std::uint32_t w, std::uint32_t h, std::uint32_t fmt,
                  std::uint32_t ws, std::uint32_t wt, bool mipmap) {
    w &= 0xFFFFu;
    h &= 0xFFFFu;
    for (unsigned i = 0; i < 8u; ++i) {
        Memory::Write32(oa + i * 4u, 0);
    }
    std::uint32_t mode0 = 0;
    std::uint32_t mode1 = 0;
    std::uint8_t flags = 0;
    set_field(mode0, 2, 0, ws);
    set_field(mode0, 2, 2, wt);
    set_field(mode0, 1, 4, 1);  // mag GX_LINEAR
    if (mipmap) {
        flags |= 1u;
        // Colour-indexed mipmaps cannot interpolate between levels: GX_LIN_MIP_NEAR.
        set_field(mode0, 3, 5, is_ci_format(fmt) ? 5u : 6u);
        set_field(mode1, 8, 8, (16u * floor_log2(w > h ? w : h)) & 0xFFu);
    } else {
        set_field(mode0, 3, 5, 4);  // GX_LINEAR
    }
    std::uint32_t image0 = 0;
    set_field(image0, 10, 0, w - 1u);
    set_field(image0, 10, 10, h - 1u);
    set_field(image0, 4, 20, fmt & 0xFu);
    const std::uint32_t image3 = (phys(data) >> 5) & 0x00FFFFFFu;

    // loadCnt/loadFmt are only read by the TMEM preload paths; filled in for completeness.
    std::uint32_t sx = 0, sy = 0;
    tex_tile_shift(fmt, &sx, &sy);
    const std::uint32_t tiles = ((w + (1u << sx) - 1u) >> sx) * ((h + (1u << sy) - 1u) >> sy);
    std::uint8_t load_fmt = 2;
    if (fmt == 0x0Eu) {
        load_fmt = 0;
    } else if (sx == 3u && sy == 3u) {
        load_fmt = 1;
    } else if (fmt == 6u) {
        load_fmt = 3;
    }

    if (!is_ci_format(fmt)) {
        flags |= 2u;
    }
    Memory::Write32(oa + 0x00u, mode0);
    Memory::Write32(oa + 0x04u, mode1);
    Memory::Write32(oa + 0x08u, image0);
    Memory::Write32(oa + 0x0Cu, image3);
    Memory::Write32(oa + 0x14u, fmt);
    Memory::Write32(oa + 0x18u, 0);
    Memory::Write16(oa + 0x1Cu, static_cast<std::uint16_t>(tiles & 0x7FFFu));
    Memory::Write8(oa + 0x1Eu, load_fmt);
    Memory::Write8(oa + 0x1Fu, flags);
}

// Writes the six texture-image registers (+ TLUT for colour-indexed objects) for texmap `id`.
// image1/image2 describe the TMEM cache regions; the decoder does not model TMEM caching, but
// the values are the SDK's (32 KB even/odd regions, width exponent 3).
void load_tex_obj_regs(std::uint32_t oa, std::uint32_t image1, std::uint32_t image2, std::uint32_t id) {
    if (oa == 0 || id >= 8u) {
        return;
    }
    const std::uint32_t mode0 = Memory::Read32(oa + 0x00u) & 0x00FFFFFFu;
    const std::uint32_t mode1 = Memory::Read32(oa + 0x04u) & 0x00FFFFFFu;
    const std::uint32_t image0 = Memory::Read32(oa + 0x08u) & 0x00FFFFFFu;
    const std::uint32_t image3 = Memory::Read32(oa + 0x0Cu) & 0x00FFFFFFu;
    Memory::Write32(oa + 0x00u, mode0 | (static_cast<std::uint32_t>(kTexMode0Ids[id]) << 24));
    Memory::Write32(oa + 0x04u, mode1 | (static_cast<std::uint32_t>(kTexMode1Ids[id]) << 24));
    Memory::Write32(oa + 0x08u, image0 | (static_cast<std::uint32_t>(kTexImage0Ids[id]) << 24));
    Memory::Write32(oa + 0x0Cu, image3 | (static_cast<std::uint32_t>(kTexImage3Ids[id]) << 24));
    emit_bp(kTexMode0Ids[id], mode0);
    emit_bp(kTexMode1Ids[id], mode1);
    emit_bp(kTexImage0Ids[id], image0);
    emit_bp(kTexImage1Ids[id], image1);
    emit_bp(kTexImage2Ids[id], image2);
    emit_bp(kTexImage3Ids[id], image3);
    if ((Memory::Read8(oa + 0x1Fu) & 2u) == 0) {
        const std::uint32_t name = Memory::Read32(oa + 0x18u);
        if (name < kTlutNames) {
            emit_bp(kTexTlutIds[id], g_gx.tlut_word[name]);
        }
    }
    g_gx.dirty |= kDirtySuTex;
}

} // namespace

extern "C" {

void GX__InitTexObj_801707f8(uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt,
                             uint32_t mipmap) {
    if (oa == 0) {
        return;
    }
    init_tex_obj(oa, da, w, h, f, ws, wt, gx_bool(mipmap));
}

// GXInitTexObjCI(obj, image, w, h, fmt, wrap_s, wrap_t, mipmap, tlut_name): the ninth argument
// is on the caller's stack at 8(r1).
void GX__InitTexObjCI_80170a04(CpuContext* ctx) {
    const std::uint32_t oa = ctx->gpr[3];
    if (oa == 0) {
        return;
    }
    init_tex_obj(oa, ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7], ctx->gpr[8], ctx->gpr[9],
                 gx_bool(ctx->gpr[10]));
    Memory::Write32(oa + 0x18u, Memory::Read32(ctx->gpr[1] + 8u));
    Memory::Write8(oa + 0x1Fu, static_cast<std::uint8_t>(Memory::Read8(oa + 0x1Fu) & ~2u));
}

// GXInitTexObjLOD(obj, min_filt, mag_filt, min_lod, max_lod, lod_bias, bias_clamp, do_edge_lod,
// max_aniso) - Aurora GXTexture.cpp.
void GX__InitTexObjLOD_80170a4c(uint32_t oa, uint32_t min_filter, uint32_t mag_filter, float min_lod, float max_lod,
                                float bias, uint32_t bias_clamp, uint32_t edge_lod, uint32_t max_aniso) {
    if (oa == 0) {
        return;
    }
    std::uint32_t mode0 = Memory::Read32(oa + 0x00u);
    std::uint32_t mode1 = Memory::Read32(oa + 0x04u);
    set_field(mode0, 8, 9, lod_bias_field(bias));
    set_field(mode0, 1, 4, mag_filter == 1u ? 1u : 0u);
    set_field(mode0, 3, 5, kGx2HwFilt[min_filter < 6u ? min_filter : 0u]);
    set_field(mode0, 1, 8, gx_bool(edge_lod) ? 0u : 1u);
    set_field(mode0, 1, 17, 0);
    set_field(mode0, 1, 18, 0);
    set_field(mode0, 2, 19, max_aniso);
    set_field(mode0, 1, 21, gx_bool(bias_clamp) ? 1u : 0u);
    set_field(mode1, 8, 0, lod_field(min_lod));
    set_field(mode1, 8, 8, lod_field(max_lod));
    Memory::Write32(oa + 0x00u, mode0);
    Memory::Write32(oa + 0x04u, mode1);
}

void GX__InitTexObjWrapMode_80170b50(uint32_t oa, uint32_t ws, uint32_t wt) {
    if (oa == 0) {
        return;
    }
    std::uint32_t mode0 = Memory::Read32(oa);
    set_field(mode0, 2, 0, ws);
    set_field(mode0, 2, 2, wt);
    Memory::Write32(oa, mode0);
}

void GX__InitTexObjTlut_80170b64(uint32_t oa, uint32_t tlut_name) {
    if (oa != 0) {
        Memory::Write32(oa + 0x18u, tlut_name);
    }
}

void GX__InitTexObjFilter_80170b6c(uint32_t oa, uint32_t min_filter, uint32_t mag_filter) {
    if (oa == 0) {
        return;
    }
    std::uint32_t mode0 = Memory::Read32(oa);
    set_field(mode0, 3, 5, kGx2HwFilt[min_filter < 6u ? min_filter : 0u]);
    set_field(mode0, 1, 4, mag_filter == 1u ? 1u : 0u);
    Memory::Write32(oa, mode0);
}

void GX__InitTexObjLODBias_80170b94(uint32_t oa, float bias) {
    if (oa == 0) {
        return;
    }
    std::uint32_t mode0 = Memory::Read32(oa);
    set_field(mode0, 8, 9, lod_bias_field(bias));
    Memory::Write32(oa, mode0);
}

void GX__InitTexObjUserData_80170be8(uint32_t oa, uint32_t user) {
    if (oa != 0) {
        Memory::Write32(oa + 0x10u, user);
    }
}

void GX__GetTexObjUserData_80170bf0(CpuContext* ctx) {
    ctx->gpr[3] = ctx->gpr[3] != 0 ? Memory::Read32(ctx->gpr[3] + 0x10u) : 0u;
}

// GXGetTexObjAll(obj, void** image, u16* w, u16* h, GXTexFmt*, GXTexWrapMode* s, GXTexWrapMode* t,
// GXBool* mipmap). The image pointer is the physical address, as GXGetTexObjData returns it.
void GX__GetTexObjAll_80170bf8(uint32_t oa, uint32_t pimage, uint32_t pw, uint32_t ph, uint32_t pfmt, uint32_t pws,
                               uint32_t pwt, uint32_t pmip) {
    if (oa == 0) {
        return;
    }
    const std::uint32_t mode0 = Memory::Read32(oa + 0x00u);
    const std::uint32_t image0 = Memory::Read32(oa + 0x08u);
    const std::uint32_t image3 = Memory::Read32(oa + 0x0Cu);
    if (pimage != 0) Memory::Write32(pimage, (image3 & 0x00FFFFFFu) << 5);
    if (pw != 0) Memory::Write16(pw, static_cast<std::uint16_t>(get_field(image0, 10, 0) + 1u));
    if (ph != 0) Memory::Write16(ph, static_cast<std::uint16_t>(get_field(image0, 10, 10) + 1u));
    if (pfmt != 0) Memory::Write32(pfmt, Memory::Read32(oa + 0x14u));
    if (pws != 0) Memory::Write32(pws, get_field(mode0, 2, 0));
    if (pwt != 0) Memory::Write32(pwt, get_field(mode0, 2, 2));
    if (pmip != 0) Memory::Write8(pmip, static_cast<std::uint8_t>(Memory::Read8(oa + 0x1Fu) & 1u));
}

void GX__GetTexObjWidth_80170c68(CpuContext* ctx) {
    ctx->gpr[3] = get_field(Memory::Read32(ctx->gpr[3] + 0x08u), 10, 0) + 1u;
}

void GX__GetTexObjHeight_80170c7c(CpuContext* ctx) {
    ctx->gpr[3] = get_field(Memory::Read32(ctx->gpr[3] + 0x08u), 10, 10) + 1u;
}

void GX__GetTexObjFmt_80170c90(CpuContext* ctx) {
    ctx->gpr[3] = Memory::Read32(ctx->gpr[3] + 0x14u);
}

void GX__GetTexObjWrapS_80170c98(CpuContext* ctx) {
    ctx->gpr[3] = get_field(Memory::Read32(ctx->gpr[3]), 2, 0);
}

void GX__GetTexObjWrapT_80170ca4(CpuContext* ctx) {
    ctx->gpr[3] = get_field(Memory::Read32(ctx->gpr[3]), 2, 2);
}

void GX__GetTexObjMipMap_80170cb0(CpuContext* ctx) {
    ctx->gpr[3] = Memory::Read8(ctx->gpr[3] + 0x1Fu) & 1u;
}

// GXGetTexObjLODAll(obj, min_filt*, mag_filt*, min_lod*, max_lod*, lod_bias*, bias_clamp*,
// do_edge_lod*, max_aniso*): the ninth argument is on the caller's stack at 8(r1).
void GX__GetTexObjLODAll_80170cbc(CpuContext* ctx) {
    const std::uint32_t oa = ctx->gpr[3];
    if (oa == 0) {
        return;
    }
    const std::uint32_t mode0 = Memory::Read32(oa + 0x00u);
    const std::uint32_t mode1 = Memory::Read32(oa + 0x04u);
    const std::uint32_t p_aniso = Memory::Read32(ctx->gpr[1] + 8u);
    if (ctx->gpr[4] != 0) Memory::Write32(ctx->gpr[4], kHw2GxFilt[get_field(mode0, 3, 5)]);
    if (ctx->gpr[5] != 0) Memory::Write32(ctx->gpr[5], get_field(mode0, 1, 4));
    if (ctx->gpr[6] != 0) write_guest_f32(ctx->gpr[6], static_cast<float>(get_field(mode1, 8, 0)) / 16.0f);
    if (ctx->gpr[7] != 0) write_guest_f32(ctx->gpr[7], static_cast<float>(get_field(mode1, 8, 8)) / 16.0f);
    if (ctx->gpr[8] != 0) {
        const auto bias = static_cast<std::int8_t>(get_field(mode0, 8, 9));
        write_guest_f32(ctx->gpr[8], static_cast<float>(bias) / 32.0f);
    }
    if (ctx->gpr[9] != 0) Memory::Write8(ctx->gpr[9], static_cast<std::uint8_t>(get_field(mode0, 1, 21)));
    if (ctx->gpr[10] != 0) Memory::Write8(ctx->gpr[10], get_field(mode0, 1, 8) != 0 ? 0u : 1u);
    if (p_aniso != 0) Memory::Write32(p_aniso, get_field(mode0, 2, 19));
}

// 0x80170DA0 sits between GXGetTexObjLODAll and the three-instruction GXGetTexObjMagFilt
// (0x80170DB4, translated); five instructions with a small-data table lookup is
// GXGetTexObjMinFilt: HW2GXFiltConv[mode0 bits 5-7].
void GX__GetTexObjMinFilt_80170da0(CpuContext* ctx) {
    ctx->gpr[3] = kHw2GxFilt[get_field(Memory::Read32(ctx->gpr[3]), 3, 5)];
}

void GX__GetTexObjTlut_80170dc0(CpuContext* ctx) {
    ctx->gpr[3] = Memory::Read32(ctx->gpr[3] + 0x18u);
}

// GXLoadTexObjPreLoaded(obj, region, id): the TMEM region (GXInitTexCacheRegion, translated) holds
// image1 at +0 and image2 at +4.
void GX__LoadTexObjPreLoaded_80170dc8(uint32_t oa, uint32_t region, uint32_t id) {
    std::uint32_t image1 = 0;
    std::uint32_t image2 = 0;
    if (region != 0) {
        image1 = Memory::Read32(region) & 0x00FFFFFFu;
        image2 = Memory::Read32(region + 4u) & 0x00FFFFFFu;
    }
    load_tex_obj_regs(oa, image1, image2, id);
}

// GXLoadTexObj(obj, id): the default region callback gives texmap n the n-th 32 KB even/odd
// cache pair (GXInit: even i * 0x8000, odd 0x80000 + i * 0x8000).
void GX__LoadTexObj_80170f2c(uint32_t oa, uint32_t id) {
    if (id >= 8u) {
        return;
    }
    const std::uint32_t even = id * 0x8000u;
    const std::uint32_t odd = 0x80000u + id * 0x8000u;
    const std::uint32_t image1 = (even >> 5) | (3u << 15) | (3u << 18);
    const std::uint32_t image2 = (odd >> 5) | (3u << 15) | (3u << 18);
    load_tex_obj_regs(oa, image1, image2, id);
}

// GXInitTlutObj(obj, lut, fmt, n_entries). GXTlutObj: +0 tlut (fmt at bits 10-11, BP 0x98
// layout), +4 loadTlut0 (BP 0x64: physical address >> 5), +8 u16 entries.
void GX__InitTlutObj_80170f80(uint32_t obj, uint32_t lut, uint32_t fmt, uint32_t n_entries) {
    if (obj == 0) {
        return;
    }
    std::uint32_t tlut = 0;
    set_field(tlut, 2, 10, fmt);
    const std::uint32_t load0 = (0x64u << 24) | ((phys(lut) >> 5) & 0x00FFFFFFu);
    Memory::Write32(obj + 0x00u, tlut);
    Memory::Write32(obj + 0x04u, load0);
    Memory::Write16(obj + 0x08u, static_cast<std::uint16_t>(n_entries));
    Memory::Write16(obj + 0x0Au, 0);
}

// GXLoadTlut(obj, name): BP 0x64 (source address) then BP 0x65 (TMEM offset 0-9, 16-entry lines
// 10-20), bracketed by __GXFlushTextureState. The region is the GXInit default for `name`; the
// line count is the object's entry count rounded up to 16 (the SDK sends the region's size).
// Records fmt | TMEM offset for texture objects that name this TLUT.
void GX__LoadTlut_80170fa8(uint32_t obj, uint32_t name) {
    if (obj == 0 || name >= kTlutNames) {
        return;
    }
    const std::uint32_t tmem = tlut_tmem_field(name);
    const std::uint32_t entries = Memory::Read16(obj + 0x08u);
    std::uint32_t lines = (entries + 15u) / 16u;
    if (lines > 0x7FFu) {
        lines = 0x7FFu;
    }
    flush_texture_state();
    emit_bp(0x64, Memory::Read32(obj + 0x04u));
    emit_bp(0x65, tmem | (lines << 10));
    flush_texture_state();
    std::uint32_t tlut = Memory::Read32(obj + 0x00u) & 0x00FFFFFFu;
    set_field(tlut, 10, 0, tmem);
    Memory::Write32(obj + 0x00u, tlut);
    g_gx.tlut_word[name] = tlut;
}

// GXInvalidateTexAll (SDK): BP 0x66 twice (even then odd TMEM banks), bracketed like GXLoadTlut.
void GX__InvalidateTexAll_80171110() {
    flush_texture_state();
    emit_bp(0x66, 0x001000);
    emit_bp(0x66, 0x001100);
    flush_texture_state();
}

void GX__SetTexRegionCallback_80171158(CpuContext* ctx) {
    const std::uint32_t previous = g_tex_region_cb;
    g_tex_region_cb = ctx->gpr[3];
    ctx->gpr[3] = previous;
}

void GX__SetTlutRegionCallback_8017116c(CpuContext* ctx) {
    const std::uint32_t previous = g_tlut_region_cb;
    g_tlut_region_cb = ctx->gpr[3];
    ctx->gpr[3] = previous;
}

// GXSetTexCoordScaleManually(coord, enable, ss, ts) - see su_reg_value for the bit-20 marker.
void GX__SetTexCoordScaleManually_80171180(uint32_t coord, uint32_t enable, uint32_t ss, uint32_t ts) {
    if (coord >= 8u) {
        return;
    }
    const bool on = gx_bool(enable);
    g_gx.tcs_man_enab = static_cast<std::uint8_t>((g_gx.tcs_man_enab & ~(1u << coord)) | (on ? 1u << coord : 0u));
    if (on) {
        set_field(g_gx.su_ts0[coord], 16, 0, (ss - 1u) & 0xFFFFu);
        set_field(g_gx.su_ts1[coord], 16, 0, (ts - 1u) & 0xFFFFu);
    }
    // The SDK writes nothing on disable (the next draw recomputes from the texture). The marker
    // has to be cleared explicitly here or the decoder would keep scaling by the old size.
    emit_su_ts0(coord);
    emit_su_ts1(coord);
}

// GXSetTexCoordBias(coord, s_enable, t_enable): bit 16; written only for manual coordinates.
void GX__SetTexCoordBias_801711fc(uint32_t coord, uint32_t s_enable, uint32_t t_enable) {
    if (coord >= 8u) {
        return;
    }
    set_field(g_gx.su_ts0[coord], 1, 16, gx_bool(s_enable) ? 1u : 0u);
    set_field(g_gx.su_ts1[coord], 1, 16, gx_bool(t_enable) ? 1u : 0u);
    if ((g_gx.tcs_man_enab & (1u << coord)) != 0) {
        emit_su_ts0(coord);
        emit_su_ts1(coord);
    }
}

// __GetImageTileCount(fmt, w, h, u32* row_tiles, u32* col_tiles, u32* cmp_tiles) (SDK GXTexture.c).
void GX__GetImageTileCount_80170738(uint32_t fmt, uint32_t w, uint32_t h, uint32_t prow, uint32_t pcol,
                                   uint32_t pcmp) {
    std::uint32_t sx = 0, sy = 0;
    tex_tile_shift(fmt, &sx, &sy);
    w &= 0xFFFFu;
    h &= 0xFFFFu;
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    if (prow != 0) Memory::Write32(prow, (w + (1u << sx) - 1u) >> sx);
    if (pcol != 0) Memory::Write32(pcol, (h + (1u << sy) - 1u) >> sy);
    if (pcmp != 0) Memory::Write32(pcmp, (fmt == 6u || fmt == 0x16u) ? 2u : 1u);
}

// =============================================================================================
// Indirect texturing (Aurora GXBump.cpp)
// =============================================================================================

// GXSetTevIndirect(tev_stage, ind_stage, fmt, bias_sel, mtx_sel, wrap_s, wrap_t, add_prev, utc_lod,
// alpha_sel) -> BP 0x10 + stage: ind stage 0-1, fmt 2-3, bias 4-6, alpha sel 7-8, mtx 9-12,
// wrap s 13-15, wrap t 16-18, utc lod 19, add prev 20. utc_lod and alpha_sel are passed on the
// caller's stack (8(r1), 12(r1)); the bindings read them.
void GX__SetTevIndirect_801717ac(uint32_t ts, uint32_t is, uint32_t fmt, uint32_t bias, uint32_t mtx, uint32_t ws,
                                 uint32_t wt, uint32_t add_prev, uint32_t utc_lod, uint32_t alpha_sel) {
    if (ts >= kTevStages) {
        return;
    }
    std::uint32_t reg = 0;
    set_field(reg, 2, 0, is);
    set_field(reg, 2, 2, fmt);
    set_field(reg, 3, 4, bias);
    set_field(reg, 2, 7, alpha_sel);
    set_field(reg, 4, 9, mtx);
    set_field(reg, 3, 13, ws);
    set_field(reg, 3, 16, wt);
    set_field(reg, 1, 19, gx_bool(utc_lod) ? 1u : 0u);
    set_field(reg, 1, 20, gx_bool(add_prev) ? 1u : 0u);
    emit_bp(0x10u + ts, reg);
}

// GXSetIndTexMtx(id, const f32 offset[2][3], s8 scale_exp): BP 0x06/0x07/0x08 + 3 * slot, one
// column per register (row 0 bits 0-10, row 1 bits 11-21, s1.10 fixed point), with the biased
// scale exponent (scale_exp + 17) split two bits per register into bits 22-23. The dynamic S/T
// ids select the same slot, exactly as the SDK does.
void GX__SetIndTexMtx_80171814(uint32_t id, uint32_t ma, uint32_t scale_exp) {
    std::uint32_t slot = 0;
    if (id >= 1u && id <= 3u) {
        slot = id - 1u;
    } else if (id >= 5u && id <= 7u) {
        slot = id - 5u;
    } else if (id >= 9u && id <= 11u) {
        slot = id - 9u;
    }
    float m[6]{};
    if (ma != 0) {
        read_be_floats(ma, m, 6);
    }
    auto fixed = [](float v) -> std::uint32_t {
        float s = 1024.0f * v;
        if (!(s > -2147483648.0f)) s = -2147483648.0f;
        if (s > 2147483520.0f) s = 2147483520.0f;
        return static_cast<std::uint32_t>(static_cast<std::int32_t>(s)) & 0x7FFu;
    };
    const std::int32_t adj = static_cast<std::int32_t>(static_cast<std::int8_t>(scale_exp & 0xFFu)) + 17;
    for (unsigned col = 0; col < 3u; ++col) {
        std::uint32_t reg = 0;
        set_field(reg, 11, 0, fixed(m[col]));
        set_field(reg, 11, 11, fixed(m[3u + col]));
        set_field(reg, 2, 22, static_cast<std::uint32_t>(adj >> (col * 2u)) & 3u);
        emit_bp(0x06u + slot * 3u + col, reg);
    }
}

// GXSetIndTexCoordScale(ind_stage, scale_s, scale_t): BP 0x25 (stages 0/1) / 0x26 (2/3), four
// bits each: stage s at 0-3, t at 4-7; the odd stage at 8-15.
void GX__SetIndTexCoordScale_80171968(uint32_t stage, uint32_t ss, uint32_t ts) {
    if (stage >= 4u) {
        return;
    }
    std::uint32_t& reg = g_gx.ind_scale[stage >> 1];
    const unsigned base = (stage & 1u) * 8u;
    set_field(reg, 4, base, ss);
    set_field(reg, 4, base + 4u, ts);
    emit_bp(0x25u + (stage >> 1), reg);
}

// GXSetIndTexOrder(ind_stage, coord, map): BP 0x27, six bits per stage (map 0-2, coord 3-5).
void GX__SetIndTexOrder_80171a6c(uint32_t stage, uint32_t coord, uint32_t map) {
    if (stage >= 4u) {
        return;
    }
    set_field(g_gx.iref, 3, stage * 6u, map);
    set_field(g_gx.iref, 3, stage * 6u + 3u, coord);
    emit_bp(0x27, g_gx.iref);
    g_gx.dirty |= kDirtySuTex | kDirtyBpMask;
}

void GX__SetNumIndStages_80171b38(uint32_t n) {
    set_field(g_gx.gen_mode, 3, 16, n);
    g_gx.dirty |= kDirtyBpMask | kDirtyGenMode;
}

void GX__SetTevDirect_80171b58(uint32_t s) {
    GX__SetTevIndirect_801717ac(s, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

// GXSetTevIndWarp(tev_stage, ind_stage, signed_offsets, replace_mode, mtx_sel).
void GX__SetTevIndWarp_80171ba0(uint32_t ts, uint32_t is, uint32_t signed_offsets, uint32_t replace_mode,
                                uint32_t mtx) {
    const std::uint32_t wrap = gx_bool(replace_mode) ? 6u : 0u;    // GX_ITW_0 : GX_ITW_OFF
    const std::uint32_t bias = gx_bool(signed_offsets) ? 7u : 0u;  // GX_ITB_STU : GX_ITB_NONE
    GX__SetTevIndirect_801717ac(ts, is, 0, bias, mtx, wrap, wrap, 0, 0, 0);
}

// =============================================================================================
// TEV (Aurora GXTev.cpp)
// BP 0xC0 + 2n colour env: d 0-3, c 4-7, b 8-11, a 12-15, bias 16-17, sub 18, clamp 19,
//                          scale 20-21, dest 22-23
// BP 0xC1 + 2n alpha env:  ras swap 0-1, tex swap 2-3, d 4-6, c 7-9, b 10-12, a 13-15, then as colour
// Compare ops (GX_TEV_COMP_*) keep op & 1 in bit 18, put (op >> 1) & 3 in the scale field and
// GX_TB_COMPARE (3) in the bias field.
// =============================================================================================

} // extern "C"

namespace {

void set_tev_env_op(std::uint32_t& reg, std::uint32_t op, std::uint32_t bias, std::uint32_t scale, bool clamp,
                    std::uint32_t out) {
    set_field(reg, 1, 18, op & 1u);
    if (op <= 1u) {
        set_field(reg, 2, 20, scale);
        set_field(reg, 2, 16, bias);
    } else {
        set_field(reg, 2, 20, (op >> 1) & 3u);
        set_field(reg, 2, 16, 3);
    }
    set_field(reg, 1, 19, clamp ? 1u : 0u);
    set_field(reg, 2, 22, out);
}

void tev_color_in(unsigned s, std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    std::uint32_t& reg = g_gx.tevc[s];
    set_field(reg, 4, 12, a);
    set_field(reg, 4, 8, b);
    set_field(reg, 4, 4, c);
    set_field(reg, 4, 0, d);
    emit_bp(0xC0u + s * 2u, reg);
}

void tev_alpha_in(unsigned s, std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    std::uint32_t& reg = g_gx.teva[s];
    set_field(reg, 3, 13, a);
    set_field(reg, 3, 10, b);
    set_field(reg, 3, 7, c);
    set_field(reg, 3, 4, d);
    emit_bp(0xC1u + s * 2u, reg);
}

void tev_color_op(unsigned s, std::uint32_t op, std::uint32_t bias, std::uint32_t scale, bool clamp,
                  std::uint32_t out) {
    set_tev_env_op(g_gx.tevc[s], op, bias, scale, clamp, out);
    emit_bp(0xC0u + s * 2u, g_gx.tevc[s]);
}

void tev_alpha_op(unsigned s, std::uint32_t op, std::uint32_t bias, std::uint32_t scale, bool clamp,
                  std::uint32_t out) {
    set_tev_env_op(g_gx.teva[s], op, bias, scale, clamp, out);
    emit_bp(0xC1u + s * 2u, g_gx.teva[s]);
}

// BP 0xE0 + 2n (R bits 0-10, A bits 12-22) and 0xE1 + 2n (B 0-10, G 12-22); bit 23 selects the
// konstant register bank. The SDK repeats the BG write for a hardware latch quirk; once suffices.
void tev_reg_write(std::uint32_t id, std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a,
                   bool konst) {
    std::uint32_t ra = 0;
    std::uint32_t bg = 0;
    set_field(ra, 11, 0, r);
    set_field(ra, 11, 12, a);
    set_field(bg, 11, 0, b);
    set_field(bg, 11, 12, g);
    if (konst) {
        ra |= 1u << 23;
        bg |= 1u << 23;
    }
    emit_bp(0xE0u + id * 2u, ra);
    emit_bp(0xE1u + id * 2u, bg);
}

} // namespace

extern "C" {

// GXSetTevOp(stage, mode): the five presets, built from the four fine-grained calls.
void GX__SetTevOp_80171c4c(uint32_t s, uint32_t m) {
    if (s >= kTevStages) {
        return;
    }
    const std::uint32_t carg = s == 0 ? 10u : 0u;  // GX_CC_RASC : GX_CC_CPREV
    const std::uint32_t aarg = s == 0 ? 5u : 0u;   // GX_CA_RASA : GX_CA_APREV
    constexpr std::uint32_t kCcZero = 15, kCcOne = 12, kCcTexc = 8, kCcTexa = 9;
    constexpr std::uint32_t kCaZero = 7, kCaTexa = 4;
    switch (m) {
    case 0:  // GX_MODULATE
        tev_color_in(s, kCcZero, kCcTexc, carg, kCcZero);
        tev_alpha_in(s, kCaZero, kCaTexa, aarg, kCaZero);
        break;
    case 1:  // GX_DECAL
        tev_color_in(s, carg, kCcTexc, kCcTexa, kCcZero);
        tev_alpha_in(s, kCaZero, kCaZero, kCaZero, aarg);
        break;
    case 2:  // GX_BLEND
        tev_color_in(s, carg, kCcOne, kCcTexc, kCcZero);
        tev_alpha_in(s, kCaZero, kCaTexa, aarg, kCaZero);
        break;
    case 3:  // GX_REPLACE
        tev_color_in(s, kCcZero, kCcZero, kCcZero, kCcTexc);
        tev_alpha_in(s, kCaZero, kCaZero, kCaZero, kCaTexa);
        break;
    case 4:  // GX_PASSCLR
        tev_color_in(s, kCcZero, kCcZero, kCcZero, carg);
        tev_alpha_in(s, kCaZero, kCaZero, kCaZero, aarg);
        break;
    default:
        break;
    }
    tev_color_op(s, 0, 0, 0, true, 0);
    tev_alpha_op(s, 0, 0, 0, true, 0);
}

void GX__SetTevColorIn_80171ce0(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if (s < kTevStages) {
        tev_color_in(s, a, b, c, d);
    }
}

void GX__SetTevAlphaIn_80171d20(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if (s < kTevStages) {
        tev_alpha_in(s, a, b, c, d);
    }
}

void GX__SetTevColorOp_80171d60(uint32_t s, uint32_t op, uint32_t bias, uint32_t scale, uint32_t clamp,
                                uint32_t out_reg) {
    if (s < kTevStages) {
        tev_color_op(s, op, bias, scale, gx_bool(clamp), out_reg);
    }
}

void GX__SetTevAlphaOp_80171db8(uint32_t s, uint32_t op, uint32_t bias, uint32_t scale, uint32_t clamp,
                                uint32_t out_reg) {
    if (s < kTevStages) {
        tev_alpha_op(s, op, bias, scale, gx_bool(clamp), out_reg);
    }
}

// GXColor is a struct passed by value: the register holds a pointer to the caller's copy.
void GX__SetTevColor_80171e10(uint32_t id, uint32_t color_ptr) {
    if (id >= 4u || color_ptr == 0) {
        return;
    }
    const std::uint32_t c = Memory::Read32(color_ptr);
    tev_reg_write(id, c >> 24, (c >> 16) & 0xFFu, (c >> 8) & 0xFFu, c & 0xFFu, false);
}

// GXColorS10: four s16 components, written as their low 11 bits without clamping.
void GX__SetTevColorS10_80171e70(uint32_t id, uint32_t color_ptr) {
    if (id >= 4u || color_ptr == 0) {
        return;
    }
    tev_reg_write(id, Memory::Read16(color_ptr) & 0x7FFu, Memory::Read16(color_ptr + 2u) & 0x7FFu,
                  Memory::Read16(color_ptr + 4u) & 0x7FFu, Memory::Read16(color_ptr + 6u) & 0x7FFu, false);
}

void GX__SetTevKColor_80171ed4(uint32_t id, uint32_t color_ptr) {
    if (id >= 4u || color_ptr == 0) {
        return;
    }
    const std::uint32_t c = Memory::Read32(color_ptr);
    tev_reg_write(id, c >> 24, (c >> 16) & 0xFFu, (c >> 8) & 0xFFu, c & 0xFFu, true);
}

// BP 0xF6 + stage / 2: kcsel 4-8 / kasel 9-13 for the even stage, 14-18 / 19-23 for the odd one.
void GX__SetTevKColorSel_80171f30(uint32_t stage, uint32_t sel) {
    if (stage >= kTevStages) {
        return;
    }
    std::uint32_t& reg = g_gx.ksel[stage >> 1];
    set_field(reg, 5, (stage & 1u) ? 14u : 4u, sel);
    emit_bp(0xF6u + (stage >> 1), reg);
}

void GX__SetTevKAlphaSel_80171f80(uint32_t stage, uint32_t sel) {
    if (stage >= kTevStages) {
        return;
    }
    std::uint32_t& reg = g_gx.ksel[stage >> 1];
    set_field(reg, 5, (stage & 1u) ? 19u : 9u, sel);
    emit_bp(0xF6u + (stage >> 1), reg);
}

// GXSetTevSwapMode: the swap-table selects live in the stage's alpha env register (bits 0-3).
void GX__SetTevSwapMode_80171fd0(uint32_t s, uint32_t ras_sel, uint32_t tex_sel) {
    if (s >= kTevStages) {
        return;
    }
    set_field(g_gx.teva[s], 2, 0, ras_sel);
    set_field(g_gx.teva[s], 2, 2, tex_sel);
    emit_bp(0xC1u + s * 2u, g_gx.teva[s]);
}

// GXSetTevSwapModeTable(table, r, g, b, a): ksel[2t] bits 0-1 r / 2-3 g, ksel[2t+1] b / a.
void GX__SetTevSwapModeTable_8017200c(uint32_t id, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (id >= 4u) {
        return;
    }
    std::uint32_t& rg = g_gx.ksel[id * 2u];
    std::uint32_t& ba = g_gx.ksel[id * 2u + 1u];
    set_field(rg, 2, 0, r);
    set_field(rg, 2, 2, g);
    emit_bp(0xF6u + id * 2u, rg);
    set_field(ba, 2, 0, b);
    set_field(ba, 2, 2, a);
    emit_bp(0xF7u + id * 2u, ba);
}

// BP 0xF3: ref0 0-7, ref1 8-15, comp0 16-18, comp1 19-21, op 22-23.
void GX__SetAlphaCompare_80172088(uint32_t c0, uint32_t r0, uint32_t op, uint32_t c1, uint32_t r1) {
    std::uint32_t reg = 0;
    set_field(reg, 8, 0, r0);
    set_field(reg, 8, 8, r1);
    set_field(reg, 3, 16, c0);
    set_field(reg, 3, 19, c1);
    set_field(reg, 2, 22, op);
    emit_bp(0xF3, reg);
}

// GXSetZTexture(op, fmt, bias): BP 0xF4 = bias, BP 0xF5 = z format 0-1 (Z8 0, Z16 1, Z24X8 2),
// op 2-3.
void GX__SetZTexture_801720c0(uint32_t op, uint32_t fmt, uint32_t bias) {
    std::uint32_t zfmt = 2;
    if (fmt == 0x11u) {
        zfmt = 0;
    } else if (fmt == 0x13u) {
        zfmt = 1;
    }
    emit_bp(0xF4, bias & 0x00FFFFFFu);
    emit_bp(0xF5, ((op & 3u) << 2) | zfmt);
}

// GXSetTevOrder(stage, coord, map, color) -> BP 0x28 + stage / 2 (RAS1_TREF). Even stage: map
// 0-2, coord 3-5, enable 6, colour 7-9; the odd stage the same at +12. GX_COLOR_NULL (and any
// other out-of-table channel) is hardware 7 (zero).
void GX__SetTevOrder_8017214c(uint32_t s, uint32_t coord, uint32_t map, uint32_t color) {
    if (s >= kTevStages) {
        return;
    }
    static constexpr std::uint8_t kC2R[9] = {0, 1, 0, 1, 0, 1, 7, 5, 6};
    std::uint32_t tmap = map & ~kTexMapDisable;
    if (tmap >= 8u) {
        tmap = 0;
    }
    const std::uint32_t tcoord = coord < 8u ? coord : 0u;
    const std::uint32_t chan = color < 9u ? kC2R[color] : 7u;
    const std::uint32_t enable = (map != kTexMapNull && (map & kTexMapDisable) == 0) ? 1u : 0u;
    std::uint32_t& reg = g_gx.tref[s >> 1];
    const unsigned base = (s & 1u) * 12u;
    set_field(reg, 3, base + 0u, tmap);
    set_field(reg, 3, base + 3u, tcoord);
    set_field(reg, 1, base + 6u, enable);
    set_field(reg, 3, base + 7u, chan);
    emit_bp(0x28u + (s >> 1), reg);
    g_gx.dirty |= kDirtySuTex;
}

void GX__SetNumTevStages_801722a8(uint32_t n) {
    set_field(g_gx.gen_mode, 4, 10, (n & 0xFFu) - 1u);
    g_gx.dirty |= kDirtyGenMode;
}

// =============================================================================================
// Fog (Aurora GXPixel.cpp GXSetFog)
// BP 0xEE A (mantissa 0-10, exponent 11-18, sign 19), 0xEF B magnitude, 0xF0 B shift,
// 0xF1 C (as A) + projection 20 + fsel 21-23, 0xF2 colour (B 0-7, G 8-15, R 16-23).
// =============================================================================================

} // extern "C"

namespace {

void fog_write(std::uint32_t type, float start_z, float end_z, float near_z, float far_z, std::uint32_t rgba) {
    const bool ortho = (type & 8u) != 0;
    float a = 0.0f;
    float c = 0.0f;
    std::uint32_t b_m = 0;
    std::uint32_t b_s = 0;
    if (!ortho) {
        float fa = 0.0f;
        float fb = 0.5f;  // the degenerate (far == near) case normalises to b_m = 0.5, b_s = 1
        float fc = 0.0f;
        if (far_z != near_z && end_z != start_z) {
            fa = (far_z * near_z) / ((far_z - near_z) * (end_z - start_z));
            fb = far_z / (far_z - near_z);
            fc = start_z / (end_z - start_z);
        }
        float b_mant = fb;
        int b_expn = 0;
        while (b_mant > 1.0f) {
            b_mant *= 0.5f;
            ++b_expn;
        }
        while (b_mant > 0.0f && b_mant < 0.5f) {
            b_mant *= 2.0f;
            --b_expn;
        }
        a = std::ldexp(fa, -(b_expn + 1));
        b_m = static_cast<std::uint32_t>(8.388638e6f * b_mant);
        b_s = static_cast<std::uint32_t>(b_expn + 1) & 0x1Fu;
        c = fc;
    } else if (far_z != near_z && end_z != start_z) {
        const float inv = 1.0f / (end_z - start_z);
        a = inv * (far_z - near_z);
        c = inv * (start_z - near_z);
    }
    const std::uint32_t a_hex = f32_to_bits(a);
    const std::uint32_t c_hex = f32_to_bits(c);
    std::uint32_t fog0 = 0;
    set_field(fog0, 11, 0, (a_hex >> 12) & 0x7FFu);
    set_field(fog0, 8, 11, (a_hex >> 23) & 0xFFu);
    set_field(fog0, 1, 19, a_hex >> 31);
    std::uint32_t fog3 = 0;
    set_field(fog3, 11, 0, (c_hex >> 12) & 0x7FFu);
    set_field(fog3, 8, 11, (c_hex >> 23) & 0xFFu);
    set_field(fog3, 1, 19, c_hex >> 31);
    set_field(fog3, 1, 20, ortho ? 1u : 0u);
    set_field(fog3, 3, 21, type & 7u);
    const std::uint32_t fogc = ((rgba >> 24) << 16) | (((rgba >> 16) & 0xFFu) << 8) | ((rgba >> 8) & 0xFFu);
    emit_bp(0xEE, fog0);
    emit_bp(0xEF, b_m & 0x00FFFFFFu);
    emit_bp(0xF0, b_s);
    emit_bp(0xF1, fog3);
    emit_bp(0xF2, fogc);
}

} // namespace

extern "C" {

// GXSetFog(type, startz, endz, nearz, farz, GXColor): r3 type, f1..f4 the planes, r4 a pointer to
// the caller's copy of the by-value GXColor.
void GX__SetFog_801722cc(uint32_t type, float start_z, float end_z, float near_z, float far_z, uint32_t color_ptr) {
    fog_write(type, start_z, end_z, near_z, far_z, color_ptr != 0 ? Memory::Read32(color_ptr) : 0u);
}

// GXInitFogAdjTable(GXFogAdjTable* table, u16 width, const f32 projmtx[4][4]) (SDK GXPixel.c):
// ten u16 range-adjustment factors (x256, 12 bits), read by the translated GXSetFogRangeAdj.
void GX__InitFogAdjTable_801724f8(uint32_t table, uint32_t width, uint32_t projmtx) {
    if (table == 0 || projmtx == 0) {
        return;
    }
    float m[16]{};
    read_be_floats(projmtx, m, 16);
    float near_z = 0.0f;
    float side_x = 0.0f;
    if (m[15] == 0.0f) {  // perspective
        near_z = m[11] / (m[10] - 1.0f);
        side_x = near_z / m[0];
    } else {
        side_x = 1.0f / m[0];
        near_z = 1.73205f * side_x;
    }
    const std::uint32_t w = width & 0xFFFFu;
    const float iw = 2.0f / static_cast<float>(w == 0 ? 1u : w);
    for (std::uint32_t i = 0; i < 10u; ++i) {
        float xi = static_cast<float>((i + 1u) << 5);
        xi *= iw;
        xi *= side_x;
        const float range = std::sqrt(1.0f + (xi * xi) / (near_z * near_z));
        const float scaled = range * 256.0f;
        const std::uint32_t v = (scaled >= 0.0f && scaled < 4294967040.0f) ? static_cast<std::uint32_t>(scaled) : 0u;
        Memory::Write16(table + i * 2u, static_cast<std::uint16_t>(v & 0xFFFu));
    }
}

// =============================================================================================
// Pixel engine (Aurora GXPixel.cpp)
// BP 0x40 zmode: enable 0, func 1-3, update 4.
// BP 0x41 cmode0: blend 0, logic 1, dither 2, colour update 3, alpha update 4, dst factor 5-7,
//                 src factor 8-10, subtract 11, logic op 12-15.
// BP 0x42 cmode1: constant alpha 0-7, enable 8, YUV select 9-10.
// BP 0x43 peCtrl: pixel format 0-2, z format 3-5, z before texture 6.
// =============================================================================================

void GX__SetBlendMode_8017277c(uint32_t t, uint32_t s, uint32_t d, uint32_t op) {
    set_field(g_gx.cmode0, 1, 0, (t == 1u || t == 3u) ? 1u : 0u);  // GX_BM_BLEND, GX_BM_SUBTRACT
    set_field(g_gx.cmode0, 1, 11, t == 3u ? 1u : 0u);
    set_field(g_gx.cmode0, 1, 1, t == 2u ? 1u : 0u);                 // GX_BM_LOGIC
    set_field(g_gx.cmode0, 4, 12, op);
    set_field(g_gx.cmode0, 3, 8, s);
    set_field(g_gx.cmode0, 3, 5, d);
    emit_bp(0x41, g_gx.cmode0);
}

void GX__SetColorUpdate_801727cc(uint32_t en) {
    set_field(g_gx.cmode0, 1, 3, gx_bool(en) ? 1u : 0u);
    emit_bp(0x41, g_gx.cmode0);
}

void GX__SetAlphaUpdate_801727f8(uint32_t en) {
    set_field(g_gx.cmode0, 1, 4, gx_bool(en) ? 1u : 0u);
    emit_bp(0x41, g_gx.cmode0);
}

void GX__SetZMode_80172824(uint32_t ce, uint32_t fn, uint32_t ue) {
    set_field(g_gx.zmode, 1, 0, gx_bool(ce) ? 1u : 0u);
    set_field(g_gx.zmode, 3, 1, fn);
    set_field(g_gx.zmode, 1, 4, gx_bool(ue) ? 1u : 0u);
    emit_bp(0x40, g_gx.zmode);
}

void GX__SetZCompLoc_80172858(uint32_t before_tex) {
    set_field(g_gx.pe_ctrl, 1, 6, gx_bool(before_tex) ? 1u : 0u);
    emit_bp(0x43, g_gx.pe_ctrl);
}

// GXSetPixelFmt(pix_fmt, z_fmt): peCtrl is written only when it changes; RGB565_Z16 also turns
// on genMode bit 9 (multisampling).
void GX__SetPixelFmt_80172888(uint32_t pix_fmt, uint32_t z_fmt) {
    static constexpr std::uint8_t kP2F[8] = {0, 1, 2, 3, 4, 4, 4, 5};
    const std::uint32_t hw = kP2F[pix_fmt & 7u];
    const std::uint32_t old = g_gx.pe_ctrl;
    set_field(g_gx.pe_ctrl, 3, 0, hw);
    set_field(g_gx.pe_ctrl, 3, 3, z_fmt);
    if (old != g_gx.pe_ctrl) {
        emit_bp(0x43, g_gx.pe_ctrl);
        set_field(g_gx.gen_mode, 1, 9, pix_fmt == 2u ? 1u : 0u);
        g_gx.dirty |= kDirtyGenMode;
    }
    if (hw == 4u) {
        set_field(g_gx.cmode1, 2, 9, (pix_fmt - 4u) & 3u);
        emit_bp(0x42, g_gx.cmode1);
    }
}

void GX__SetDither_80172930(uint32_t en) {
    set_field(g_gx.cmode0, 1, 2, gx_bool(en) ? 1u : 0u);
    emit_bp(0x41, g_gx.cmode0);
}

void GX__SetDstAlpha_8017295c(uint32_t en, uint32_t alpha) {
    set_field(g_gx.cmode1, 8, 0, alpha);
    set_field(g_gx.cmode1, 1, 8, gx_bool(en) ? 1u : 0u);
    emit_bp(0x42, g_gx.cmode1);
}

// GXSetFieldMask(odd_mask, even_mask): BP 0x44 even 0, odd 1.
void GX__SetFieldMask_8017298c(uint32_t odd_mask, uint32_t even_mask) {
    emit_bp(0x44, (gx_bool(even_mask) ? 1u : 0u) | (gx_bool(odd_mask) ? 2u : 0u));
}

void GX__DrawSphere_80172a30(uint32_t, uint32_t) {}

// =============================================================================================
// Display lists (Aurora GXDispList.cpp)
//
// GXBeginDisplayList flushes pending state into the live stream, then moves the live FIFO (and
// the GXData shadow) aside so the list records from offset 0 into the whole buffer. Everything
// the game emits until GXEndDisplayList - natives and raw WGPipe writes alike - is list content.
// GXEndDisplayList pads to 32 bytes, copies the list out and puts the live FIFO back; with
// dlSaveContext (GXData+0x5F9) the shadow is restored too, so state set inside the list does not
// leak, otherwise the shadow keeps it (as on hardware). No frame is presented while recording.
// =============================================================================================

void GX__BeginDisplayList_80172e00(uint32_t list, uint32_t size) {
    flush_dirty();
    mkwv::gx::Fifo* f = fifo();
    g_dl.base = list;
    g_dl.size = size;
    g_dl.active = f != nullptr && list != 0 && size != 0;
    if (g_dl.active) {
        g_dl_saved_fifo = *f;
        g_dl_saved_gx = g_gx;
        mkwv::gx::fifo_reset_buffer_only(*f);
    }
    if (list != 0) {
        Memory::Write32(kDlFifoAddr + 0x00u, list);
        Memory::Write32(kDlFifoAddr + 0x04u, size >= 4u ? list + size - 4u : list);
        Memory::Write32(kDlFifoAddr + 0x08u, size);
        Memory::Write32(kDlFifoAddr + 0x14u, list);
        Memory::Write32(kDlFifoAddr + 0x18u, list);
        Memory::Write32(kDlFifoAddr + 0x1Cu, 0);
        Memory::Write8(kDlFifoAddr + 0x20u, 0);
    }
    const std::uint32_t gd = Memory::Read32(kGXDataPtrAddr);
    if (gd != 0) {
        Memory::Write8(gd + kGXDataInDispList, 1);
    }
}

std::uint32_t GX__EndDisplayList_80172eb4() {
    const std::uint32_t gd = Memory::Read32(kGXDataPtrAddr);
    std::uint32_t result = 0;
    if (g_dl.active) {
        flush_dirty();
        if (mkwv::gx::Fifo* f = raw_fifo()) {
            const std::uint32_t used = f->size();
            const std::uint32_t padded = (used + 31u) & ~31u;
            if (!f->overflow() && padded <= g_dl.size && g_dl.base != 0) {
                static const std::uint8_t kZeros[32] = {};
                copy_guest_bytes(g_dl.base, f->data(), used);
                copy_guest_bytes(g_dl.base + used, kZeros, padded - used);
                result = padded;
            }
            const bool save_context = gd != 0 && Memory::Read8(gd + kGXDataDlSaveContext) != 0;
            if (save_context) {
                *f = g_dl_saved_fifo;
                g_gx = g_dl_saved_gx;
            } else {
                mkwv::gx::fifo_reset_buffer_only(*f);
                f->write_bytes(g_dl_saved_fifo.data(), g_dl_saved_fifo.size());
            }
        }
        Memory::Write32(kDlFifoAddr + 0x1Cu, result);
        Memory::Write32(kDlFifoAddr + 0x18u, g_dl.base + result);
    }
    if (gd != 0) {
        Memory::Write8(gd + kGXDataInDispList, 0);
    }
    g_dl = DisplayListState{};
    return result;
}

// GXEndDisplayList returns the list size in r3.
void GX__EndDisplayListCpu_80172eb4(CpuContext* ctx) {
    ctx->gpr[3] = GX__EndDisplayList_80172eb4();
}

void GX__CallDisplayList_80172f64(uint32_t list, uint32_t nbytes) {
    mkwv::runtime::hle_fifo_early_handoff();
    flush_dirty();
    if (list == 0 || nbytes == 0 || nbytes > 0x100000u) {
        return;
    }
    if (mkwv::gx::Fifo* f = fifo()) {
        f->call_dl(list, nbytes);  // the decoder resolves and walks the list itself
    }
}

// =============================================================================================
// Transform unit (Aurora GXTransform.cpp)
// =============================================================================================

// XF 0x1020..0x1025 projection parameters, 0x1026 projection type.
void GX__SetProjection_8017301c(uint32_t ma, uint32_t pt) {
    float m[16]{};
    read_be_floats(ma, m, 16);
    const bool ortho = pt == 1u;  // GX_ORTHOGRAPHIC
    float raw[6]{};
    raw[0] = m[0];
    raw[1] = ortho ? m[3] : m[2];
    raw[2] = m[5];
    raw[3] = ortho ? m[7] : m[6];
    raw[4] = m[10];
    raw[5] = m[11];
    g_gx.proj_vec[0] = ortho ? 1.0f : 0.0f;
    for (unsigned i = 0; i < 6u; ++i) {
        g_gx.proj_vec[1u + i] = raw[i];
    }
    if (mkwv::gx::Fifo* f = fifo()) {
        const std::uint32_t type = ortho ? 1u : 0u;
        f->load_xf_f32(0x1020, raw, 6);
        f->load_xf(0x1026, &type, 1);
    }
}

void GX__SetProjectionv_80173080(uint32_t pa) {
    if (pa == 0) {
        return;
    }
    float v[7];
    for (unsigned i = 0; i < 7u; ++i) {
        v[i] = read_guest_f32(pa + i * 4u);
    }
    std::memcpy(g_gx.proj_vec, v, sizeof(g_gx.proj_vec));
    if (mkwv::gx::Fifo* f = fifo()) {
        // v[0] is the projection type as a float: 0.0 perspective, anything else orthographic.
        const std::uint32_t type = v[0] != 0.0f ? 1u : 0u;
        f->load_xf_f32(0x1020, v + 1, 6);
        f->load_xf(0x1026, &type, 1);
    }
}

void GX__GetProjectionv_801730cc(uint32_t pa) {
    if (pa == 0 || !Memory::Contains(pa, 28)) {
        return;
    }
    for (unsigned i = 0; i < 7u; ++i) {
        write_guest_f32(pa + i * 4u, g_gx.proj_vec[i]);
    }
}

void GX__LoadPosMtxImm_8017310c(uint32_t ma, uint32_t id) {
    float mtx[12]{};
    read_be_floats(ma, mtx, 12);
    emit_xf_f32(id * 4u, mtx, 12);
}

// GXLoadPosMtxIndx(u16 index, id): CP command 0x20 - index 16-31, 12 words (count-1 = 11) at 12-15,
// XF address id * 4 at 0-11. The decoder fetches the matrix through CP array 0xAC (GX_POS_MTX_ARRAY).
void GX__LoadPosMtxIndx_8017315c(uint32_t index, uint32_t id) {
    if (mkwv::gx::Fifo* f = fifo()) {
        f->write_u8(0x20);
        f->write_u32(((index & 0xFFFFu) << 16) | (11u << 12) | ((id * 4u) & 0xFFFu));
    }
}

// GX passes a 3x4 source matrix; XF normal matrices hold the upper-left 3x3.
void GX__LoadNrmMtxImm_80173188(uint32_t ma, uint32_t id) {
    float source[12]{};
    float normal[9]{};
    read_be_floats(ma, source, 12);
    for (unsigned row = 0; row < 3u; ++row) {
        for (unsigned col = 0; col < 3u; ++col) {
            normal[row * 3u + col] = source[row * 4u + col];
        }
    }
    emit_xf_f32(0x400u + id * 3u, normal, 9);
}

// GXLoadNrmMtxIndx3x3(u16 index, id): CP command 0x28, 9 words (count-1 = 8), XF 0x400 + id * 3.
void GX__LoadNrmMtxIndx3x3_801731e0(uint32_t index, uint32_t id) {
    if (mkwv::gx::Fifo* f = fifo()) {
        f->write_u8(0x28);
        f->write_u32(((index & 0xFFFFu) << 16) | (8u << 12) | ((0x400u + id * 3u) & 0xFFFu));
    }
}

// GXSetCurrentMtx: MATINDEX_A bits 0-5 only - the texture matrix ids in the rest of the register
// must survive (zeroing them sent every texgen through PNMTX0).
void GX__SetCurrentMtx_80173214(uint32_t id) {
    set_field(g_gx.mat_idx_a, 6, 0, id);
    emit_matrix_index(false);
}

// GXLoadTexMtxImm(mtx, id, type): GX_TEXMTXn at XF id * 4, GX_PTTEXMTXn (id >= 64) at
// 0x500 + (id - 64) * 4; GX_MTX3x4 (0) is 12 floats, GX_MTX2x4 8.
void GX__LoadTexMtxImm_80173234(uint32_t ma, uint32_t id, uint32_t t) {
    const unsigned count = t == 0u ? 12u : 8u;
    float mtx[12]{};
    read_be_floats(ma, mtx, count);
    const std::uint32_t addr = id >= 64u ? 0x500u + (id - 64u) * 4u : id * 4u;
    emit_xf_f32(addr, mtx, count);
}

// Viewport and scissor go through the decoder's Aurora escapes (it depends on them), with the
// values shadowed for the getters.
} // extern "C"

namespace {

void set_viewport(float l, float t, float w, float h, float nz, float fz) {
    g_gx.viewport[0] = l;
    g_gx.viewport[1] = t;
    g_gx.viewport[2] = w;
    g_gx.viewport[3] = h;
    g_gx.viewport[4] = nz;
    g_gx.viewport[5] = fz;
    if (mkwv::gx::Fifo* f = fifo()) {
        f->set_viewport(l, t, w, h, nz, fz);
    }
}

} // namespace

extern "C" {

// GXSetViewportJitter(l, t, w, h, n, f, field): field 0 (the even field) shifts up half a line.
void GX__SetViewportJitter_80173378(float l, float t, float w, float h, float nz, float fz, uint32_t field) {
    if (field == 0) {
        t -= 0.5f;
    }
    set_viewport(l, t, w, h, nz, fz);
}

void GX__SetViewport_801733b4(float l, float t, float w, float h, float nz, float fz) {
    set_viewport(l, t, w, h, nz, fz);
}

void GX__GetViewportv_801733e0(uint32_t oa) {
    if (oa == 0) {
        return;
    }
    for (unsigned i = 0; i < 6u; ++i) {
        write_guest_f32(oa + i * 4u, g_gx.viewport[i]);
    }
}

void GX__SetZScaleOffset_80173400(float, float) {
    // The decoder works in clip space and the GXM viewport establishes the depth range; nothing
    // consumes a z scale/offset yet.
}

void GX__SetScissor_80173430(uint32_t l, uint32_t t, uint32_t w, uint32_t h) {
    g_gx.scissor[0] = l;
    g_gx.scissor[1] = t;
    g_gx.scissor[2] = w;
    g_gx.scissor[3] = h;
    if (mkwv::gx::Fifo* f = fifo()) {
        f->set_scissor(static_cast<std::int32_t>(l), static_cast<std::int32_t>(t), static_cast<std::int32_t>(w),
                       static_cast<std::int32_t>(h));
    }
}

void GX__GetScissor_80173498(uint32_t pl, uint32_t pt, uint32_t pw, uint32_t ph) {
    if (pl != 0) Memory::Write32(pl, g_gx.scissor[0]);
    if (pt != 0) Memory::Write32(pt, g_gx.scissor[1]);
    if (pw != 0) Memory::Write32(pw, g_gx.scissor[2]);
    if (ph != 0) Memory::Write32(ph, g_gx.scissor[3]);
}

// GXSetScissorBoxOffset(x, y): BP 0x59, ((x + 342) >> 1) at 0-9 and ((y + 342) >> 1) at 10-19.
void GX__SetScissorBoxOffset_801734e0(int32_t xo, int32_t yo) {
    g_gx.scissor_off[0] = xo;
    g_gx.scissor_off[1] = yo;
    const std::uint32_t x = (static_cast<std::uint32_t>(xo + 342) >> 1) & 0x3FFu;
    const std::uint32_t y = (static_cast<std::uint32_t>(yo + 342) >> 1) & 0x3FFu;
    emit_bp(0x59, x | (y << 10));
}

// GXSetClipMode(mode): XF 0x1005 (GX_CLIP_ENABLE = 0).
void GX__SetClipMode_8017351c(uint32_t mode) {
    emit_xf(0x1005, mode);
}

std::int32_t NAND__Check_8019ead0(uint32_t num_bytes, uint32_t num_inodes, uint32_t answer_ptr) {
    (void)num_bytes;
    (void)num_inodes;
    // Report "enough space, nothing wrong". Falling through left r3 holding the caller's leftover,
    // which the save-data path reads as an error code.
    if (answer_ptr != 0) {
        Memory::Write32(answer_ptr, 0);
    }
    return 0;
}

} // extern "C"

// =============================================================================================
// GXInit's default hardware state (Aurora GXManage.cpp GXInit, RVL GXInit), emitted through the
// natives above so it is exactly what those calls would produce. Uses the PAL 528 render mode
// the VI HLE reports (640 x 528 EFB and XFB, frame rendering).
// =============================================================================================

namespace {

void run_gx_init_sequence() {
    mkwv::gx::Fifo* f = raw_fifo();
    if (f == nullptr) {
        g_init_pending = true;
        return;
    }
    g_in_init_sequence = true;

    for (unsigned i = 0; i < 8u; ++i) {
        f->load_cp(static_cast<std::uint8_t>(0x80u + i), g_gx.vat_b[i]);
    }
    emit_xf(0x1000, 0x3F);  // XF error/mode control
    emit_xf(0x1012, 1);     // dual texture transform enable
    emit_bp(0x58, 0x0F);

    for (unsigned i = 0; i < 8u; ++i) {
        GX__SetTexCoordGen2_8016e37c(i, 1u /*GX_TG_MTX2x4*/, 4u + i /*GX_TG_TEXn*/, kTexMtxIdentity, 0,
                                     kPtTexMtxIdentity);
    }
    GX__SetNumTexGens_8016e5a4(1);
    GX__ClearVtxDesc_8016dc34();
    GX__InvalidateVtxCache_8016e36c();
    GX__SetLineWidth_8016f314(6, 0);
    GX__SetPointSize_8016f348(6, 0);
    for (unsigned i = 0; i < 8u; ++i) {
        GX__EnableTexOffsets_8016f37c(i, 0, 0);
    }

    static const float kIdentity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    static const float kIdentity3x3[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    emit_xf_f32(0x000, kIdentity, 12);                                // GXLoadPosMtxImm(PNMTX0)
    emit_xf_f32(0x400, kIdentity3x3, 9);                              // GXLoadNrmMtxImm(PNMTX0)
    GX__SetCurrentMtx_80173214(0);
    emit_xf_f32(kTexMtxIdentity * 4u, kIdentity, 12);                 // GXLoadTexMtxImm(GX_IDENTITY)
    emit_xf_f32(0x500u + (kPtTexMtxIdentity - 64u) * 4u, kIdentity, 12);  // (GX_PTIDENTITY)

    GX__SetViewport_801733b4(0.0f, 0.0f, 640.0f, 528.0f, 0.0f, 1.0f);
    GX__SetCoPlanar_8016f3e0(0);
    GX__SetCullMode_8016f3b8(2);  // GX_CULL_BACK
    GX__SetClipMode_8017351c(0);  // GX_CLIP_ENABLE
    GX__SetScissor_80173430(0, 0, 640, 528);
    GX__SetScissorBoxOffset_801734e0(0, 0);

    GX__SetNumChans_8017054c(0);
    for (std::uint32_t chan = 4; chan <= 5u; ++chan) {  // GX_COLOR0A0, GX_COLOR1A1
        GX__SetChanCtrl_80170570(chan, 0, 0 /*GX_SRC_REG*/, 1 /*GX_SRC_VTX*/, 0, 0 /*GX_DF_NONE*/, kAfNone);
        set_chan_color(g_gx.amb_color, 0x100A, chan, 0x00000000u);  // black, alpha 0
        set_chan_color(g_gx.mat_color, 0x100C, chan, 0xFFFFFFFFu);  // white
    }

    GX__InvalidateTexAll_80171110();

    for (std::uint32_t s = 0; s < 8u; ++s) {
        GX__SetTevOrder_8017214c(s, s, s, 4 /*GX_COLOR0A0*/);
    }
    for (std::uint32_t s = 8; s < kTevStages; ++s) {
        GX__SetTevOrder_8017214c(s, 0xFF, kTexMapNull, kColorNull);
    }
    GX__SetNumTevStages_801722a8(1);
    GX__SetTevOp_80171c4c(0, 3 /*GX_REPLACE*/);
    GX__SetAlphaCompare_80172088(7 /*GX_ALWAYS*/, 0, 0 /*GX_AOP_AND*/, 7, 0);
    GX__SetZTexture_801720c0(0 /*GX_ZT_DISABLE*/, 0x11 /*GX_TF_Z8*/, 0);
    for (std::uint32_t s = 0; s < kTevStages; ++s) {
        GX__SetTevKColorSel_80171f30(s, 6 /*GX_TEV_KCSEL_1_4*/);
        GX__SetTevKAlphaSel_80171f80(s, 0 /*GX_TEV_KASEL_1*/);
        GX__SetTevSwapMode_80171fd0(s, 0, 0);
    }
    GX__SetTevSwapModeTable_8017200c(0, 0, 1, 2, 3);  // RGBA
    GX__SetTevSwapModeTable_8017200c(1, 0, 0, 0, 3);  // RRRA
    GX__SetTevSwapModeTable_8017200c(2, 1, 1, 1, 3);  // GGGA
    GX__SetTevSwapModeTable_8017200c(3, 2, 2, 2, 3);  // BBBA
    for (std::uint32_t s = 0; s < kTevStages; ++s) {
        GX__SetTevDirect_80171b58(s);
    }
    GX__SetNumIndStages_80171b38(0);
    for (std::uint32_t s = 0; s < 4u; ++s) {
        GX__SetIndTexCoordScale_80171968(s, 0 /*GX_ITS_1*/, 0);
    }

    fog_write(0 /*GX_FOG_NONE*/, 0.0f, 1.0f, 0.1f, 1.0f, 0x00000000u);
    emit_bp(0xE8, 342);  // GXSetFogRangeAdj(GX_DISABLE, 0, NULL): centre + 342, disabled
    GX__SetBlendMode_8017277c(0 /*GX_BM_NONE*/, 4 /*GX_BL_SRCALPHA*/, 5 /*GX_BL_INVSRCALPHA*/, 0 /*GX_LO_CLEAR*/);
    GX__SetColorUpdate_801727cc(1);
    GX__SetAlphaUpdate_801727f8(1);
    GX__SetZMode_80172824(1, 3 /*GX_LEQUAL*/, 1);
    GX__SetZCompLoc_80172858(1);
    GX__SetDither_80172930(1);
    GX__SetDstAlpha_8017295c(0, 0);
    GX__SetPixelFmt_80172888(0 /*GX_PF_RGB8_Z24*/, 0 /*GX_ZC_LINEAR*/);
    GX__SetFieldMask_8017298c(1, 1);
    // GXSetFieldMode(field_rendering = 0, half_aspect = 0) - translated, but its register effect
    // belongs to the initial state: line half-aspect off, BP 0x68 field mode off.
    set_field(g_gx.lp_size, 1, 22, 0);
    emit_bp(0x22, g_gx.lp_size);
    emit_bp(0x68, 0);

    flush_dirty();
    g_in_init_sequence = false;
}

} // namespace

namespace mkwv::runtime {

// Called by the GXInit native (hle_os.cpp) after it has set up GXData and the FIFO object.
void hle_gx_init() {
    reset_shadows();
    g_dl = DisplayListState{};
    g_draw_done_cb = 0;
    g_tex_region_cb = 0;
    g_tlut_region_cb = 0;
    g_init_pending = false;
    run_gx_init_sequence();  // defers itself until the FIFO is bound
}

// GXFlush.
void hle_gx_flush() {
    flush_dirty();
}

// True between GXBeginDisplayList and GXEndDisplayList: frame presentation must be skipped.
bool hle_gx_in_display_list() {
    return g_dl.active;
}

} // namespace mkwv::runtime

// ---------------------------------------------------------------------------
// nw4r::lyt::detail::DrawQuad. Every 2D element in the game - the strap screen,
// menus, HUD, text - reaches GX through this one function, so leaving it
// unimplemented means the entire UI silently draws nothing.
// ---------------------------------------------------------------------------

namespace {

std::uint8_t scale_lyt_alpha(std::uint8_t alpha, std::uint32_t modulate) {
    // Reproduces the guest's reciprocal-multiply divide by 255 exactly; a plain
    // (alpha * modulate) / 255 differs by one on some inputs and shows up as banding.
    const std::int32_t product = static_cast<std::int32_t>(alpha) * static_cast<std::int32_t>(modulate & 0xFFu);
    const std::int32_t magic = static_cast<std::int32_t>(0x80808081u);
    std::int32_t quotient = static_cast<std::int32_t>((static_cast<std::int64_t>(magic) * product) >> 32);
    quotient = static_cast<std::int32_t>(quotient + product) >> 7;
    quotient += static_cast<std::int32_t>(static_cast<std::uint32_t>(quotient) >> 31);
    return static_cast<std::uint8_t>(quotient);
}

std::uint32_t read_lyt_color(std::uint32_t color_addr, unsigned index, bool modulate, std::uint32_t alpha) {
    const std::uint32_t addr = color_addr + index * 4u;
    if (!modulate || (alpha & 0xFFu) == 0xFFu) {
        return Memory::Read32(addr);
    }
    const std::uint8_t r = Memory::Read8(addr);
    const std::uint8_t g = Memory::Read8(addr + 1u);
    const std::uint8_t b = Memory::Read8(addr + 2u);
    const std::uint8_t a = scale_lyt_alpha(Memory::Read8(addr + 3u), alpha);
    return (static_cast<std::uint32_t>(r) << 24) | (static_cast<std::uint32_t>(g) << 16) |
           (static_cast<std::uint32_t>(b) << 8) | a;
}

void append_lyt_vertex(std::uint8_t* packet, std::uint32_t& pos, float x, float y, std::uint32_t tex_coord_addr,
                       unsigned tex_coord_count, std::uint32_t corner_offset, const std::uint32_t* colors,
                       unsigned color_index) {
    BigEndian::WriteFloat32(packet + pos, x);
    pos += 4u;
    BigEndian::WriteFloat32(packet + pos, y);
    pos += 4u;
    if (colors != nullptr) {
        BigEndian::Write32(packet + pos, colors[color_index]);
        pos += 4u;
    }
    for (unsigned i = 0; i < tex_coord_count; ++i) {
        const std::uint32_t coord = tex_coord_addr + i * 32u + corner_offset;
        BigEndian::WriteFloat32(packet + pos, read_guest_f32(coord));
        pos += 4u;
        BigEndian::WriteFloat32(packet + pos, read_guest_f32(coord + 4u));
        pos += 4u;
    }
}

// The packet below is pos(2xf32) + optional RGBA8 + n_tex x (2xf32). nw4r::lyt sets exactly that
// descriptor before calling DrawQuad, but if it ever does not, the decoder computes a different
// stride and consumes the wrong number of bytes - silently turning the rest of the frame's FIFO
// into garbage. Cheap insurance against a corrupt frame that would be very hard to trace back.
std::uint32_t g_lyt_emitted = 0;
std::uint32_t g_lyt_skipped = 0;

// One sample of what DrawQuad was actually handed, so a frame full of zero UVs can be traced to
// either this port reading the wrong texcoord table or the guest genuinely supplying zeros.
struct LytTrace {
    std::uint32_t pos_addr, size_addr, tex_addr, color_addr, alpha;
    std::int32_t n_tex;
    float x0, y0, x1, y1, u0, v0;
} g_lyt_trace{};

// Reason the last quad was rejected, so a non-zero `dropped` says which contract broke.
// 0 = none, 1 = vertex descriptor, 2 = VAT formats.
std::uint32_t g_lyt_drop_reason = 0;

// SDK values used by the guard below.
constexpr std::uint32_t kPosXY = 0;     // GX_POS_XY
constexpr std::uint32_t kClrRGBA = 1;   // GX_CLR_RGBA
constexpr std::uint32_t kTexST = 1;     // GX_TEX_ST
constexpr std::uint32_t kF32 = 4;       // GX_F32
constexpr std::uint32_t kRGBA8 = 5;     // GX_RGBA8
constexpr std::uint32_t kAttrNone = 0;  // GX_NONE
constexpr std::uint32_t kAttrDirect = 1;

// Upstream's CanSubmitLytDrawDirect checks the VAT as well as the descriptor: position must be
// XY/F32, colour RGBA/RGBA8 and every texcoord ST/F32.
bool lyt_vtx_attr_fmt_matches(unsigned n_tex, bool has_colors) {
    for (unsigned i = 0; i < n_tex; ++i) {
        std::uint32_t cnt = 0, type = 0, frac = 0;
        get_vat(0, kVaTex0 + i, &cnt, &type, &frac);
        if (cnt != kTexST || type != kF32) {
            return false;
        }
    }
    std::uint32_t cnt = 0, type = 0, frac = 0;
    get_vat(0, kVaPos, &cnt, &type, &frac);
    if (cnt != kPosXY || type != kF32) {
        return false;
    }
    if (has_colors) {
        get_vat(0, kVaClr0, &cnt, &type, &frac);
        if (cnt != kClrRGBA || type != kRGBA8) {
            return false;
        }
    }
    return true;
}

bool lyt_vtx_desc_matches(unsigned n_tex, bool has_colors) {
    if ((g_gx.vcd_lo & 0x1FFu) != 0u) {
        return false;  // no position/texture matrix indices
    }
    if (get_field(g_gx.vcd_lo, 2, 9) != kAttrDirect) {
        return false;
    }
    if (get_field(g_gx.vcd_lo, 2, 11) != kAttrNone) {
        return false;  // normals
    }
    if (get_field(g_gx.vcd_lo, 2, 13) != (has_colors ? kAttrDirect : kAttrNone)) {
        return false;
    }
    if (get_field(g_gx.vcd_lo, 2, 15) != kAttrNone) {
        return false;  // clr1
    }
    for (unsigned i = 0; i < 8u; ++i) {
        if (get_field(g_gx.vcd_hi, 2, i * 2u) != (i < n_tex ? kAttrDirect : kAttrNone)) {
            return false;
        }
    }
    return true;
}

void emit_lyt_draw_quad(std::uint32_t pos_addr, std::uint32_t size_addr, std::int32_t tex_coord_count,
                        std::uint32_t tex_coord_addr, const std::uint32_t* colors) {
    // GX has exactly 8 texture coordinates, so nw4r::lyt cannot ask for more; anything else would
    // overrun the packet below.
    if (pos_addr == 0 || size_addr == 0 || tex_coord_count < 0 || tex_coord_count > 8) {
        return;
    }
    const unsigned n_tex = static_cast<unsigned>(tex_coord_count);
    if (!lyt_vtx_desc_matches(n_tex, colors != nullptr)) {
        g_lyt_drop_reason = 1;
        ++g_lyt_skipped;
        return;
    }
    if (!lyt_vtx_attr_fmt_matches(n_tex, colors != nullptr)) {
        g_lyt_drop_reason = 2;
        ++g_lyt_skipped;
        return;
    }
    // This stands in for GXBegin, so pending VCD/VAT/genMode state goes out first.
    flush_dirty();
    mkwv::gx::Fifo* f = fifo();
    if (f == nullptr) {
        return;
    }
    ++g_lyt_emitted;
    const float x0 = read_guest_f32(pos_addr);
    const float y0 = read_guest_f32(pos_addr + 4u);
    const float x1 = static_cast<float>(x0 + read_guest_f32(size_addr));
    const float y1 = static_cast<float>(y0 - read_guest_f32(size_addr + 4u));

    g_lyt_trace.pos_addr = pos_addr;
    g_lyt_trace.size_addr = size_addr;
    g_lyt_trace.tex_addr = tex_coord_addr;
    g_lyt_trace.n_tex = tex_coord_count;
    g_lyt_trace.x0 = x0;
    g_lyt_trace.y0 = y0;
    g_lyt_trace.x1 = x1;
    g_lyt_trace.y1 = y1;
    // Corner 1, not corner 0: corner 0 of a sprite is legitimately (0,0), so reporting it proved
    // nothing. Corner 1 is the +s edge and should be non-zero for any real texcoord table.
    g_lyt_trace.u0 = n_tex != 0u ? read_guest_f32(tex_coord_addr + 8u) : 0.0f;
    g_lyt_trace.v0 = n_tex != 0u ? read_guest_f32(tex_coord_addr + 12u) : 0.0f;

    // pos(2) + optional colour(1) + 8 texcoords(2 each), four corners.
    std::uint8_t packet[4u * (2u + 1u + 8u * 2u) * 4u];
    std::uint32_t pos = 0;
    // The winding order and each corner's texcoord byte offset are load-bearing.
    append_lyt_vertex(packet, pos, x0, y0, tex_coord_addr, n_tex, 0u, colors, 0u);
    append_lyt_vertex(packet, pos, x1, y0, tex_coord_addr, n_tex, 8u, colors, 1u);
    append_lyt_vertex(packet, pos, x1, y1, tex_coord_addr, n_tex, 24u, colors, 3u);
    append_lyt_vertex(packet, pos, x0, y1, tex_coord_addr, n_tex, 16u, colors, 2u);
    f->write_u8(0x80);  // GX_DRAW_QUADS | GX_VTXFMT0
    f->write_u16(4);
    f->write_bytes(packet, pos);
}

} // namespace

extern "C" void nw4r__lyt__detail__DrawQuad_800847c0(CpuContext* ctx) {
    std::uint32_t colors[4];
    const std::uint32_t color_addr = ctx->gpr[7];
    const std::uint32_t* color_ptr = nullptr;
    if (color_addr != 0) {
        for (unsigned i = 0; i < 4u; ++i) {
            colors[i] = read_lyt_color(color_addr, i, false, 0xFFu);
        }
        color_ptr = colors;
    }
    emit_lyt_draw_quad(ctx->gpr[3], ctx->gpr[4], static_cast<std::int32_t>(ctx->gpr[5]), ctx->gpr[6], color_ptr);
}

extern "C" void nw4r__lyt__detail__DrawQuad_80084d20(CpuContext* ctx) {
    std::uint32_t colors[4];
    const std::uint32_t color_addr = ctx->gpr[7];
    const std::uint32_t* color_ptr = nullptr;
    if (color_addr != 0) {
        const std::uint32_t alpha = ctx->gpr[8];
        for (unsigned i = 0; i < 4u; ++i) {
            colors[i] = read_lyt_color(color_addr, i, true, alpha);
        }
        color_ptr = colors;
    }
    g_lyt_trace.color_addr = color_addr;
    g_lyt_trace.alpha = ctx->gpr[8];
    emit_lyt_draw_quad(ctx->gpr[3], ctx->gpr[4], static_cast<std::int32_t>(ctx->gpr[5]), ctx->gpr[6], color_ptr);
}

namespace mkwv::runtime {

int lyt_trace_line(char* out, unsigned cap) {
    return std::snprintf(out, cap,
                         "mkwv: LYT last pos=%08X size=%08X tex=%08X ntex=%d color=%08X alpha=%u"
                         " box=[%.1f %.1f %.1f %.1f] uv1=[%.3f %.3f] vat=%08X/%08X drop=%u\n",
                         g_lyt_trace.pos_addr, g_lyt_trace.size_addr, g_lyt_trace.tex_addr, g_lyt_trace.n_tex,
                         g_lyt_trace.color_addr, g_lyt_trace.alpha, g_lyt_trace.x0, g_lyt_trace.y0, g_lyt_trace.x1,
                         g_lyt_trace.y1, g_lyt_trace.u0, g_lyt_trace.v0, g_gx.vat_a[0], g_gx.vat_b[0],
                         g_lyt_drop_reason);
}

void lyt_quad_stats(std::uint32_t* emitted, std::uint32_t* skipped) {
    if (emitted != nullptr) {
        *emitted = g_lyt_emitted;
    }
    if (skipped != nullptr) {
        *skipped = g_lyt_skipped;
    }
}

} // namespace mkwv::runtime
