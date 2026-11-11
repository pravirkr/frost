#!/usr/bin/env bash
# Mechanical checks for the rules in docs/architecture.md.
#
# Usage: scripts/check_architecture.sh
#   CXX=<compiler>     compiler for the standalone public-header check (default c++)
#   BUILD_DIR=<dir>    configured build directory supplying CPM _deps includes
#   SKIP_COMPILE=1     skip the standalone public-header check
#
# Exits non-zero and prints every violation found.

set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 2

status=0
fail() {
    printf '  %s\n' "$@"
    status=1
}
section() { printf '%s\n' "$1"; }

# All C++/CUDA sources under the given directories.
sources() {
    find "$@" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.cu' -o -name '*.cuh' \) 2>/dev/null | sort
}

# 1. Public headers carry no private or GPU toolkit code and no backend macro.
section "public headers are GPU-free and private-free"
while IFS= read -r line; do fail "$line"; done < <(
    grep -rnE '#[[:space:]]*include[[:space:]]*[<"](lib/|cuda/|hip/|thrust/|cub/|cuda_runtime|hip_runtime|cufft|hipfft)|FROST_ENABLE_(GPU|CUDA|HIP|PSRDADA)' include/)

# 2. Include prefixes and resolved private headers.
section "include prefixes are <frost/...>, \"lib/...\" or test \"support/...\""
while IFS= read -r line; do fail "$line"; done < <(
    sources lib applications |
        xargs grep -nE '#[[:space:]]*include[[:space:]]*"[^"]*/' |
        grep -vE '#[[:space:]]*include[[:space:]]*"lib/')
while IFS= read -r line; do fail "$line"; done < <(
    sources tests/cpp |
        xargs grep -nE '#[[:space:]]*include[[:space:]]*"[^"]*/' |
        grep -vE '#[[:space:]]*include[[:space:]]*"(lib|support)/')
while IFS= read -r line; do fail "$line"; done < <(
    sources include lib applications tests/cpp |
        xargs grep -nE '#[[:space:]]*include[[:space:]]*"frost/|#[[:space:]]*include[[:space:]]*<frost/[^>]*(kernels|device_rfi|gpu_utils|gpu_compat)[^>]*>')
while IFS= read -r entry; do
    src=${entry%%:*}
    inc=$(printf '%s\n' "$entry" | sed -E 's/.*"([^"]+)".*/\1/')
    if [[ ! -f "$inc" ]]; then
        fail "$src: unresolved private include $inc"
    fi
done < <(sources lib tests/cpp | xargs grep -nE '#[[:space:]]*include[[:space:]]*"lib/')

# 3. Private lib/ headers, CPU/GPU layout and toolkit boundaries.
section "private lib/ and toolkit headers stay out of public and host code"
while IFS= read -r line; do fail "$line"; done < <(
    sources include applications tests/cpp/api |
        xargs grep -nE '#[[:space:]]*include[[:space:]]*[<"](lib/|cuda/|hip/|thrust/|cub/|cuda_runtime|hip_runtime|cufft|hipfft)')
while IFS= read -r line; do fail "$line"; done < <(
    sources lib | grep -vE '^lib/cuda/' |
        xargs grep -nE '#[[:space:]]*include[[:space:]]*[<"](lib/cuda/|cuda/|hip/|thrust/|cub/|cuda_runtime|hip_runtime|cufft|hipfft)')
while IFS= read -r f; do
    fail "host source outside domain/cpu directory: $f"
done < <(find lib -name '*.cpp' | sort | grep -vE '^lib/[^/]+/.+\.cpp$' || true)
while IFS= read -r f; do
    fail "host source inside lib/cuda/: $f"
done < <(find lib/cuda -name '*.cpp' 2>/dev/null | sort)
while IFS= read -r f; do
    fail "device source must be lib/cuda/*_cuda.cu: $f"
done < <(find lib -name '*.cu' | sort | grep -vE '^lib/cuda/[^/]+_cuda\.cu$' || true)
while IFS= read -r line; do fail "$line"; done < <(
    find lib/cuda -name '*.cu' 2>/dev/null | sort |
        xargs grep -nE '#[[:space:]]*include[[:space:]]*"lib/cpu/')

# 4. Backend macros only in dispatch .cpp files, lib/cuda and internal tests.
section "FROST_ENABLE_* only in dispatch code, lib/cuda and internal tests"
while IFS= read -r line; do fail "$line"; done < <(
    sources include lib applications tests/cpp |
        grep -vE '^lib/cuda/|^tests/cpp/internal/' |
        grep -vE '^lib/(common|config|dedisp|output|pipeline|prepare|search)/[a-z0-9_]+\.cpp$' |
        xargs grep -nE 'FROST_ENABLE_(GPU|CUDA|HIP|PSRDADA)')

# 5. fmt::format, never std::format: libstdc++ < 13 has no <format>.
section "no std::format or <format> (use fmt::format)"
while IFS= read -r line; do fail "$line"; done < <(
    sources include lib applications tests/cpp |
        xargs grep -nE 'std::(v?format|format_to|format_string|formatter)\b|#[[:space:]]*include[[:space:]]*<format>')

# 6. Public headers open only the namespaces their directory allows.
section "public namespaces follow the directory"
for h in $(find include/frost -mindepth 2 -name '*.hpp' | sort); do
    d=$(basename "$(dirname "$h")")
    case $d in
        common) ok='frost|detail' ;;
        utils) ok='frost::stats' ;;
        prepare) ok='frost::prepare::(rfi|spectra)' ;;
        *) ok="frost::$d" ;;
    esac
    while IFS= read -r ns; do fail "$h: namespace $ns"; done < <(
        grep -oE '^namespace [A-Za-z_:]+' "$h" | awk '{print $2}' | grep -vxE "$ok")
done

# 7. Every public header compiles on its own.
if [[ "${SKIP_COMPILE:-0}" != 1 ]]; then
    section "public headers compile standalone"
    cxx=${CXX:-c++}
    inc_flags=()
    deps_dir=""
    for cand in "${BUILD_DIR:-}" build build-ci-tidy build-ci build-release; do
        if [[ -n "$cand" && -d "$cand/_deps" ]]; then
            deps_dir="$cand/_deps"
            break
        fi
    done
    if [[ -n "$deps_dir" ]]; then
        for pkg in dmt psrio fmt spdlog; do
            if [[ -d "$deps_dir/${pkg}-src/include" ]]; then
                inc_flags+=(-isystem "$deps_dir/${pkg}-src/include")
            fi
        done
    fi
    if [[ -n "$deps_dir" ]] || echo '#include <dmt/common/backend.hpp>' |
        "$cxx" -std=c++20 -fsyntax-only -x c++ - >/dev/null 2>&1; then
        for h in $(cd include && find frost -name '*.hpp' | sort); do
            if ! out=$(echo "#include <$h>" |
                "$cxx" -std=c++20 -fsyntax-only -Iinclude "${inc_flags[@]+"${inc_flags[@]}"}" -x c++ - 2>&1); then
                fail "$h: $(echo "$out" | grep -m1 error)"
            fi
        done
    else
        echo "  (skipped standalone compile: configure a build directory or set BUILD_DIR)"
    fi
fi

if [[ $status -eq 0 ]]; then
    echo "architecture checks passed"
else
    echo "architecture checks FAILED (see docs/architecture.md)"
fi
exit $status
