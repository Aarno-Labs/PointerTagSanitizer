//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise public musl APIs whose thread operations pass immediate or retained
// addresses through futex and clone on both x86-64 and AArch64.
// Returning a worker and joining it also exercises tagless stack cleanup
// through SYS_munmap. The x86-only arch_prctl checks live in arch_prctl.c.
//
// RUN: %ptsan_musl_strict -pthread -o %t %s
// RUN: %run %t

#define _GNU_SOURCE

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct thread_state {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  int worker_ready;
  int worker_can_exit;
};

static void *worker(void *argument) {
  struct thread_state *state = (struct thread_state *)argument;
  if (pthread_mutex_lock(&state->mutex) != 0) {
    return (void *)1;
  }

  state->worker_ready = 1;
  if (pthread_cond_signal(&state->condition) != 0) {
    pthread_mutex_unlock(&state->mutex);
    return (void *)2;
  }

  while (!state->worker_can_exit) {
    if (pthread_cond_wait(&state->condition, &state->mutex) != 0) {
      pthread_mutex_unlock(&state->mutex);
      return (void *)3;
    }
  }

  if (pthread_mutex_unlock(&state->mutex) != 0) {
    return (void *)4;
  }
  return NULL;
}

int main(void) {
  struct thread_state state;
  memset(&state, 0, sizeof(struct thread_state));
  if (pthread_mutex_init(&state.mutex, NULL) != 0) {
    return 2;
  }
  if (pthread_cond_init(&state.condition, NULL) != 0) {
    pthread_mutex_destroy(&state.mutex);
    return 3;
  }

  pthread_t thread;
  if (pthread_create(&thread, NULL, worker, &state) != 0) {
    pthread_cond_destroy(&state.condition);
    pthread_mutex_destroy(&state.mutex);
    return 4;
  }

  int result = 0;
  if (pthread_mutex_lock(&state.mutex) != 0) {
    result = 5;
  } else {
    while (!state.worker_ready) {
      if (pthread_cond_wait(&state.condition, &state.mutex) != 0) {
        result = 6;
        break;
      }
    }
    state.worker_can_exit = 1;
    if (pthread_cond_signal(&state.condition) != 0 && !result) {
      result = 7;
    }
    if (pthread_mutex_unlock(&state.mutex) != 0 && !result) {
      result = 8;
    }
  }

  void *worker_result = NULL;
  if (pthread_join(thread, &worker_result) != 0 && !result) {
    result = 9;
  }
  if (worker_result && !result) {
    result = 10;
  }
  if (pthread_cond_destroy(&state.condition) != 0 && !result) {
    result = 11;
  }
  if (pthread_mutex_destroy(&state.mutex) != 0 && !result) {
    result = 12;
  }
  return result;
}
