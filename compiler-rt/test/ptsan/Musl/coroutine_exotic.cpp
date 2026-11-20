// RUN: split-file %S/../coroutine_exotic.cpp %t
// RUN: %ptsanxx_musl_strict -std=c++20 \
// RUN:   -mllvm -coro-elide-branch-ratio=0 \
// RUN:   %t/main.cpp %t/allocator.cpp -o %t/test
// RUN: %run %t/test
// RUN: %ptsanxx_musl_id32_strict -std=c++20 \
// RUN:   -mllvm -coro-elide-branch-ratio=0 \
// RUN:   %t/main.cpp %t/allocator.cpp -o %t/id32
// RUN: %run %t/id32
