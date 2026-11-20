// RUN: split-file %s %t
// RUN: %clangxx_ptsan -std=c++20 -O2 -mllvm -ptsan-abort-on-error \
// RUN:   %t/main.cpp %t/copy.cpp -o %t/id16
// RUN: %run %t/id16
// RUN: %clangxx_ptsan_id32 -std=c++20 -O2 -mllvm -ptsan-abort-on-error \
// RUN:   %t/main.cpp %t/copy.cpp -o %t/id32
// RUN: %run %t/id32

// Access the object representation so merely forming a reference to the empty
// promise cannot make the test vacuous.

//--- main.cpp
#ifdef USE_STD_COROUTINE
#include <coroutine>
using NoopPromise = std::noop_coroutine_promise;
#else
struct NoopPromise {};
#endif

extern void copy_bytes(void *out, const void *in, __SIZE_TYPE__ size);

int main() {
  static_assert(sizeof(NoopPromise) == 1);
#ifdef USE_STD_COROUTINE
  auto handle = std::noop_coroutine();
  NoopPromise *promise = &handle.promise();
#else
  void *frame = __builtin_coro_noop();
  auto *promise = static_cast<NoopPromise *>(
      __builtin_coro_promise(frame, alignof(NoopPromise), false));
#endif
  unsigned char representation[sizeof(NoopPromise)];
  copy_bytes(representation, promise, sizeof(NoopPromise));
  return 0;
}

//--- copy.cpp
void copy_bytes(void *out, const void *in, __SIZE_TYPE__ size) {
  __builtin_memcpy(out, in, size);
}
