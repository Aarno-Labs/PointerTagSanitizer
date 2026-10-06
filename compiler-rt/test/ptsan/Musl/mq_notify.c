//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise public POSIX message-queue notification with both signal and thread
// delivery. The direct syscall mode covers Linux's nested SIGEV_THREAD cookie.
//
// RUN: %ptsan_musl_strict -pthread -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad-cookie 2> %t.bad-cookie
// RUN: FileCheck %s < %t.bad-cookie
//
// CHECK: Found bad

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

static int notification_pipe[2];
static void *expected_cookie;

static void notifyThread(union sigval value) {
  const char notification =
      value.sival_ptr == expected_cookie && *(char *)value.sival_ptr == 42;
  (void)write(notification_pipe[1], &notification, sizeof(char));
}

static int receiveByte(mqd_t queue) {
  char byte;
  return mq_receive(queue, &byte, sizeof(byte), NULL) == sizeof(byte) ? 0 : -1;
}

static int sendByte(mqd_t queue) {
  const char byte = 1;
  return mq_send(queue, &byte, sizeof(byte), 0);
}

static int waitForThreadNotification(void) {
  struct pollfd descriptor = {notification_pipe[0], POLLIN, 0};
  int poll_result;
  do {
    poll_result = poll(&descriptor, 1, 2000);
  } while (poll_result < 0 && errno == EINTR);
  if (poll_result != 1 || !(descriptor.revents & POLLIN)) {
    return -1;
  }
  char notification;
  if (read(notification_pipe[0], &notification, sizeof(char)) != sizeof(char)) {
    return -1;
  }
  return notification ? 0 : -1;
}

static int testGood(void) {
  char queue_name[64];
  snprintf(queue_name, sizeof(queue_name), "/ptsan-mq-notify-%ld",
           (long)getpid());
  (void)mq_unlink(queue_name);

  struct mq_attr attributes;
  memset(&attributes, 0, sizeof(struct mq_attr));
  attributes.mq_maxmsg = 1;
  attributes.mq_msgsize = 1;
  mqd_t queue =
      mq_open(queue_name, O_CREAT | O_EXCL | O_RDWR, 0600, &attributes);
  if (queue == (mqd_t)-1) {
    return 1;
  }

  int result = 0;
  expected_cookie = malloc(1);
  if (!expected_cookie) {
    result = 2;
    goto cleanup_queue;
  }
  *(char *)expected_cookie = 42;

  sigset_t signal_set;
  sigset_t old_mask;
  if (sigemptyset(&signal_set) != 0 || sigaddset(&signal_set, SIGUSR1) != 0 ||
      sigprocmask(SIG_BLOCK, &signal_set, &old_mask) != 0) {
    result = 3;
    goto cleanup_cookie;
  }

  struct sigevent event;
  memset(&event, 0, sizeof(struct sigevent));
  event.sigev_notify = SIGEV_SIGNAL;
  event.sigev_signo = SIGUSR1;
  event.sigev_value.sival_ptr = expected_cookie;
  if (mq_notify(queue, &event) != 0 || sendByte(queue) != 0) {
    result = 4;
    goto restore_mask;
  }

  siginfo_t info;
  memset(&info, 0, sizeof(siginfo_t));
  const struct timespec timeout = {1, 0};
  if (sigtimedwait(&signal_set, &info, &timeout) != SIGUSR1 ||
      info.si_value.sival_ptr != expected_cookie || receiveByte(queue) != 0) {
    result = 5;
    goto restore_mask;
  }
  if (sigprocmask(SIG_SETMASK, &old_mask, NULL) != 0) {
    result = 6;
    goto cleanup_cookie;
  }

  memset(&event, 0, sizeof(struct sigevent));
  event.sigev_notify = SIGEV_THREAD;
  event.sigev_notify_function = notifyThread;
  event.sigev_value.sival_ptr = expected_cookie;
  if (pipe(notification_pipe) != 0) {
    result = 7;
    goto cleanup_cookie;
  }
  if (mq_notify(queue, &event) != 0 || sendByte(queue) != 0 ||
      waitForThreadNotification() != 0 || receiveByte(queue) != 0) {
    result = 8;
    goto cleanup_thread_notification;
  }
  if (mq_notify(queue, NULL) != 0) {
    result = 9;
  }
cleanup_thread_notification:
  (void)close(notification_pipe[0]);
  (void)close(notification_pipe[1]);
  goto cleanup_cookie;

restore_mask:
  (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
cleanup_cookie:
  free(expected_cookie);
cleanup_queue:
  (void)mq_close(queue);
  (void)mq_unlink(queue_name);
  return result;
}

static int testBadCookie(void) {
  char queue_name[64];
  snprintf(queue_name, sizeof(queue_name), "/ptsan-mq-notify-bad-%ld",
           (long)getpid());
  (void)mq_unlink(queue_name);
  mqd_t queue = mq_open(queue_name, O_CREAT | O_EXCL | O_RDWR, 0600, NULL);
  if (queue == (mqd_t)-1) {
    return 1;
  }
  if (mq_unlink(queue_name) != 0) {
    (void)mq_close(queue);
    return 2;
  }

  int socket_descriptor = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, 0);
  if (socket_descriptor < 0) {
    (void)mq_close(queue);
    return 3;
  }

  struct sigevent event;
  memset(&event, 0, sizeof(struct sigevent));
  event.sigev_notify = SIGEV_THREAD;
  event.sigev_signo = socket_descriptor;

  char *cookie = malloc(31);
  if (!cookie) {
    return 4;
  }
  event.sigev_value.sival_ptr = cookie;
  (void)syscall(SYS_mq_notify, queue, &event);
  free(cookie);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strcmp(argv[1], "good")) {
    return testGood();
  }
  if (!strcmp(argv[1], "bad-cookie")) {
    return testBadCookie();
  }
  return 2;
}
