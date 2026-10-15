//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise Linux file-handle buffers through public musl APIs.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad-path 2> %t.path
// RUN: FileCheck %s < %t.path
// RUN: not %run %t bad-header 2> %t.header
// RUN: FileCheck %s < %t.header
// RUN: not %run %t bad-payload 2> %t.payload
// RUN: FileCheck %s < %t.payload
// RUN: not %run %t bad-mount-id 2> %t.mount
// RUN: FileCheck %s < %t.mount
//
// CHECK: Found bad

#define _GNU_SOURCE

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct file_handle *allocate_handle(unsigned handle_bytes) {
  struct file_handle *handle =
      calloc(1, sizeof(struct file_handle) + handle_bytes);
  if (handle) {
    handle->handle_bytes = handle_bytes;
  }
  return handle;
}

static int test_good(void) {
  struct file_handle *handle = allocate_handle(MAX_HANDLE_SZ);
  struct file_handle *oversized = allocate_handle(0);
  if (!handle || !oversized) {
    free(oversized);
    free(handle);
    return 1;
  }

  int mount_id = 0;
  int result = name_to_handle_at(AT_FDCWD, ".", handle, &mount_id, 0);
  if (result == 0) {
    int mount_fd = open(".", O_PATH | O_DIRECTORY);
    if (mount_fd >= 0) {
      int opened_fd = open_by_handle_at(mount_fd, handle, O_RDONLY);
      if (opened_fd >= 0) {
        close(opened_fd);
      }
      close(mount_fd);
    }
  }

  // Linux rejects oversized handles after reading only their fixed header.
  oversized->handle_bytes = MAX_HANDLE_SZ + 1;
  (void)open_by_handle_at(-1, oversized, O_RDONLY);

  free(oversized);
  free(handle);
  return 0;
}

static int test_bad_path(void) {
  char *path = malloc(2);
  struct file_handle *handle = allocate_handle(MAX_HANDLE_SZ);
  if (!path || !handle) {
    free(handle);
    free(path);
    return 1;
  }
  path[0] = '.';
  path[1] = '\0';

  int mount_id = 0;
  (void)name_to_handle_at(AT_FDCWD, path + 2, handle, &mount_id, 0);
  free(handle);
  free(path);
  return 0;
}

static int test_bad_header(void) {
  char *storage = malloc(sizeof(struct file_handle) - 1);
  if (!storage) {
    return 1;
  }

  (void)open_by_handle_at(-1, (struct file_handle *)storage, O_RDONLY);
  free(storage);
  return 0;
}

static int test_bad_payload(void) {
  struct file_handle *handle = allocate_handle(0);
  if (!handle) {
    return 1;
  }
  handle->handle_bytes = 1;

  (void)open_by_handle_at(-1, handle, O_RDONLY);
  free(handle);
  return 0;
}

static int test_bad_mount_id(void) {
  struct file_handle *handle = allocate_handle(MAX_HANDLE_SZ);
  char *mount_id = malloc(sizeof(int) - 1);
  if (!handle || !mount_id) {
    free(mount_id);
    free(handle);
    return 1;
  }

  (void)name_to_handle_at(AT_FDCWD, ".", handle, (int *)mount_id, 0);
  free(mount_id);
  free(handle);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strcmp(argv[1], "good")) {
    return test_good();
  }
  if (!strcmp(argv[1], "bad-path")) {
    return test_bad_path();
  }
  if (!strcmp(argv[1], "bad-header")) {
    return test_bad_header();
  }
  if (!strcmp(argv[1], "bad-payload")) {
    return test_bad_payload();
  }
  if (!strcmp(argv[1], "bad-mount-id")) {
    return test_bad_mount_id();
  }
  return 2;
}
