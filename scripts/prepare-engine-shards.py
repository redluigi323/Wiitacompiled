#!/usr/bin/env python3
"""Rename selected PPC definitions in build copies; all callers bind to native wrappers.

Unlike --wrap, this also intercepts calls within the defining shard. The original
translation remains available for the no_native_engine switch and guarded fallbacks.
Translator-owned sources remain unchanged. Statefree specializations retain PPC code.
"""
import pathlib
import re
import sys

source_dir, catalog, output_dir, cmake_file = map(pathlib.Path, sys.argv[1:])
addresses = set(re.findall(r'MKWV_ENGINE_ENTRY\(([0-9A-F]{8})\)', catalog.read_text()))
found = set()
output_dir.mkdir(parents=True, exist_ok=True)
lines = ['set(MKWV_ENGINE_SHARDS)']
for source in sorted(source_dir.glob('shard_*.cpp')):
    content = source.read_text()
    used = set()
    def replace(match):
        address = match[1]
        if address not in addresses:
            return match[0]
        if address in found:
            raise SystemExit(f'duplicate engine definition {address}')
        used.add(address)
        found.add(address)
        return match[0].replace(f'func_{address}', f'mkwv_ppc_{address}', 1)
    patched = re.sub(r'extern "C" void func_([0-9A-F]{8})\(CpuContext\* MKW_RESTRICT ctx\)\s*\{', replace, content)
    selected = source
    if used:
        selected = output_dir / source.name
        if not selected.exists() or selected.read_text() != patched:
            selected.write_text(patched)
    lines.append(f'list(APPEND MKWV_ENGINE_SHARDS "{selected.resolve()}")')
    lines.append(f'set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "{source.resolve()}")')
missing = addresses - found
if missing:
    raise SystemExit('missing engine definitions: ' + ' '.join(sorted(missing)))
cmake_file.write_text('\n'.join(lines) + '\n')
print(f'mkwv: {len(found)} engine entries redirected to native code; original PPC fallbacks retained')
