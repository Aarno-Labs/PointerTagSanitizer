// RUN: split-file %S/../noop_coroutine_promise.cpp %t
// RUN: %ptsanxx_musl_strict -std=c++20 -DUSE_STD_COROUTINE \
// RUN:   %t/main.cpp %t/copy.cpp -o %t/id16
// RUN: %run %t/id16
// RUN: %ptsanxx_musl_id32_strict -std=c++20 -DUSE_STD_COROUTINE \
// RUN:   %t/main.cpp %t/copy.cpp -o %t/id32
// RUN: %run %t/id32
