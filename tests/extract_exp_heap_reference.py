"""Build the host test oracle from the game's actual translated SDK routines."""
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text()
addresses = ["80198798", "8019899C", "80198A78", "80198B40", "80198D88",
             "80198E38", "80199038", "80199180", "80199358"]
parts = ['#include "ppc_runtime.h"\n#include "abi_bridge.h"\n#include "memory.h"\n']
parts.append('#undef MKW_STATIC_TRANSLATED_CALL\n'
             '#define MKW_STATIC_TRANSLATED_CALL(address, entry, context) entry(context)\n')
for address in ["80006038", *addresses]:
    parts.append(f'extern "C" void func_{address}(CpuContext*);\n')
parts.append('extern "C" void func_80006038(CpuContext* cpu) {\n'
             'for (uint32_t i = 0; i < cpu->gpr[5]; ++i)\n'
             'Memory::Write8(cpu->gpr[3] + i, static_cast<uint8_t>(cpu->gpr[4]));\n}\n')
for address in addresses:
    start = source.index(f'extern "C" void func_{address}(')
    end = source.index('// RECOMP_GUEST_ABI', start)
    parts.append(source[start:end])
pathlib.Path(sys.argv[2]).write_text('\n'.join(parts))
