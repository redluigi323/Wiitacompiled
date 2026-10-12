"""Generate independent PPC oracles from the user's original translation."""
import pathlib
import re
import sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'scripts'))
from extract_translated import read_entries

source, catalog, output = map(pathlib.Path, sys.argv[1:])
addresses = {int(a, 16) for a in re.findall(r'MKWV_ENGINE_ENTRY\(([0-9A-F]{8})\)', catalog.read_text())}
found = read_entries(source, addresses)
if len(found) != len(addresses):
    raise SystemExit('missing reference functions')
parts = ['#include "abi_bridge.h"\n', '#undef MKW_STATIC_TRANSLATED_CALL\n#define MKW_STATIC_TRANSLATED_CALL(a,e,c) e(c)\n']
for addr in addresses:
    parts.append(f'extern "C" void engine_ref_{addr:08X}(CpuContext*);\n')
for addr in sorted(addresses):
    text = found[addr][1]
    for target in addresses:
        text = re.sub(rf'\bfunc_{target:08X}\b', f'engine_ref_{target:08X}', text)
    text = re.sub(r'InvokeDirectCpu<0x([0-9A-F]{8})u>\(ctx\);', lambda m: f'engine_ref_{m[1]}(ctx);', text)
    parts.append(text)
output.write_text('\n'.join(parts))
