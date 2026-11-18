// RUN: split-file %s %t.dir
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror %t.dir/queue.c %t.dir/main.c -o %t.id16
// RUN: %run %t.id16 > %t.id16.out
// RUN: FileCheck %s < %t.id16.out
// RUN: %ptsan_musl_id32_strict -Wall -Wextra -Werror %t.dir/queue.c %t.dir/main.c -o %t
// RUN: %run %t > %t.out
// RUN: FileCheck %s < %t.out

// CHECK: atomic-pointer-exchange-ok

// Cycle an MPSC queue through its embedded stub. The internal push is inlined
// when the queue becomes empty, while the following public push crosses a
// translation-unit boundary and observes the stub from atomic storage.

//--- queue.h
#ifndef QUEUE_H
#define QUEUE_H

#include <stdatomic.h>

struct Node {
  _Atomic(struct Node *) next;
};

struct Queue {
  _Atomic(struct Node *) head;
  struct Node *tail;
  struct Node stub;
};

void queueInit(struct Queue *queue);
void queuePush(struct Queue *queue, struct Node *node);
struct Node *queuePop(struct Queue *queue);
int queueReplaceStub(struct Queue *queue, struct Node *node);
int queueReplaceNodeWithStub(struct Queue *queue, struct Node *node);

#endif

//--- queue.c
#include "queue.h"

#include <stddef.h>

static __attribute__((always_inline)) inline void
push(struct Queue *queue, struct Node *node) {
  atomic_store_explicit(&node->next, NULL, memory_order_relaxed);
  struct Node *previous = atomic_exchange_explicit(
      &queue->head, node, memory_order_acq_rel);
  atomic_store_explicit(&previous->next, node, memory_order_release);
}

void queueInit(struct Queue *queue) {
  atomic_init(&queue->head, &queue->stub);
  queue->tail = &queue->stub;
  atomic_init(&queue->stub.next, NULL);
}

void queuePush(struct Queue *queue, struct Node *node) {
  push(queue, node);
}

struct Node *queuePop(struct Queue *queue) {
  struct Node *current = queue->tail;
  struct Node *next =
      atomic_load_explicit(&current->next, memory_order_acquire);
  if (current == &queue->stub) {
    if (next == NULL) {
      return NULL;
    }
    queue->tail = next;
    current = next;
    next = atomic_load_explicit(&current->next, memory_order_acquire);
  }
  if (next != NULL) {
    queue->tail = next;
    return current;
  }
  struct Node *head =
      atomic_load_explicit(&queue->head, memory_order_acquire);
  if (current != head) {
    return NULL;
  }
  push(queue, &queue->stub);
  next = atomic_load_explicit(&current->next, memory_order_acquire);
  if (next != NULL) {
    queue->tail = next;
    return current;
  }
  return NULL;
}

int queueReplaceStub(struct Queue *queue, struct Node *node) {
  // Exercise a locally computed pointer as the cmpxchg expected payload.
  struct Node *expected = &queue->stub;
  return atomic_compare_exchange_strong_explicit(
      &queue->head, &expected, node, memory_order_acq_rel,
      memory_order_acquire);
}

int queueReplaceNodeWithStub(struct Queue *queue, struct Node *node) {
  // Exercise a locally computed pointer as the cmpxchg desired payload.
  struct Node *expected = node;
  return atomic_compare_exchange_strong_explicit(
      &queue->head, &expected, &queue->stub, memory_order_acq_rel,
      memory_order_acquire);
}

//--- main.c
#include "queue.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  struct Queue *queue = malloc(sizeof(*queue));
  struct Queue *compare_queue = malloc(sizeof(*compare_queue));
  struct Node *first = malloc(sizeof(*first));
  struct Node *second = malloc(sizeof(*second));
  assert(queue != NULL && compare_queue != NULL && first != NULL &&
         second != NULL);

  queueInit(queue);
  queuePush(queue, first);
  assert(queuePop(queue) == first);
  queuePush(queue, second);

  queueInit(compare_queue);
  assert(queueReplaceStub(compare_queue, first));
  assert(queueReplaceNodeWithStub(compare_queue, first));
  queuePush(compare_queue, second);

  puts("atomic-pointer-exchange-ok");
  return 0;
}
