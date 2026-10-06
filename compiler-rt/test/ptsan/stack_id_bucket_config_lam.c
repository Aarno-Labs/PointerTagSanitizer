// REQUIRES: ptsan-lam-u48-runtime
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -g -O2 -mllvm -ptsan-lam -mllvm -ptsan-stack-ids-per-bucket=32767 -mllvm -ptsan-stack-id-thread-buckets=1 -o lam_too_wide %s
// RUN: not ./lam_too_wide 2> lam_too_wide.err
// RUN: FileCheck %s --check-prefix=LAM-TOO-WIDE < lam_too_wide.err

int main(void) {
  return 0;
}

// LAM-TOO-WIDE: PTSan stack ID configuration is invalid: reserved stack IDs exceed runtime ID space
