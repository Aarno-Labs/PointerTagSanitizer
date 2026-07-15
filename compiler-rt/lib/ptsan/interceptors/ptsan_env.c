/* Copyright (C) 1991-2024 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <https://www.gnu.org/licenses/>.  */

// Adapted from glibc-2.3.9:getenv.c/setenv/putenv

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <search.h>

# define __set_errno(ev) ((errno) = (ev))

// Clear ids of things going into env. Makes us need a looot
// fewer env-handling-specific wrappers
inline const char *clear_id(const char *ptr) {
  uint64_t addr = (uint64_t)ptr & 0x0000ffffffffffff;
  return (const char *)addr;
}


// magically supplied by libc
extern char **environ;


static void *known_values;

# define KNOWN_VALUE(Str) \
  ({									      \
    void *value = tfind (Str, &known_values, (__compar_fn_t) strcmp);	      \
    value != NULL ? *(char **) value : NULL;				      \
  })
# define STORE_VALUE(Str) \
  tsearch (Str, &known_values, (__compar_fn_t) strcmp)

static pthread_mutex_t ptsan_envlock = PTHREAD_MUTEX_INITIALIZER;

#ifndef LOCK
# define LOCK   do { pthread_mutex_lock(&ptsan_envlock); } while (0)
#endif

#ifndef UNLOCK
# define UNLOCK do { pthread_mutex_unlock(&ptsan_envlock); } while (0)
#endif

char *__ptsan_getenv(const char *name) {
  if (environ == NULL || name[0] == '\0')
    return NULL;

  size_t len = strlen(name);
  for (char **ep = environ; *ep != NULL; ++ep) {
    if (name[0] == (*ep)[0] && strncmp(name, *ep, len) == 0 &&
        (*ep)[len] == '=')
      return *ep + len + 1;
  }

  return NULL;
}

/* If this variable is not NULL we allocated the current environment. */
static char **last_environ;

/* Core helper shared by setenv/putenv logic in glibc. */
static int
__add_to_environ (const char *name, const char *value, const char *combined,
                  int replace)
{
  char **ep;
  size_t size;

  /* Compute lengths before locking, so that the critical section is
     less of a performance bottleneck.  VALLEN is needed only if
     COMBINED is null (unfortunately GCC is not smart enough to deduce
     this; see the #pragma at the start of this file).  Testing
     COMBINED instead of VALUE causes setenv (..., NULL, ...)  to dump
     core now instead of corrupting memory later.  */
  const size_t namelen = strlen(name);
  size_t vallen;
  if (combined == NULL) {
    vallen = strlen(value) + 1;
  }

  LOCK;

  /* We have to get the pointer now that we have the lock and not earlier
     since another thread might have created a new environment.  */
  ep = environ;

  size = 0;
  if (ep != NULL) {
    for (; *ep != NULL; ++ep) {
      if (!strncmp(*ep, name, namelen) && (*ep)[namelen] == '=') {
        break;
      } else {
        ++size;
      }
    }
  }

  if (ep == NULL || *ep == NULL) {
    char **new_environ;

    /* We allocated this space; we can extend it.  Avoid using the raw
          reallocated pointer to avoid GCC -Wuse-after-free.  */
    uintptr_t ip_last_environ = (uintptr_t)last_environ;
    new_environ = (char **)realloc(last_environ, (size + 2) * sizeof(char *));
    if (new_environ == NULL) {
      UNLOCK;
      return -1;
    }

    if ((uintptr_t)environ != ip_last_environ) {
      /* Copy existing entries if environ was not ours. */
      memcpy((char *)new_environ, (char *)environ, size * sizeof(char *));
    }

    new_environ[size] = NULL;
    new_environ[size + 1] = NULL;
    ep = new_environ + size;

    last_environ = environ = new_environ;
  }

  if (*ep == NULL || replace) {
    char *np;

    /* If putenv()’s combined "NAME=VALUE" is supplied, reuse it directly. */
    if (combined != NULL) {
      np = (char *)combined;
    } else {
      const size_t varlen = namelen + 1 + vallen;
      char *new_value = malloc(varlen);
      if (new_value == NULL) {
        UNLOCK;
        return -1;
      }

      memcpy(new_value, name, namelen);
      new_value[namelen] = '=';
      memcpy(&new_value[namelen + 1], value, vallen);

      np = KNOWN_VALUE(new_value);
      if (np == NULL) {
        np = new_value;
        /* And remember the value.  */
        STORE_VALUE(np);
      } else {
        free(new_value);
      }
    }

    *ep = np;
  }

  UNLOCK;

  return 0;
}

int
__ptsan_setenv (const char *name, const char *value, int replace)
{
  if (name == NULL || *name == '\0' || strchr(name, '=') != NULL) {
    __set_errno(EINVAL);
    return -1;
  }
  return __add_to_environ(clear_id(name), clear_id(value), NULL, replace);
}

int
__ptsan_unsetenv (const char *name)
{
  size_t len;
  char **ep;

  if (name == NULL || *name == '\0' || strchr(name, '=') != NULL) {
    __set_errno(EINVAL);
    return -1;
  }

  len = strlen(name);

  LOCK;

  ep = environ;
  if (ep != NULL) {
    while (*ep != NULL) {
      if (!strncmp(*ep, name, len) && (*ep)[len] == '=') {
        /* Remove this entry by shifting later ones back. */
        char **dp = ep;
        do {
          dp[0] = dp[1];
        } while (*dp++);
        /* Continue: name could appear again. */
      } else {
        ++ep;
      }
    }
  }

  UNLOCK;
  return 0;
}

/* POSIX.9 clearenv (also present on many libcs). */
int
__ptsan_clearenv (void)
{
  LOCK;

  if (environ == last_environ && environ != NULL) {
    /* We allocated this environment, so we can free it. */
    free(environ);
    last_environ = NULL;
  }

  /* Drop the whole environment. */
  environ = NULL;

  UNLOCK;
  return 0;
}

int __ptsan_putenv(char *string) {
  const char *const name_end = strchr(string, '=');

  if (name_end != NULL) {
    char *name;
    name = malloc(name_end - string + 1);
    if (name == NULL) {
      return -1;
    }
    memcpy(name, string, name_end - string);
    name[name_end - string] = '\0';
    int result = __add_to_environ(clear_id(name), NULL, clear_id(string), 1);

    free(name);

    return result;
  }

  unsetenv(string);
  return 0;
}