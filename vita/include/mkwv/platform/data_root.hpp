#pragma once

// Canonical ux0 layout for the Vita port. The folder pun is viicompiled
// (Vita + WiiCompiled). Nintendo disc bytes are never in the VPK; they live
// here as host-side symlinks created by scripts/setup-ux0-data.sh.
//
//   ux0:data/viicompiled/guest    relocated DOL/REL blobs
//   ux0:data/viicompiled/overlay  translated overlay shards
//   ux0:data/viicompiled/disc     -> WIT DATA/files  (DVD root /)
//   ux0:data/viicompiled/sys      -> WIT DATA/sys    (fst.bin, boot.bin)
//   ux0:data/mkwv                 -> viicompiled     (compat)

namespace mkwv::platform {

inline constexpr const char* kDataRoot = "ux0:data/viicompiled";
inline constexpr const char* kGuestDir = "ux0:data/viicompiled/guest";
inline constexpr const char* kOverlayDir = "ux0:data/viicompiled/overlay";
inline constexpr const char* kDiscDir = "ux0:data/viicompiled/disc";
inline constexpr const char* kSysDir = "ux0:data/viicompiled/sys";
inline constexpr const char* kBootLog = "ux0:data/viicompiled/boot.log";
inline constexpr const char* kFstPath = "ux0:data/viicompiled/sys/fst.bin";

} // namespace mkwv::platform
