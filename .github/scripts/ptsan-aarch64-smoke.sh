#!/usr/bin/env bash
# Build and link only: no AArch64 execution or emulator is required.
set -euo pipefail

source_dir=$(cd "$(dirname "$0")/../.." && pwd)
compiler_build=${1:?Usage: ptsan-aarch64-smoke.sh COMPILER_BUILD [SMOKE_DIR]}
smoke_dir=${2:-$compiler_build/aarch64-smoke}
mkdir -p "$smoke_dir/packages" "$smoke_dir/sysroot"
smoke_dir=$(cd "$smoke_dir" && pwd)
sysroot=$smoke_dir/sysroot

# Ubuntu 24.04 runner packages. Extract locally; do not mutate the runner.
(
  cd "$smoke_dir/packages"
  apt-get download libc6-arm64-cross libc6-dev-arm64-cross \
    linux-libc-dev-arm64-cross libgcc-s1-arm64-cross libgcc-13-dev-arm64-cross \
    libstdc++6-arm64-cross libstdc++-13-dev-arm64-cross
  for package in ./*.deb; do
    dpkg-deb --extract "$package" "$sysroot"
  done
)
gcc_dir=$sysroot/usr/lib/gcc-cross/aarch64-linux-gnu/13
flags="--sysroot=$sysroot --gcc-install-dir=$gcc_dir -isystem $sysroot/usr/aarch64-linux-gnu/include -L$sysroot/usr/aarch64-linux-gnu/lib -B$sysroot/usr/aarch64-linux-gnu/lib -fuse-ld=lld"
cmake -S "$source_dir/compiler-rt" -B "$smoke_dir/runtime" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER="$compiler_build/bin/clang" \
  -DCMAKE_CXX_COMPILER="$compiler_build/bin/clang++" \
  -DCMAKE_C_COMPILER_TARGET=aarch64-linux-gnu \
  -DCMAKE_CXX_COMPILER_TARGET=aarch64-linux-gnu \
  -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" \
  -DLLVM_CONFIG_PATH="$compiler_build/bin/llvm-config" \
  -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
  -DCOMPILER_RT_SANITIZERS_TO_BUILD=ptsan \
  -DCOMPILER_RT_BUILD_BUILTINS=OFF \
  -DCOMPILER_RT_BUILD_LIBFUZZER=OFF \
  -DCOMPILER_RT_BUILD_MEMPROF=OFF \
  -DCOMPILER_RT_BUILD_PROFILE=OFF \
  -DCOMPILER_RT_BUILD_XRAY=OFF \
  -DCOMPILER_RT_INCLUDE_TESTS=OFF
ninja -C "$smoke_dir/runtime" -j"$(nproc)" ptsan

cat > "$smoke_dir/smoke.c" <<'SRC'
#include <stdlib.h>
__attribute__((noinline)) static void store(int *p, int value) { *p = value; }
int main(int argc, char **argv) {
  int *p = malloc(sizeof(*p));
  if (!p) return 1;
  store(p, argc);
  int result = *p != argc;
  free(p);
  return result;
}
SRC
for bits in 16 32; do
  for variant in regular validate; do
    extra=()
    if [[ $bits == 32 ]]; then extra+=(-fsanitize-ptsan-32-bit-ids); fi
    if [[ $variant == validate ]]; then extra+=(-fsanitize-ptsan-runtime-validate); fi
    # Keep builtin headers from Clang while selecting the cross-built runtime.
    "$compiler_build/bin/clang" --target=aarch64-linux-gnu \
      --sysroot="$sysroot" --gcc-install-dir="$gcc_dir" \
      -isystem "$sysroot/usr/aarch64-linux-gnu/include" \
      -isystem "$("$compiler_build/bin/clang" -print-resource-dir)/include" \
      -L"$sysroot/usr/aarch64-linux-gnu/lib" -B"$sysroot/usr/aarch64-linux-gnu/lib" \
      -fuse-ld=lld -resource-dir="$smoke_dir/runtime" \
      -O2 -fsanitize=ptsan "${extra[@]}" "$smoke_dir/smoke.c" \
      -o "$smoke_dir/smoke-id$bits-$variant"
    readelf -h "$smoke_dir/smoke-id$bits-$variant" | grep 'Machine:.*AArch64'
  done
done
