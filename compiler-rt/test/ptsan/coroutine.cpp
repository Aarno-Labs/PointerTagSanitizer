// RUN: split-file %s %t
// RUN: %clangxx_ptsan -std=c++20 -O2 -mllvm -ptsan-abort-on-error \
// RUN:   %t/main.cpp %t/observe.cpp -o %t/test
// RUN: %run %t/test
// RUN: %clangxx_ptsan_id32 -std=c++20 -O2 -mllvm -ptsan-abort-on-error \
// RUN:   %t/main.cpp %t/observe.cpp -o %t/id32
// RUN: %run %t/id32

//--- main.cpp
#include <coroutine>
#include <new>

extern void observe(void *pointer);

[[gnu::noinline, noreturn]] static void fail() { __builtin_trap(); }

static void check(bool condition) {
  if (!condition)
    fail();
}

// Task coroutines are kept out of line so their frames remain heap-backed.
struct Task {
  struct promise_type;
  using Handle = std::coroutine_handle<promise_type>;

  struct promise_type {
    int value = -1;
    bool caught_exception = false;

    Task get_return_object() noexcept {
      return Task(Handle::from_promise(*this));
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept { caught_exception = true; }
    std::suspend_always yield_value(int next) noexcept {
      value = next;
      return {};
    }
  };

  explicit Task(Handle handle) : handle(handle) {}
  Task(const Task &) = delete;
  Task(Task &&other) noexcept : handle(other.handle) { other.handle = {}; }
  ~Task() {
    if (handle)
      handle.destroy();
  }

  Handle handle;
};

[[gnu::noinline]] static Task sequence() {
  int values[] = {41, 42};
  co_yield values[0];
  co_yield values[1];
}

[[gnu::noinline]] static Task throws_after_suspend(int *state) {
  *state = 1;
  co_await std::suspend_always{};
  *state = 2;
  throw 7;
}

struct TransferTo {
  Task::Handle target;

  bool await_ready() const noexcept { return false; }
  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<>) const noexcept {
    return target;
  }
  void await_resume() const noexcept {}
};

[[gnu::noinline]] static Task transfer_target(int *state) {
  *state = 2;
  co_return;
}

[[gnu::noinline]] static Task transfer_source(Task::Handle target, int *state) {
  *state = 1;
  co_await TransferTo{target};
  *state = 3;
}

struct EagerTask {
  struct promise_type;
  using Handle = std::coroutine_handle<promise_type>;

  struct promise_type {
    static void *operator new(__SIZE_TYPE__ size) {
      ++allocation_count;
      return ::operator new(size);
    }
    static void operator delete(void *allocation, __SIZE_TYPE__) noexcept {
      ::operator delete(allocation);
    }

    EagerTask get_return_object() noexcept {
      return EagerTask(Handle::from_promise(*this));
    }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept { fail(); }
  };

  static int allocation_count;

  explicit EagerTask(Handle handle) : handle(handle) {}
  Handle handle;
};

int EagerTask::allocation_count;

static void run_elidable(int *state) {
  EagerTask task = [](int *out) -> EagerTask {
    int value = 2;
    *out = 1;
    co_await std::suspend_always{};
    observe(&value);
    *out = value;
  }(state);
  check(*state == 1);
  task.handle.resume();
  check(*state == 2);
}

int main() {
  // Repeated resume, promise access, spilled locals, and destruction.
  {
    Task task = sequence();
    check(!task.handle.done());
    task.handle.resume();
    check(!task.handle.done() && task.handle.promise().value == 41);
    task.handle.resume();
    check(!task.handle.done() && task.handle.promise().value == 42);
    task.handle.resume();
    check(task.handle.done());
  }

  // Exception handling after suspension.
  {
    int state = 0;
    Task task = throws_after_suspend(&state);
    task.handle.resume();
    check(state == 1 && !task.handle.done());
    task.handle.resume();
    check(state == 2 && task.handle.done());
    check(task.handle.promise().caught_exception);
  }

  // A handle returned from await_suspend performs a symmetric transfer.
  {
    int state = 0;
    Task target = transfer_target(&state);
    Task source = transfer_source(target.handle, &state);
    source.handle.resume();
    check(state == 2 && target.handle.done() && !source.handle.done());
    source.handle.resume();
    check(state == 3 && source.handle.done());
  }

  // This frame must be elided. The custom allocation counter makes the test
  // fail if the optimizer falls back to a heap-backed frame.
  {
    int state = 0;
    run_elidable(&state);
    check(state == 2);
    check(EagerTask::allocation_count == 0);
  }
}

//--- observe.cpp
void observe(void *) {}
