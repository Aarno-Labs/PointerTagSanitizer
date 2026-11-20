// RUN: split-file %s %t
// RUN: %clangxx_ptsan -std=c++20 -O2 -mllvm -ptsan-abort-on-error \
// RUN:   -mllvm -coro-elide-branch-ratio=0 \
// RUN:   %t/main.cpp %t/allocator.cpp -o %t/test
// RUN: %run %t/test
// RUN: %clangxx_ptsan_id32 -std=c++20 -O2 -mllvm -ptsan-abort-on-error \
// RUN:   -mllvm -coro-elide-branch-ratio=0 \
// RUN:   %t/main.cpp %t/allocator.cpp -o %t/id32
// RUN: %run %t/id32

// Use the same profitability override as Clang's coro_await_elidable regression
// so this test deterministically exercises the safe-elision path.

//--- main.cpp
#include <coroutine>
#include <new>

extern void observe(void *pointer);
extern int allocation_count();

[[noreturn]] __attribute__((noinline)) static void fail() { __builtin_trap(); }

static void check(bool condition) {
  if (!condition)
    fail();
}

struct FailableTask {
  struct promise_type;
  using Handle = std::coroutine_handle<promise_type>;

  struct promise_type {
    static void *operator new(__SIZE_TYPE__ size) noexcept {
      if (fail_next_allocation) {
        fail_next_allocation = false;
        return nullptr;
      }
      return ::operator new(size, std::nothrow);
    }
    static void operator delete(void *allocation, __SIZE_TYPE__) noexcept {
      ::operator delete(allocation);
    }

    static FailableTask get_return_object_on_allocation_failure() noexcept {
      return FailableTask({}, true);
    }
    FailableTask get_return_object() noexcept {
      return FailableTask(Handle::from_promise(*this), false);
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_value(int result) noexcept { value = result; }
    void unhandled_exception() noexcept { fail(); }

    int value = -1;
  };

  static bool fail_next_allocation;

  FailableTask(Handle handle, bool allocation_failed)
      : handle(handle), allocation_failed(allocation_failed) {}
  FailableTask(const FailableTask &) = delete;
  FailableTask(FailableTask &&other) noexcept
      : handle(other.handle), allocation_failed(other.allocation_failed) {
    other.handle = {};
  }
  ~FailableTask() {
    if (handle)
      handle.destroy();
  }

  Handle handle;
  bool allocation_failed;
};

bool FailableTask::fail_next_allocation;

__attribute__((noinline)) static FailableTask make_failable(int value) {
  co_return value;
}

struct [[clang::coro_await_elidable]] ElidableTask {
  struct promise_type;
  using Handle = std::coroutine_handle<promise_type>;

  struct promise_type {
    struct FinalAwaiter {
      bool await_ready() const noexcept { return false; }
      std::coroutine_handle<> await_suspend(Handle finished) const noexcept {
        std::coroutine_handle<> continuation = finished.promise().continuation;
        return continuation ? continuation : std::noop_coroutine();
      }
      void await_resume() const noexcept {}
    };

    ElidableTask get_return_object() noexcept {
      return ElidableTask(Handle::from_promise(*this));
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    FinalAwaiter final_suspend() noexcept { return {}; }
    void return_value(int result) noexcept { value = result; }
    void unhandled_exception() noexcept { fail(); }

    std::coroutine_handle<> continuation;
    int value = -1;
  };

  struct Awaiter {
    Handle handle;

    explicit Awaiter(Handle handle) : handle(handle) {}
    Awaiter(const Awaiter &) = delete;
    Awaiter(Awaiter &&other) noexcept : handle(other.handle) {
      other.handle = {};
    }
    ~Awaiter() {
      if (handle)
        handle.destroy();
    }

    bool await_ready() const noexcept { return false; }
    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> continuation) noexcept {
      handle.promise().continuation = continuation;
      return handle;
    }
    int await_resume() noexcept {
      int result = handle.promise().value;
      Handle finished = handle;
      handle = {};
      finished.destroy();
      return result;
    }
  };

  explicit ElidableTask(Handle handle) : handle(handle) {}
  ElidableTask(const ElidableTask &) = delete;
  ElidableTask(ElidableTask &&other) noexcept : handle(other.handle) {
    other.handle = {};
  }
  ~ElidableTask() {
    if (handle)
      handle.destroy();
  }

  Awaiter operator co_await() && noexcept {
    Handle transferred = handle;
    handle = {};
    return Awaiter(transferred);
  }

  Handle handle;
};

static ElidableTask leaf(int value) {
  observe(&value);
  co_return value;
}

static ElidableTask
pass_through([[clang::coro_await_elidable_argument]] ElidableTask &&task) {
  co_return co_await static_cast<ElidableTask &&>(task);
}

__attribute__((noinline)) static ElidableTask aggressive_chain() {
  co_return co_await pass_through(leaf(73));
}

int main() {
  FailableTask::fail_next_allocation = true;
  {
    FailableTask failed = make_failable(11);
    check(failed.allocation_failed);
    check(!failed.handle);
    check(!FailableTask::fail_next_allocation);
  }

  {
    FailableTask task = make_failable(19);
    check(!task.allocation_failed);
    check(static_cast<bool>(task.handle));
    task.handle.resume();
    check(task.handle.done());
    check(task.handle.promise().value == 19);
  }

  // The outer coroutine is the only allocation. The two calls marked safe by
  // coro_await_elidable and coro_await_elidable_argument are nested in its
  // frame.
  {
    int allocations_before = allocation_count();
    ElidableTask task = aggressive_chain();
    check(allocation_count() == allocations_before + 1);
    task.handle.resume();
    check(task.handle.done());
    check(task.handle.promise().value == 73);
    check(allocation_count() == allocations_before + 1);
  }
}

//--- allocator.cpp
static int allocations;

void *operator new(__SIZE_TYPE__ size) {
  ++allocations;
  if (void *allocation = __builtin_malloc(size))
    return allocation;
  __builtin_trap();
}

void operator delete(void *allocation) noexcept { __builtin_free(allocation); }

void operator delete(void *allocation, __SIZE_TYPE__) noexcept {
  __builtin_free(allocation);
}

int allocation_count() { return allocations; }

void observe(void *) {}
