# Shared helpers for the build scripts: macOS, Linux and Windows (WSL2).
# Source it, don't run it:  . "$(dirname "$0")/lib/host.sh"

host_os() {
  case "$(uname -s)" in
    Darwin) echo macos ;;
    Linux)
      if grep -qi microsoft /proc/version 2>/dev/null; then echo wsl; else echo linux; fi ;;
    MINGW*|MSYS*|CYGWIN*) echo windows-native ;;
    *) echo unknown ;;
  esac
}

# Number of CPU threads, for -j.
host_jobs() {
  if command -v nproc >/dev/null 2>&1; then nproc
  elif command -v sysctl >/dev/null 2>&1; then sysctl -n hw.ncpu
  else echo 4
  fi
}

# Physical RAM in GiB (rounded down), or 0 if unknown.
host_ram_gib() {
  if [[ -r /proc/meminfo ]]; then
    awk '/^MemTotal:/ { printf "%d\n", $2 / 1048576 }' /proc/meminfo
  elif command -v sysctl >/dev/null 2>&1; then
    echo $(( $(sysctl -n hw.memsize) / 1073741824 ))
  else
    echo 0
  fi
}

# Parallel jobs for the shard build: one per thread, but no more than one per ~0.6 GB of RAM
# (each shard compile peaks around 0.4-0.5 GB).
host_shard_jobs() {
  local jobs ram cap
  jobs="$(host_jobs)"
  ram="$(host_ram_gib)"
  if [[ "${ram}" -gt 0 ]]; then
    cap=$(( ram * 10 / 6 ))
    if [[ "${cap}" -lt 1 ]]; then cap=1; fi
    if [[ "${jobs}" -gt "${cap}" ]]; then jobs="${cap}"; fi
  fi
  echo "${jobs}"
}

sha256_files() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"
  else shasum -a 256 "$@"
  fi
}

# Windows %APPDATA% as a WSL path, or nothing.
wsl_appdata() {
  [[ "$(host_os)" == wsl ]] || return 0
  command -v cmd.exe >/dev/null 2>&1 || return 0
  local win
  win="$(cd /mnt/c 2>/dev/null && cmd.exe /c 'echo %APPDATA%' 2>/dev/null | tr -d '\r' || true)"
  if [[ -n "${win}" && "${win}" != "%APPDATA%" ]]; then
    wslpath -u "${win}"
  fi
}

# Prints Vita3K's ux0/data folder if one exists. VITA3K_UX0_DATA overrides the search.
vita3k_ux0_data() {
  if [[ -n "${VITA3K_UX0_DATA:-}" ]]; then
    echo "${VITA3K_UX0_DATA}"
    return 0
  fi
  local roots=() root appdata
  case "$(host_os)" in
    macos) roots+=("${HOME}/Library/Application Support/Vita3K/Vita3K" "${HOME}/Library/Application Support/Vita3K") ;;
    linux|wsl)
      roots+=("${XDG_DATA_HOME:-${HOME}/.local/share}/Vita3K/Vita3K" "${XDG_DATA_HOME:-${HOME}/.local/share}/Vita3K")
      appdata="$(wsl_appdata)"
      if [[ -n "${appdata}" ]]; then roots+=("${appdata}/Vita3K/Vita3K" "${appdata}/Vita3K"); fi
      ;;
  esac
  for root in "${roots[@]}"; do
    for sub in fs/ux0/data ux0/data; do
      if [[ -d "${root}/${sub}" ]]; then
        echo "${root}/${sub}"
        return 0
      fi
    done
  done
  return 0
}

# True for paths a Windows program will read through NTFS (WSL's /mnt/<drive>/...). WSL symlinks
# there are not Windows symlinks, so files have to be copied instead of linked.
is_windows_path() {
  [[ "$(host_os)" == wsl && "$1" == /mnt/[a-zA-Z]/* ]]
}

require_cmd() {
  local cmd="$1" hint="$2"
  if ! command -v "${cmd}" >/dev/null 2>&1; then
    echo "missing '${cmd}': ${hint}" >&2
    exit 1
  fi
}

refuse_native_windows() {
  if [[ "$(host_os)" == windows-native ]]; then
    echo "This needs WSL2 on Windows (Git Bash/MSYS is not supported). See BUILD.md." >&2
    exit 1
  fi
}
