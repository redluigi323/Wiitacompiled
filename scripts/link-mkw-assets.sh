#!/usr/bin/env bash
# Point upstream/Assets at the local WIT extract without copying Nintendo bytes.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
dump="${root}/mariokartwii"
assets="${root}/upstream/Assets"

if [[ ! -f "${dump}/DATA/sys/main.dol" || ! -f "${dump}/DATA/files/rel/StaticR.rel" ]]; then
  echo "link-mkw-assets: expected WIT extract at ${dump}/DATA/sys/main.dol" >&2
  exit 1
fi

mkdir -p "${assets}/DATA"
ln -sfn "${dump}/DATA/sys/main.dol" "${assets}/main.dol"
ln -sfn "${dump}/DATA/files/rel/StaticR.rel" "${assets}/StaticR.rel"
ln -sfn "${dump}/DATA/files" "${assets}/DATA/files"
ln -sfn "${dump}/DATA/sys" "${assets}/DATA/sys"

echo "linked ${assets}/main.dol -> $(readlink "${assets}/main.dol")"
echo "linked ${assets}/StaticR.rel -> $(readlink "${assets}/StaticR.rel")"
shasum -a 256 "${assets}/main.dol" "${assets}/StaticR.rel"
