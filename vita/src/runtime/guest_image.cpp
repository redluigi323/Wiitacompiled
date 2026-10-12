#include "mkwv/runtime/guest_image.hpp"
#include "mkwv/runtime/compact_memory.hpp"
#include "mkwv/runtime/diagnostics.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdint>

namespace mkwv::runtime {
namespace {

struct Section {
    std::uint32_t addr;
    std::uint32_t size;
    const char* name;
};

constexpr Section kSections[] = {
    {0x80004000u, 9312u, "_init.bin"},
    {0x800072C0u, 2349856u, "_text.bin"},
    {0x80006460u, 1472u, "extab.bin"},
    {0x80006A20u, 2208u, "extabindex.bin"},
    {0x80244DE0u, 192u, "_ctors.bin"},
    {0x80244EA0u, 32u, "_dtors.bin"},
    {0x80244EC0u, 79552u, "_rodata.bin"},
    {0x80258580u, 309952u, "_data.bin"},
    {0x80384C00u, 5056u, "_sdata.bin"},
    {0x80386FA0u, 8608u, "_sdata2.bin"},
    {0x805102E0u, 4934832u, "StaticR.bin"},
};

bool join_path(char* out, std::size_t cap, const char* root, const char* name) {
    if (root == nullptr || name == nullptr || cap == 0) {
        return false;
    }
    const int n = std::snprintf(out, cap, "%s/%s", root, name);
    return n > 0 && static_cast<std::size_t>(n) < cap;
}

// Says which step failed, not just that one did. A silent false here means "guest image missing"
// with no indication of which of eleven sections went wrong.
const char* copy_file(CompactMemory& memory, std::uint32_t addr, std::uint32_t size, const char* path,
                      std::uint32_t* failed_at) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return "fopen";
    }
    static std::uint8_t buf[64u * 1024u];
    std::uint32_t done = 0;
    while (done < size) {
        std::uint32_t chunk = size - done;
        if (chunk > sizeof(buf)) {
            chunk = static_cast<std::uint32_t>(sizeof(buf));
        }
        const std::size_t got = std::fread(buf, 1, chunk, file);
        if (got != chunk) {
            *failed_at = done;
            std::fclose(file);
            return "fread";
        }
        if (!memory.write(addr + done, buf, got)) {
            *failed_at = done;
            std::fclose(file);
            return "memwrite";
        }
        done += chunk;
    }
    std::fclose(file);
    return nullptr;
}

} // namespace

GuestImageStats load_guest_image(CompactMemory& memory, const char* root) {
    GuestImageStats stats{};
    if (root == nullptr || root[0] == '\0' || !memory.is_active()) {
        return stats;
    }

    for (const Section& section : kSections) {
        char path[256];
        if (!join_path(path, sizeof(path), root, section.name)) {
            watchdog_log_line("mkwv: IMAGE path too long\n");
            return stats;
        }
        std::uint32_t failed_at = 0;
        if (const char* why = copy_file(memory, section.addr, section.size, path, &failed_at)) {
            char line[256];
            std::snprintf(line, sizeof(line), "mkwv: IMAGE FAILED %s at +%u/%u addr=%08X step=%s\n", section.name,
                          failed_at, section.size, section.addr, why);
            watchdog_log_line(line);
            return stats;
        }
        ++stats.sections;
        stats.bytes += section.size;
    }

    stats.init_word = memory.read32(0x80004000u);
    stats.loaded = true;
    return stats;
}

} // namespace mkwv::runtime
