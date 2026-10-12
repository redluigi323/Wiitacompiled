#!/usr/bin/env bash
set -euo pipefail

# Installs vitasdk via vdpm when VITASDK is unset.
# See https://vitasdk.org

if [[ -n "${VITASDK:-}" && -x "${VITASDK}/bin/arm-vita-eabi-gcc" ]]; then
  echo "vitasdk already at ${VITASDK}"
  exit 0
fi

prefix="${VITASDK:-/usr/local/vitasdk}"
echo "Installing vitasdk into ${prefix}"
echo "Follow https://vitasdk.org for the current bootstrap. This script only"
echo "checks the result; the official installer owns the download."

if ! command -v vdpm >/dev/null 2>&1; then
  echo "vdpm not found. Install vitasdk first:"
  echo "  git clone https://github.com/vitasdk/vdpm"
  echo "  cd vdpm && ./bootstrap-vitasdk.sh && ./install-all.sh"
  exit 1
fi

if [[ ! -x "${prefix}/bin/arm-vita-eabi-gcc" ]]; then
  echo "arm-vita-eabi-gcc missing under ${prefix}"
  exit 1
fi

echo "OK ${prefix}"
