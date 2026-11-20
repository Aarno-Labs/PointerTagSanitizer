//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise public musl SysV IPC control APIs with fixed command-selected
// descriptor and information structures.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad-message 2> %t.message
// RUN: FileCheck %s < %t.message
// RUN: not %run %t bad-shared-memory 2> %t.shm
// RUN: FileCheck %s < %t.shm
//
// CHECK: Found bad

#define _GNU_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>

static int remove_objects(int message_id, int shared_memory_id, int result) {
  if (message_id >= 0 && msgctl(message_id, IPC_RMID, NULL) != 0 && !result) {
    result = 20;
  }
  if (shared_memory_id >= 0 && shmctl(shared_memory_id, IPC_RMID, NULL) != 0 &&
      !result) {
    result = 21;
  }
  return result;
}

static int test_good(void) {
  int message_id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
  if (message_id < 0) {
    return 1;
  }

  int shared_memory_id = shmget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
  if (shared_memory_id < 0) {
    return remove_objects(message_id, -1, 2);
  }

  struct msqid_ds message_status;
  memset(&message_status, 0, sizeof(struct msqid_ds));
  if (msgctl(message_id, IPC_STAT, &message_status) != 0) {
    return remove_objects(message_id, shared_memory_id, 3);
  }
  if (msgctl(message_id, IPC_SET, &message_status) != 0) {
    return remove_objects(message_id, shared_memory_id, 4);
  }

  struct msginfo message_info;
  memset(&message_info, 0, sizeof(struct msginfo));
  if (msgctl(0, IPC_INFO, (struct msqid_ds *)&message_info) < 0 ||
      msgctl(0, MSG_INFO, (struct msqid_ds *)&message_info) < 0) {
    return remove_objects(message_id, shared_memory_id, 5);
  }

  struct shmid_ds shared_memory_status;
  memset(&shared_memory_status, 0, sizeof(struct shmid_ds));
  if (shmctl(shared_memory_id, IPC_STAT, &shared_memory_status) != 0) {
    return remove_objects(message_id, shared_memory_id, 6);
  }
  if (shmctl(shared_memory_id, IPC_SET, &shared_memory_status) != 0) {
    return remove_objects(message_id, shared_memory_id, 7);
  }

  struct shminfo shared_memory_limits;
  memset(&shared_memory_limits, 0, sizeof(struct shminfo));
  if (shmctl(0, IPC_INFO, (struct shmid_ds *)&shared_memory_limits) < 0) {
    return remove_objects(message_id, shared_memory_id, 8);
  }

  struct shm_info shared_memory_info;
  memset(&shared_memory_info, 0, sizeof(struct shm_info));
  if (shmctl(0, SHM_INFO, (struct shmid_ds *)&shared_memory_info) < 0) {
    return remove_objects(message_id, shared_memory_id, 9);
  }

  if (shmctl(shared_memory_id, SHM_LOCK, NULL) == 0) {
    if (shmctl(shared_memory_id, SHM_UNLOCK, NULL) != 0) {
      return remove_objects(message_id, shared_memory_id, 10);
    }
  } else if (errno != EPERM && errno != ENOMEM) {
    return remove_objects(message_id, shared_memory_id, 11);
  }

  return remove_objects(message_id, shared_memory_id, 0);
}

static int test_bad_message(void) {
  void *buffer = calloc(1, sizeof(struct msqid_ds) - 1);
  if (!buffer) {
    return 1;
  }
  (void)msgctl(-1, IPC_STAT, (struct msqid_ds *)buffer);
  free(buffer);
  return 0;
}

static int test_bad_shared_memory(void) {
  void *buffer = calloc(1, sizeof(struct shmid_ds) - 1);
  if (!buffer) {
    return 1;
  }
  (void)shmctl(-1, IPC_STAT, (struct shmid_ds *)buffer);
  free(buffer);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strcmp(argv[1], "good")) {
    return test_good();
  }
  if (!strcmp(argv[1], "bad-message")) {
    return test_bad_message();
  }
  if (!strcmp(argv[1], "bad-shared-memory")) {
    return test_bad_shared_memory();
  }
  return 2;
}
