#!/usr/bin/env bash
# Run wiicompiled's translator over your own PAL RMCP01 dump and generate everything the Vita build
# reads from upstream/generated/. Same steps as upstream's build guide, base game only.
# Needs scripts/fetch-deps.sh and scripts/link-mkw-assets.sh first.
#   JOBS=N  translator threads (default: all CPU threads)
set -euo pipefail
. "$(dirname "$0")/lib/host.sh"
refuse_native_windows

root="$(cd "$(dirname "$0")/.." && pwd)"
up="${root}/upstream"
jobs="${JOBS:-$(host_jobs)}"

require_cmd dotnet "install the .NET 8 SDK (see BUILD.md)"
[[ -d "${up}/translator" ]] || { echo "upstream/ is missing: run scripts/fetch-deps.sh" >&2; exit 1; }
[[ -e "${up}/Assets/main.dol" ]] || { echo "upstream/Assets is missing: run scripts/link-mkw-assets.sh" >&2; exit 1; }

cd "${up}"
cli="${up}/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll"
translator() { dotnet "${cli}" "$@"; }

echo "== building the translator"
dotnet build translator/src/Translator.Cli/Translator.Cli.csproj -c Release -v quiet -nologo

mkdir -p generated/functions generated/build_shards build/base

echo "== translating (${jobs} threads)"
translator translate-recursive 0x800060A4 \
  --project projects/mkwii/recomp.yml \
  --outdir generated/functions \
  --output-metadata generated/base_translation_output.json \
  --production-source-bundle generated/base_translation_sources.bin \
  --no-function-files --prune-stale --threads "${jobs}"

echo "== base manifest"
translator emit-base-manifest \
  --project projects/mkwii/recomp.yml \
  --out build/base \
  --functions-dir generated/functions \
  --translation-output-metadata generated/base_translation_output.json \
  --region P

echo "== guest data"
translator generate-data-init --project projects/mkwii/recomp.yml

echo "== build shards"
translator emit-build-shards \
  --project projects/mkwii/recomp.yml \
  --base-metadata generated/base_translation_output.json \
  --base-functions-dir generated/functions \
  --native-source-dir runtime/src \
  --out generated/build_shards

[[ -f generated/build_shards/shards.cmake ]] || { echo "translation finished without shards.cmake" >&2; exit 1; }
echo "done: $(ls generated/build_shards/base_common/shard_*.cpp | wc -l | tr -d ' ') shards in upstream/generated/build_shards"
