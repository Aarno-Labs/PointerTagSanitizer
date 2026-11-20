// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt
// RUN: cat out.txt | FileCheck %s

// CHECK-DAG:[T0] pthread_mutex_timedlock should TIME OUT …
// CHECK-DAG:[T1] waiting on binary sem ... (will be flushed)
// CHECK-DAG:[T2] waiting on binary sem ... (will be cancelled)
// CHECK-DAG:[T0] timedlock result = ETIMEDOUT (110)
// CHECK-DAG:[MAIN] cancelling T2 while it waits
// CHECK-DAG:[MAIN] flushing semaphore to wake T1
// CHECK-DAG:[T1] woke up via FLUSH, did not decrement sem
// CHECK:[MAIN] All paths exercised -- done.


// Thanks chatgpt!
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

/* ---------------- minimal OSAL-style binary semaphore -------------- */
typedef struct
{
  pthread_mutex_t id;
  pthread_cond_t  cv;
  int             current_value;
  sig_atomic_t    flush_request;
} binsem_t;

static void binsem_init(binsem_t *s, int initial)
{
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);

  pthread_mutex_init(&s->id, &a);
  pthread_cond_init(&s->cv, NULL);
  s->current_value = initial ? 1 : 0;
  s->flush_request = 0;

  pthread_mutexattr_destroy(&a);
}

static void binsem_give(binsem_t *s)
{
  pthread_mutex_lock(&s->id);
  s->current_value = 1;
  pthread_cond_signal(&s->cv);
  pthread_mutex_unlock(&s->id);
}

static void binsem_flush(binsem_t *s)
{
  pthread_mutex_lock(&s->id);
  ++s->flush_request;
  pthread_cond_broadcast(&s->cv);
  pthread_mutex_unlock(&s->id);
}

static void binsem_destroy(binsem_t *s)
{
  pthread_cond_destroy(&s->cv);
  pthread_mutex_destroy(&s->id);
}

static int binsem_wait_abs(binsem_t *s, const struct timespec *abs)
{
  int rc = 0;
  sig_atomic_t fc;

  pthread_mutex_lock(&s->id);
  fc = s->flush_request;

  pthread_cleanup_push((void (*)(void *))pthread_mutex_unlock, &s->id);

  while (s->current_value == 0 && s->flush_request == fc)
  {
    rc = abs ? pthread_cond_timedwait(&s->cv, &s->id, abs)
             : pthread_cond_wait(&s->cv, &s->id);
    if (rc == ETIMEDOUT)
      break;
  }

  if (rc == 0 && s->flush_request == fc)
    s->current_value = 0;

  pthread_cleanup_pop(1);
  return rc;
}

static int binsem_wait_ms(binsem_t *s, int ms)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec  +=  ms / 1000;
  ts.tv_nsec += (ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
  return binsem_wait_abs(s, &ts);
}

/* ---------------- threads exercising each path --------------------- */
static binsem_t sem;

/* T0: try to lock `sem.id` with 1-s timeout.  Will time out because
 * main thread is holding the mutex during that period. */
static void *thread_timed_mutex_fail(void *arg)
{
  (void)arg;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += 1;

  printf("[T0] pthread_mutex_timedlock should TIME OUT …\n");
  int r = pthread_mutex_timedlock(&sem.id, &ts);
  printf("[T0] timedlock result = %s (%d)\n",
         r == 0 ? "success (unexpected)" :
         r == ETIMEDOUT ? "ETIMEDOUT" : strerror(r), r);
  if (r == 0) pthread_mutex_unlock(&sem.id);   /* shouldn’t happen */

  return NULL;
}

/* T1: will be awoken by FLUSH, not by taking the semaphore */
static void *thread_wait_flushed(void *arg)
{
  (void)arg;
  printf("[T1] waiting on binary sem ... (will be flushed)\n");
  binsem_wait_abs(&sem, NULL);
  printf("[T1] woke up via FLUSH, did not decrement sem\n");
  return NULL;
}

/* T2: cancelled while blocked in cond_wait */
static void *thread_wait_cancelled(void *arg)
{
  (void)arg;
  pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
  pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

  printf("[T2] waiting on binary sem ... (will be cancelled)\n");
  binsem_wait_ms(&sem, 5000);
  printf("[T2] ** should never print **\n");
  return NULL;
}

/* ---------------- main sequence ------------------------------------ */
int main(void)
{
  pthread_t t[3];
  binsem_init(&sem, 0);

  /* Lock the semaphore mutex *before* starting T0 so its timedlock waits */
  pthread_mutex_lock(&sem.id);

  pthread_create(&t[0], NULL, thread_timed_mutex_fail, NULL);
  pthread_create(&t[1], NULL, thread_wait_flushed,     NULL);
  pthread_create(&t[2], NULL, thread_wait_cancelled,   NULL);

  /* hold lock long enough for T0 to hit its timeout */
  sleep(2);
  pthread_mutex_unlock(&sem.id);

  printf("[MAIN] cancelling T2 while it waits\n");
  pthread_cancel(t[2]);               /* send cancel first                */

  printf("[MAIN] flushing semaphore to wake T1\n");
  binsem_flush(&sem);

  /* give the semaphore once everything else is done */
  binsem_give(&sem);

  for (int i = 0; i < 3; ++i)
    pthread_join(t[i], NULL);

  binsem_destroy(&sem);
  printf("[MAIN] All paths exercised -- done.\n");
  return 0;
}
