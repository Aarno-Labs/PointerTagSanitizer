// RUN: split-file %S/../coroutine.cpp %t
// RUN: %ptsanxx_musl_strict -std=c++20 %t/main.cpp %t/observe.cpp -o %t/test
// RUN: %run %t/test
// RUN: %ptsanxx_musl_id32_strict -std=c++20 \
// RUN:   %t/main.cpp %t/observe.cpp -o %t/id32
// RUN: %run %t/id32
