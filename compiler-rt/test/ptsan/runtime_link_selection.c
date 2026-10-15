// RUN: %clang_raw -fsanitize=ptsan -### %s 2>&1 | FileCheck %s --check-prefix=DEFAULT --implicit-check-not='libclang_rt.ptsan_{{.*}}.a'
// RUN: %clang_raw -fsanitize=ptsan -mllvm -ptsan-runtime-validate -### %s 2>&1 | FileCheck %s --check-prefix=VALIDATE --implicit-check-not='libclang_rt.ptsan.a' --implicit-check-not='libclang_rt.ptsan_lam.a' --implicit-check-not='libclang_rt.ptsan_lam_validate.a'
// RUN: %clang_raw --target=x86_64-unknown-linux-gnu -fsanitize=ptsan -mllvm -ptsan-lam -### %s 2>&1 | FileCheck %s --check-prefix=LAM --implicit-check-not='libclang_rt.ptsan.a' --implicit-check-not='libclang_rt.ptsan_validate.a' --implicit-check-not='libclang_rt.ptsan_lam_validate.a'
// RUN: %clang_raw --target=x86_64-unknown-linux-gnu -fsanitize=ptsan -mllvm -ptsan-lam -mllvm -ptsan-runtime-validate -### %s 2>&1 | FileCheck %s --check-prefix=LAM-VALIDATE --implicit-check-not='libclang_rt.ptsan.a' --implicit-check-not='libclang_rt.ptsan_validate.a' --implicit-check-not='libclang_rt.ptsan_lam.a'
// RUN: %clang_raw -target x86_64-unknown-linux-gnu -fsanitize=ptsan -fsyntax-only %s
// RUN: %clang_raw -target aarch64-unknown-linux-gnu -fsanitize=ptsan -fsyntax-only %s
// RUN: not %clang_raw -target aarch64_be-unknown-linux-gnu -fsanitize=ptsan -fsyntax-only %s 2>&1 | FileCheck %s --check-prefix=UNSUPPORTED-PTSAN
// RUN: not %clang_raw -target aarch64-none-elf -fsanitize=ptsan -fsyntax-only %s 2>&1 | FileCheck %s --check-prefix=UNSUPPORTED-BAREMETAL
// RUN: not %clang_raw -target aarch64-unknown-linux-gnu -fsanitize=ptsan -mllvm -ptsan-lam -### %s 2>&1 | FileCheck %s --check-prefix=UNSUPPORTED-LAM-DRIVER

// DEFAULT: libclang_rt.ptsan.a
// VALIDATE: libclang_rt.ptsan_validate.a
// LAM: libclang_rt.ptsan_lam.a
// LAM-VALIDATE: libclang_rt.ptsan_lam_validate.a
// UNSUPPORTED-PTSAN: unsupported option '-fsanitize=ptsan' for target 'aarch64_be-unknown-linux-gnu'
// UNSUPPORTED-BAREMETAL: unsupported option '-fsanitize=ptsan' for target 'aarch64-unknown-none-elf'
// UNSUPPORTED-LAM-DRIVER: unsupported option '-mllvm -ptsan-lam' for target 'aarch64-unknown-linux-gnu'

int main(void) { return 0; }
