// RUN: %clang_raw -fsanitize=ptsan -S -emit-llvm -O1 -o - %s | FileCheck %s --check-prefix=DEFAULT
// RUN: %clang_raw --target=x86_64-unknown-linux-gnu -fsanitize=ptsan -S -emit-llvm -O1 -mllvm -ptsan-lam -o - %s | FileCheck %s --check-prefix=LAM

// DEFAULT: @ClLAM = weak_odr {{.*}}constant i32 0
// LAM: @ClLAM = weak_odr {{.*}}constant i32 1

int main(void) { return 0; }
