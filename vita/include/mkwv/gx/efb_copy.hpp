#pragma once

// EFB -> texture copies (GXCopyTex).
//
// Deliberately its own header rather than part of mkwv/gx/fifo.hpp or types.hpp: those
// reach every translated shard through memory_access.h -> hle_fifo.hpp, and this feature
// needs neither a new Processor field nor a new FIFO method - the natives write an escape
// command through Fifo's existing public write_* API, and the decoder records the event
// in file-scope state. So none of this forces a shard rebuild.

#include <cstdint>

namespace mkwv::gx {

// FIFO escape sub-command, carried under kLoadAurora like the others. 0x0040 is unused by
// the Aurora sub-command set in types.hpp.
inline constexpr std::uint16_t kAuroraEfbCopyTex = 0x0040;

// One GXCopyTex, recorded at the point in the command stream where the guest issued it.
struct EfbCopyEvent {
    std::uint32_t draw_index = 0;  // the copy observes draws [0, draw_index)
    std::int32_t src_x = 0;        // EFB pixels, top-left origin
    std::int32_t src_y = 0;
    std::int32_t src_w = 0;
    std::int32_t src_h = 0;
    std::uint32_t dst_w = 0;       // texels - already halved by the guest when mipmap is set
    std::uint32_t dst_h = 0;
    std::uint32_t format = 0;      // GXTexFmt / GXCopyFmt as passed to GXSetTexCopyDst
    bool clear = false;            // GXCopyTex's clear flag: clear the EFB rect afterwards
    std::uint32_t clear_rgba = 0;  // GXSetCopyClear colour, r | g<<8 | b<<16 | a<<24
    bool clear_color = false, clear_alpha = false, clear_depth = false;
    std::uint32_t clear_z = 0xFFFFFFu;
    std::uint8_t pixel_fmt = 0;
    std::uint32_t dest_guest = 0;
    const std::uint8_t* dest_host = nullptr;
};

inline constexpr unsigned kMaxEfbCopyEvents = 16;

// This frame's copies, in stream order. Cleared by Processor::end_frame()/reset().
unsigned efb_copy_events(const EfbCopyEvent** out);

// GXSetCopyClear's colour (BP 0x4F/0x50), r | g<<8 | b<<16 | a<<24 - what GX clears the
// EFB to after a display copy, so what each new frame starts from.
std::uint32_t efb_clear_color();

// GXSetDispCopySrc's rectangle: the part of the EFB the display copy sends to the TV. The
// renderer presents exactly this region, so EFB space outside it (MKW renders its light
// textures below line 456 and copies them out) never reaches the screen. BP 0x49/0x4A are
// shared with the texture copy, so this is recorded by the native, not decoded.
void set_display_copy_source(int x, int y, int w, int h);
bool display_copy_source(int* x, int* y, int* w, int* h);

// For hle_gx.cpp: the tex-copy source/destination state GXSetTexCopySrc/Dst record, which
// GXCopyTex then freezes into an escape command.
struct TexCopySetup {
    std::uint16_t src_x = 0, src_y = 0, src_w = 0, src_h = 0;
    std::uint16_t dst_w = 0, dst_h = 0;
    std::uint32_t format = 0;
    bool mipmap = false;
    bool clamp_top = true, clamp_bottom = true;
};

class Fifo;

// Guest memory access for the decoder, installed by the runtime.
//
// The decoder has to read guest memory in two places the FIFO itself does not carry:
// GX_CMD_CALL_DL (the display list's bytes) and the indexed XF loads (the matrix arrays
// GXSetArray points CP 0xAC-0xAF at). Processor::resolve only consulted map_range, which
// nothing ever calls - so every display list and every indexed matrix load was dropped,
// visible as skip(dl=1 ixf=1) on every dialog frame. `depth` is the display-list nesting
// level, so a resolver that has to stage bytes can give each level its own buffer.
using GuestResolver = const std::uint8_t* (*)(std::uint32_t addr, std::uint32_t size, unsigned depth);
void set_guest_resolver(GuestResolver resolver);

// A direct host pointer for a guest effective address - the base of a texture image or of a
// TLUT load's source. Guest RAM is contiguous on the host, so one pointer covers the whole
// image even where the range crosses the runtime's internal page size.
using DirectResolver = const std::uint8_t* (*)(std::uint32_t addr);
void set_direct_resolver(DirectResolver resolver);

// A window of guest memory around `addr` that is one contiguous host buffer: returns the
// host pointer for guest address *base, valid for *size bytes, with addr inside, or nullptr.
// The vertex decoder resolves a vertex array's window once per draw with this and then
// indexes elements in place, instead of a resolver call plus a copy per element. Optional:
// without one (or where it returns nullptr) elements go through the GuestResolver.
using WindowResolver = const std::uint8_t* (*)(std::uint32_t addr, std::uint32_t* base, std::uint32_t* size);
void set_window_resolver(WindowResolver resolver);

// The last CALL_DL and indexed-XF load the decoder could not resolve this frame, so a
// skip(dl=N ixf=N) in the dump comes with the address it could not read.
struct GuestFetchFailure {
    std::uint32_t dl_addr = 0, dl_bytes = 0, dl_depth = 0;
    std::uint32_t ixf_kind = 0, ixf_base = 0, ixf_stride = 0, ixf_index = 0, ixf_words = 0;
    std::uint32_t ixf_why = 0;  // 1 no CP base/stride, 2 resolver refused, 3 XF address refused
};
GuestFetchFailure last_guest_fetch_failure();

// Writes the GXCopyTex escape. Lives in fifo.cpp beside the code that parses it, so the
// byte layout has exactly one definition.
bool write_efb_copy_escape(Fifo& fifo, const TexCopySetup& setup, bool clear, std::uint32_t dest_guest,
                           const std::uint8_t* dest_host);

} // namespace mkwv::gx
