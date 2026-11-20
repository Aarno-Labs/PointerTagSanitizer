// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe

// clock_cousins_test.c
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define NOINLINE __attribute__((noinline))

static NOINLINE void print_ts(const char *label, const struct timespec *ts) {
  printf("%s: %ld.%09ld\n", label, (long)ts->tv_sec, ts->tv_nsec);
}

static NOINLINE void print_res(const char *label, clockid_t cid) {
  struct timespec r = {0, 0};
  if (clock_getres(cid, &r) == 0) {
    print_ts(label, &r);
  } else {
    printf("%s: clock_getres error: %s\n", label, strerror(errno));
  }
}

static NOINLINE void test_clock_gettime_all(void) {
  struct timespec ts = {0, 0};

  if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
    print_ts("CLOCK_REALTIME", &ts);
  } else {
    printf("CLOCK_REALTIME error: %s\n", strerror(errno));
  }

  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
    print_ts("CLOCK_MONOTONIC", &ts);
  } else {
    printf("CLOCK_MONOTONIC error: %s\n", strerror(errno));
  }

#ifdef CLOCK_MONOTONIC_RAW
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) == 0) {
    print_ts("CLOCK_MONOTONIC_RAW", &ts);
  } else {
    printf("CLOCK_MONOTONIC_RAW error: %s\n", strerror(errno));
  }
#endif

#ifdef CLOCK_BOOTTIME
  if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) {
    print_ts("CLOCK_BOOTTIME", &ts);
  } else {
    printf("CLOCK_BOOTTIME error: %s\n", strerror(errno));
  }
#endif

#ifdef CLOCK_TAI
  if (clock_gettime(CLOCK_TAI, &ts) == 0) {
    print_ts("CLOCK_TAI", &ts);
  } else {
    printf("CLOCK_TAI error: %s\n", strerror(errno));
  }
#endif

#ifdef CLOCK_PROCESS_CPUTIME_ID
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0) {
    print_ts("CLOCK_PROCESS_CPUTIME_ID", &ts);
  } else {
    printf("CLOCK_PROCESS_CPUTIME_ID error: %s\n", strerror(errno));
  }
#endif

#ifdef CLOCK_THREAD_CPUTIME_ID
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
    print_ts("CLOCK_THREAD_CPUTIME_ID", &ts);
  } else {
    printf("CLOCK_THREAD_CPUTIME_ID error: %s\n", strerror(errno));
  }
#endif
}

static NOINLINE void test_clock_getres_some(void) {
  print_res("RES REALTIME", CLOCK_REALTIME);
  print_res("RES MONOTONIC", CLOCK_MONOTONIC);
#ifdef CLOCK_MONOTONIC_RAW
  print_res("RES MONOTONIC_RAW", CLOCK_MONOTONIC_RAW);
#endif
}

static NOINLINE void test_clock_getcpuclockid_then_gettime(void) {
  clockid_t pidclk;
  if (clock_getcpuclockid(0, &pidclk) == 0) {
    struct timespec ts = {0, 0};
    if (clock_gettime(pidclk, &ts) == 0) {
      print_ts("clock_getcpuclockid(0)", &ts);
    } else {
      printf("clock_gettime(pidclk) error: %s\n", strerror(errno));
    }
  } else {
    printf("clock_getcpuclockid error: %s\n", strerror(errno));
  }
}

static NOINLINE void add_ns(struct timespec *ts, long add_ns) {
  ts->tv_nsec += add_ns;
  while (ts->tv_nsec >= 1000000000L) {
    ts->tv_nsec -= 1000000000L;
    ts->tv_sec += 1;
  }
  while (ts->tv_nsec < 0) {
    ts->tv_nsec += 1000000000L;
    ts->tv_sec -= 1;
  }
}

static NOINLINE void test_clock_nanosleep_rel_abs(void) {
  // Relative sleep ~1ms using CLOCK_MONOTONIC.
  struct timespec rel = {0, 1000000L};
  if (clock_nanosleep(CLOCK_MONOTONIC, 0, &rel, NULL) != 0) {
    printf("clock_nanosleep (rel) error: %s\n", strerror(errno));
  } else {
    printf("clock_nanosleep (rel) done\n");
  }

  // Absolute sleep ~1ms in the future.
  struct timespec now = {0, 0};
  if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
    add_ns(&now, 1000000L);
    if (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &now, NULL) != 0) {
      printf("clock_nanosleep (abs) error: %s\n", strerror(errno));
    } else {
      printf("clock_nanosleep (abs) done\n");
    }
  } else {
    printf("clock_gettime(CLOCK_MONOTONIC) before abs sleep: %s\n",
           strerror(errno));
  }
}

static NOINLINE void test_timespec_get_c11(void) {
#if __STDC_VERSION__ >= 201112L
  struct timespec ts = {0, 0};
  if (timespec_get(&ts, TIME_UTC) == TIME_UTC) {
    print_ts("timespec_get(TIME_UTC)", &ts);
  } else {
    printf("timespec_get failed\n");
  }
#else
  printf("timespec_get not available on this C standard level\n");
#endif
}

static NOINLINE void test_gettimeofday_legacy(void) {
  struct timeval tv;
  if (gettimeofday(&tv, NULL) == 0) {
    printf("gettimeofday: %ld.%06ld\n", (long)tv.tv_sec, (long)tv.tv_usec);
  } else {
    printf("gettimeofday error: %s\n", strerror(errno));
  }
}

int main(void) {
  test_clock_gettime_all();
  test_clock_getres_some();
  test_clock_getcpuclockid_then_gettime();
  test_clock_nanosleep_rel_abs();
  test_timespec_get_c11();
  test_gettimeofday_legacy();
  return 0;
}
