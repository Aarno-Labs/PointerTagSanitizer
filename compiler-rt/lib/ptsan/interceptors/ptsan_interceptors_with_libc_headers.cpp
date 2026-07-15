// It's a lot nicer to be able to include libc headers
// to get types for interceptor arguments.

// However, that does not play well with the common interceptors,
// so we do that in a seperate file.

// Also some of these don't need libc headers, but recompiling common
// takes 5ever so much more convenient to put them here

#include "ptsan.h"
#include "ptsan_interceptors.h"
#include "ptsan_bounds.h"
#include "ptsan_thread.h"
#include "ptsan_ptr_utils.h"
#include "ptsan_drift.h"

#include "interception/interception.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <stdio.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pwd.h>
#include <fcntl.h>
#include <dirent.h>
#include <search.h>
#include <setjmp.h>
#include <nl_types.h>
#include <wchar.h>   // wcsncpy, wchar_t
#include <locale.h>
#include <glob.h>
struct obstack;

#include "sanitizer_common/sanitizer_linux.h"

using namespace __sanitizer;
using namespace __ptsan;

// Used for execv-`not-e` and tz-time family interceptors
extern char **environ;



/* Shared lock guarding temporary environ swaps */
static pthread_mutex_t ptsan_env_lock = PTHREAD_MUTEX_INITIALIZER;

// Swap in and out an untagged environment
#define PTSAN_ENV_GUARD_BEGIN()                                                 \
  ENSURE_PTSAN_INITED();                                                        \
  ptsan_in_libc++;                                                              \
  pthread_mutex_lock(&ptsan_env_lock);                                          \
  char **__ptsan_saved_env = environ;                                           \
  size_t __ptsan_envc = ptsan_strv_len(environ);                                \
  char **__ptsan_clean_envp = (char **)__builtin_alloca(                        \
      (__ptsan_envc + 1) * sizeof(*__ptsan_clean_envp));                        \
  ptsan_strv_deep_untag_into(environ, __ptsan_clean_envp, __ptsan_envc);        \
  environ = __ptsan_clean_envp;

#define PTSAN_ENV_GUARD_END()                                                   \
  environ = __ptsan_saved_env;                                                  \
  pthread_mutex_unlock(&ptsan_env_lock);                                        \
  ptsan_in_libc--;


// Redefine each internal_XXX libc function to clear ids and set return values.
// These are common enough (and there are few enough) that we can just ~do them by hand.~
// make chatGPT do them. Put them in a seperate file because they're Quite Long
#include "ptsan_libc.inc"

DECLARE_REAL(void *, calloc, SIZE_T nmemb, SIZE_T size)
DECLARE_REAL(void, free, void *ptr)

#define COMMON_INTERCEPTOR_ENTER(func, ...)                               \
  if (ptsan_init_is_running)                                                   \
    return REAL(func)(__VA_ARGS__);                                       \
  ENSURE_PTSAN_INITED();

// Take a (potentially tagged) null-terminated list and find
// how many elements it has
static size_t ptsan_strv_len(char *const *v_in) {
  char *const *v = clear_id(v_in);
  if (v == 0) {
    return 0;
  }
  size_t n = 0;
  while (v[n] != 0) {
    n++;
  }
  return n;
}

static void ptsan_strv_deep_untag_into(char *const *src_in, char **dst, size_t n) {
  char *const *src = clear_id(src_in);
  for (size_t i = 0; i < n; i++) {
    dst[i] = clear_id(src[i]);  // void* -> char* is implicit in C
  }
  dst[n] = 0;
}

/* Count how many varargs (each const char*) appear after arg0,
   up to but not including the terminating NULL. */
static size_t ptsan_count_varargs_until_null(va_list ap_in) {
  size_t n = 0;
  va_list ap;
  va_copy(ap, ap_in);
  for (;;) {
    const char *a = clear_id(va_arg(ap, const char *));
    if (a == 0) {
      break;
    }
    n++;
  }
  va_end(ap);
  return n;
}

/* Build argv[] from (arg0, varargs...) into dst for len entries
 *
 * This consumes entries in ap */
static void ptsan_build_argv_from_varargs_into(const char *arg0, va_list &ap,
                                              char **dst, size_t len) {
  size_t i = 0;
  CHECK(len > 0 && "varag with no arg0?");

  dst[i++] = const_cast<char *>(clear_id(arg0));

  while (i < len) {
    const char *a = va_arg(ap, const char *);
    CHECK(a != 0 && "null in varag list??");

    dst[i++] = const_cast<char *>(clear_id(a));
  }
  dst[i] = 0;
}


// ------------------------------
//  Mutexes
// ------------------------------
INTERCEPTOR(int, pthread_mutex_init,
            pthread_mutex_t *m, const pthread_mutexattr_t *attr) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_mutex_init)(clear_id(m), clear_id(attr));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_mutex_destroy, pthread_mutex_t *m) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_mutex_destroy)(clear_id(m));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_mutex_lock, pthread_mutex_t *m) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_mutex_lock)(clear_id(m));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_mutex_trylock, pthread_mutex_t *m) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_mutex_trylock)(clear_id(m));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_mutex_timedlock,
            pthread_mutex_t *m, const struct timespec *abstime) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res =
      REAL(pthread_mutex_timedlock)(clear_id(m), clear_id(abstime));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_mutex_unlock, pthread_mutex_t *m) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_mutex_unlock)(clear_id(m));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_mutexattr_init, pthread_mutexattr_t *__attr) {
  ENSURE_PTSAN_INITED();

  ptsan_in_libc++;
  int res = REAL(pthread_mutexattr_init)(clear_id(__attr));
  ptsan_in_libc--;

  return res;
}


INTERCEPTOR(int, pthread_mutexattr_setprotocol, pthread_mutexattr_t *__attr, int protocol) {
  ENSURE_PTSAN_INITED();

  ptsan_in_libc++;
  int res = REAL(pthread_mutexattr_setprotocol)(clear_id(__attr), protocol);
  ptsan_in_libc--;

  return res;
}

INTERCEPTOR(int, pthread_mutexattr_settype, pthread_mutexattr_t *__attr, int type) {
  ENSURE_PTSAN_INITED();

  ptsan_in_libc++;
  int res = REAL(pthread_mutexattr_settype)(clear_id(__attr), type);
  ptsan_in_libc--;

  return res;
}

INTERCEPTOR(int, pthread_attr_init, pthread_attr_t *__attr) {
  ENSURE_PTSAN_INITED();

  ptsan_in_libc++;
  int res = REAL(pthread_attr_init)(clear_id(__attr));
  ptsan_in_libc--;

  return res;
}

INTERCEPTOR(int, pthread_attr_setstack, pthread_attr_t *__attr, void *stackaddr, size_t stackSize) {
  ENSURE_PTSAN_INITED();

  ptsan_in_libc++;
  int res = REAL(pthread_attr_setstack)(clear_id(__attr), clear_id(stackaddr), stackSize);
  ptsan_in_libc--;

  return res;
}

INTERCEPTOR(int, pthread_attr_setdetachstate, pthread_attr_t *__attr, int __detachstate) {
  ENSURE_PTSAN_INITED();

  ptsan_in_libc++;
  int res = REAL(pthread_attr_setdetachstate)(clear_id(__attr), __detachstate);
  ptsan_in_libc--;

  return res;
}

INTERCEPTOR(int, pthread_attr_setstacksize, pthread_attr_t *__attr, size_t __stacksize) {
  ENSURE_PTSAN_INITED();

  ptsan_in_libc++;
  int res = REAL(pthread_attr_setstacksize)(clear_id(__attr), __stacksize);
  ptsan_in_libc--;

  return res;
}

INTERCEPTOR(int, pthread_attr_setinheritsched, pthread_attr_t *__attr, int __inherit) {
  ENSURE_PTSAN_INITED();

  ptsan_in_libc++;
  int res = REAL(pthread_attr_setinheritsched)(clear_id(__attr), __inherit);
  ptsan_in_libc--;

  return res;
}

INTERCEPTOR(int, pthread_attr_setschedpolicy, pthread_attr_t *__attr, int __policy) {
  ENSURE_PTSAN_INITED();

  ptsan_in_libc++;
  int res = REAL(pthread_attr_setschedpolicy)(clear_id(__attr), __policy);
  ptsan_in_libc--;

  return res;
}

INTERCEPTOR(int, pthread_attr_destroy, pthread_attr_t *__attr) {
  ENSURE_PTSAN_INITED();

  ptsan_in_libc++;
  int res = REAL(pthread_attr_destroy)(clear_id(__attr));
  ptsan_in_libc--;

  return res;
}

INTERCEPTOR(int, pthread_getattr_np, pthread_t thread, pthread_attr_t *attr) {
  ptsan_in_libc++;
  int out = REAL(pthread_getattr_np)(thread, clear_id(attr));
  ptsan_in_libc--;
  return out;
}

INTERCEPTOR(int, pthread_attr_setguardsize, pthread_attr_t *attr, size_t guardsize) {
  ptsan_in_libc++;
  int out = REAL(pthread_attr_setguardsize)(clear_id(attr), guardsize);
  ptsan_in_libc--;
  return out;
}


// ------------------------------
//  Condition variables
// ------------------------------
INTERCEPTOR(int, pthread_cond_init,
            pthread_cond_t *c, const pthread_condattr_t *attr) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_cond_init)(clear_id(c), clear_id(attr));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_cond_destroy, pthread_cond_t *c) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_cond_destroy)(clear_id(c));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_cond_wait,
            pthread_cond_t *c, pthread_mutex_t *m) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res =
      REAL(pthread_cond_wait)(clear_id(c), clear_id(m));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_cond_timedwait,
            pthread_cond_t *c, pthread_mutex_t *m,
            const struct timespec *abstime) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_cond_timedwait)(clear_id(c), clear_id(m),
                                         clear_id(abstime));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_cond_signal, pthread_cond_t *c) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_cond_signal)(clear_id(c));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_cond_broadcast, pthread_cond_t *c) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_cond_broadcast)(clear_id(c));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_condattr_init, pthread_condattr_t *attr) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(pthread_condattr_init)(
    (pthread_condattr_t *)clear_id((void *)attr)
  );

  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_condattr_setclock, pthread_condattr_t *attr, clockid_t clock_id) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(pthread_condattr_setclock)(
    (pthread_condattr_t *)clear_id((void *)attr),
    clock_id
  );

  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_condattr_destroy, pthread_condattr_t *attr) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(pthread_condattr_destroy)(
    (pthread_condattr_t *)clear_id((void *)attr)
  );

  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, timer_create, clockid_t __clock_id,
			 struct sigevent *__restrict __evp,
			 timer_t *__restrict __timerid) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  struct sigevent *cleared_evp = clear_id(__evp);
  // if(NULL != cleared_evp) {
  //   cleared_evp->_sigev_un._sigev_thread._function = clear_id(__evp->_sigev_un._sigev_thread._function);
  //   cleared_evp->_sigev_un._sigev_thread._attribute = clear_id(__evp->_sigev_un._sigev_thread._attribute);
  // }
  int res = REAL(timer_create)(__clock_id, cleared_evp, clear_id(__timerid));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(void, pthread_exit, void *retval) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  REAL(pthread_exit)(retval);
  ptsan_in_libc--; // unreachable
}

INTERCEPTOR(int, pthread_register_cancel, __pthread_unwind_buf_t *__buf) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_register_cancel)(clear_id_preserve_align(__buf));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(void, __pthread_unwind_next, __pthread_unwind_buf_t *exc)
{
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  REAL(__pthread_unwind_next)(clear_id_preserve_align(exc));

  /* the real function never returns; this is just to calm the compiler */
  __builtin_unreachable();
}

struct mq_attr {
               long mq_flags;       /* Flags: 0 or O_NONBLOCK */
               long mq_maxmsg;      /* Max. # of messages on queue */
               long mq_msgsize;     /* Max. message size (bytes) */
               long mq_curmsgs;     /* # of messages currently in queue */
           };

//TODO(epl): this should return mqd_t degrades to int for now
INTERCEPTOR(int, mq_open, const char *__name, int __oflag, ...) {

  mode_t mode = 0;
  struct mq_attr *attr = NULL;
  const char *__name_clear = clear_id(__name);
  if (__oflag & O_CREAT) {
      va_list ap;

      va_start (ap, __oflag);
      mode = va_arg (ap, mode_t);
      attr = clear_id(va_arg (ap, struct mq_attr *));
      va_end (ap);
  }
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = (__oflag & O_CREAT)
    ? REAL(mq_open)(__name_clear, __oflag, mode, attr)
    : REAL(mq_open)(__name_clear, __oflag);
  ptsan_in_libc--;
  return res;
}


INTERCEPTOR(int, mq_unlink, const char *name) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(mq_unlink)(clear_id(name));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(ssize_t, mq_receive, int mqdes, char *msg_ptr, size_t msg_len, unsigned *msg_prio) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  ssize_t res = REAL(mq_receive)(
    mqdes,
    clear_id(msg_ptr),
    msg_len,
    clear_id(msg_prio)
  );
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(ssize_t, mq_timedreceive, int mqdes, char *msg_ptr, size_t msg_len,
            unsigned *msg_prio, const struct timespec *abs_timeout) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  ssize_t res = REAL(mq_timedreceive)(
    mqdes,
    clear_id(msg_ptr),
    msg_len,
    clear_id(msg_prio),
    clear_id(abs_timeout)
  );
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, mq_timedsend, int mqdes, const char *msg_ptr, size_t msg_len,
            unsigned msg_prio, const struct timespec *abs_timeout) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(mq_timedsend)(
    mqdes,
    clear_id(msg_ptr),
    msg_len,
    msg_prio,
    clear_id(abs_timeout)
  );
  ptsan_in_libc--;
  return res;
}


#include <sys/socket.h>
#include <sys/uio.h>

INTERCEPTOR(ssize_t, sendmsg, int sockfd, const struct msghdr *msg, int flags) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  struct msghdr clean_msg;
  if (msg) {
    const struct msghdr *clear_msg = clear_id(msg);
    clean_msg = *clear_msg;

    // Pointers inside msghdr
    clean_msg.msg_name    = clear_id(clear_msg->msg_name);
    clean_msg.msg_control = clear_id(clear_msg->msg_control);

    if (clear_msg->msg_iov) {
      // Cast away const so we can walk the array
      struct iovec *iov = (struct iovec *)clear_id(clear_msg->msg_iov);
      for (size_t i = 0; i < clear_msg->msg_iovlen; i++) {
        iov[i].iov_base = clear_id(iov[i].iov_base);
      }
      clean_msg.msg_iov = (struct iovec *)clear_id((void *)clear_msg->msg_iov);
    }
  }

  ssize_t res = REAL(sendmsg)(
    sockfd,
    msg ? &clean_msg : NULL,
    flags
  );

  ptsan_in_libc--;
  return res;
}

// ------------------------------
//  Read-write locks
// ------------------------------
INTERCEPTOR(int, pthread_rwlock_init,
            pthread_rwlock_t *rw, const pthread_rwlockattr_t *attr) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_rwlock_init)(clear_id(rw), clear_id(attr));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_rwlock_destroy, pthread_rwlock_t *rw) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_rwlock_destroy)(clear_id(rw));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_rwlock_rdlock, pthread_rwlock_t *rw) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_rwlock_rdlock)(clear_id(rw));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_rwlock_tryrdlock, pthread_rwlock_t *rw) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_rwlock_tryrdlock)(clear_id(rw));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_rwlock_wrlock, pthread_rwlock_t *rw) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_rwlock_wrlock)(clear_id(rw));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_rwlock_trywrlock, pthread_rwlock_t *rw) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_rwlock_trywrlock)(clear_id(rw));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_rwlock_unlock, pthread_rwlock_t *rw) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_rwlock_unlock)(clear_id(rw));
  ptsan_in_libc--;
  return res;
}

/* sysconf */
INTERCEPTOR(long, sysconf, int name) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  long res = REAL(sysconf)(name);
  ptsan_in_libc--;
  return res;
}
/* pthread_cancel */
INTERCEPTOR(int, pthread_cancel, pthread_t thread) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_cancel)(thread);
  ptsan_in_libc--;
  return res;
}


// ------------------------------
//  Exceptions
// ------------------------------
INTERCEPTOR(void *, __cxa_allocate_exception, size_t sz) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  void * res = REAL(__cxa_allocate_exception)(sz);
  ptsan_in_libc--;
  return res;
}


// ------------------------------
//  Signals
// ------------------------------
// Note: some of these are in common. IDK why

// XXX: Put checks in all of these

// __sigaddset(set, sig)
INTERCEPTOR(int, sigaddset, sigset_t *set, int sig) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(sigaddset)(clear_id(set), sig);
  ptsan_in_libc--;
  return res;
}

// __sigdelset(set, sig)
INTERCEPTOR(int, sigdelset, sigset_t *set, int sig) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(sigdelset)(clear_id(set), sig);
  ptsan_in_libc--;
  return res;
}

// __sigismember(set, sig)
INTERCEPTOR(int, sigismember, const sigset_t *set, int sig) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(sigismember)(clear_id(set), sig);
  ptsan_in_libc--;
  return res;
}

// __sigisemptyset(set)
INTERCEPTOR(int, sigisemptyset, const sigset_t *set) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(sigisemptyset)(clear_id(set));
  ptsan_in_libc--;
  return res;
}

// __sigaddsetp(set, sig) – synonym that some libcs export
INTERCEPTOR(int, sigaddsetp, sigset_t *set, int sig) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(sigaddsetp)(clear_id(set), sig);
  ptsan_in_libc--;
  return res;
}

// __sigdelsetp(set, sig)
INTERCEPTOR(int, sigdelsetp, sigset_t *set, int sig) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(sigdelsetp)(clear_id(set), sig);
  ptsan_in_libc--;
  return res;
}

// Resources

INTERCEPTOR(int, getrusage, int who, struct rusage *usage) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(getrusage)(who, clear_id(usage));
  ptsan_in_libc--;
  return res;
}


INTERCEPTOR(int, getrlimit, int resource, struct rlimit *rlim) {

  COMMON_INTERCEPTOR_ENTER(getrlimit, resource, rlim);
  ptsan_in_libc++;
  int res = REAL(getrlimit)(resource, clear_id(rlim));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, setrlimit, int resource, const struct rlimit *rlim) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(setrlimit)(resource, clear_id(rlim));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, prlimit,
            pid_t pid, int resource,
            const struct rlimit *new_limit, struct rlimit *old_limit) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(prlimit)(pid, resource,
                          clear_id(new_limit), clear_id(old_limit));
  ptsan_in_libc--;
  return res;
}


//===----------------------------------------------------------------------===//
// ISO C90 stdio positioning primitives
//===----------------------------------------------------------------------===//

INTERCEPTOR(int, fseek,
            FILE *stream, long int offset, int whence) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(fseek)(clear_id(stream), offset, whence);
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(long int, ftell,
            FILE *stream) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  long int res = REAL(ftell)(clear_id(stream));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(void, rewind,
            FILE *stream) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  REAL(rewind)(clear_id(stream));
  ptsan_in_libc--;
}

INTERCEPTOR(int, fgetpos,
            FILE *stream, fpos_t *pos) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(fgetpos)(clear_id(stream), clear_id(pos));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, fsetpos,
            FILE *stream, const fpos_t *pos) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(fsetpos)(clear_id(stream), clear_id(pos));
  ptsan_in_libc--;
  return res;
}

//===----------------------------------------------------------------------===//
// getopt
//===----------------------------------------------------------------------===//


// int getopt(int argc, char *const argv[], const char *optstring);
INTERCEPTOR(int, getopt,
            int argc, char *const *argv, const char *optstring) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(getopt)(argc,
                         clear_id(argv),
                         clear_id(optstring));
  ptsan_in_libc--;
  return res;
}

static void copy_option(struct option &out, const struct option &in) {
  out.name = clear_id(in.name);
  out.has_arg = in.has_arg;
  out.flag = clear_id(in.flag);
  out.val = in.val;
}

// int getopt_long(int argc, char *const argv[], const char *optstring,
//                 const struct option *longopts, int *longindex);
INTERCEPTOR(int, getopt_long,
            int argc, char *const *argv, const char *optstring,
            const struct option *longopts, int *longindex) {
  ENSURE_PTSAN_INITED();

  ptsan_in_libc++;

  const struct option *lo_true = clear_id(longopts);
  int n = 0;
  while (lo_true[n].name != nullptr) {
    n++;
  }

  struct option *stripped = (struct option *)REAL(calloc)(n + 1, sizeof(struct option));
  for (int i = 0; i < n; i++) {
    copy_option(stripped[i], lo_true[i]);
  }

  int res = REAL(getopt_long)(
      argc,
      clear_id(argv),
      clear_id(optstring),
      stripped,
      clear_id(longindex));

  REAL(free)(stripped);

  ptsan_in_libc--;
  return res;
}

// int getopt_long_only(...)
INTERCEPTOR(int, getopt_long_only,
            int argc, char *const *argv, const char *optstring,
            const struct option *longopts, int *longindex) {
  ENSURE_PTSAN_INITED();

  const struct option *lo_true = clear_id(longopts);
  int n = 0;
  while (lo_true[n].name != nullptr) {
    n++;
  }

  struct option *stripped = (struct option *)REAL(calloc)(n + 1, sizeof(struct option));
  for (int i = 0; i < n; i++) {
    copy_option(stripped[i], lo_true[i]);
  }


  ptsan_in_libc++;
  int res = REAL(getopt_long_only)(
      argc,
      clear_id(argv),
      clear_id(optstring),
      stripped,
      clear_id(longindex));

  REAL(free)(stripped);

  ptsan_in_libc--;
  return res;
}

// int getsubopt(char **optionp, char *const *keylist, char **valuep);
INTERCEPTOR(int, getsubopt,
            char **optionp, char *const *keylist, char **valuep) {
  CHECK(false && "Implement me");
  return -1;
}

//===----------------------------------------------------------------------===//
// mkdir family interceptors
//===----------------------------------------------------------------------===//

// int mkdir(const char *pathname, mode_t mode);
INTERCEPTOR(int, mkdir,
            const char *pathname, mode_t mode) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(mkdir)(clear_id(pathname), mode);
  ptsan_in_libc--;
  return res;
}

// int mkdirat(int dirfd, const char *pathname, mode_t mode);
INTERCEPTOR(int, mkdirat,
            int dirfd, const char *pathname, mode_t mode) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(mkdirat)(dirfd, clear_id(pathname), mode);
  ptsan_in_libc--;
  return res;
}

//===----------------------------------------------------------------------===//
// getc/putc interceptors
//===----------------------------------------------------------------------===//

INTERCEPTOR(int, fgetc, FILE *stream) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(fgetc)(clear_id(stream));
  ptsan_in_libc--;
  return res;
}

/*--------------------------------------------------------------------*/
// int getc(FILE *stream);  (usually a macro, but a real symbol exists)
INTERCEPTOR(int, getc, FILE *stream) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(getc)(clear_id(stream));
  ptsan_in_libc--;
  return res;
}

// int getchar(void);
INTERCEPTOR(int, getchar) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(getchar)();
  ptsan_in_libc--;
  return res;
}

/*--------------------------------------------------------------------*/
// int putchar(int c);
INTERCEPTOR(int, putchar, int c) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(putchar)(c);
  ptsan_in_libc--;
  return res;
}

/*--------------------------------------------------------------------*/
// int fputc(int c, FILE *stream);
INTERCEPTOR(int, fputc, int c, FILE *stream) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(fputc)(c, clear_id(stream));
  ptsan_in_libc--;
  return res;
}

/*--------------------------------------------------------------------*/
// int putc(int c, FILE *stream);               (usually a macro)
INTERCEPTOR(int, putc, int c, FILE *stream) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(putc)(c, clear_id(stream));
  ptsan_in_libc--;
  return res;
}

//===----------------------------------------------------------------------===//
// uid / gid helpers
//===----------------------------------------------------------------------===//

// int setuid(uid_t uid);
INTERCEPTOR(int, setuid, uid_t uid) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(setuid)(uid);
  ptsan_in_libc--;
  return res;
}

// int setgid(gid_t gid);
INTERCEPTOR(int, setgid, gid_t gid) {                 /* “setguid” → setgid */
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(setgid)(gid);
  ptsan_in_libc--;
  return res;
}

// uid_t geteuid(void);
INTERCEPTOR(uid_t, geteuid) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  uid_t res = REAL(geteuid)();
  ptsan_in_libc--;
  return res;
}

// Opens

INTERCEPTOR(int, open, const char *pathname, int flags, ...) {
  mode_t mode = 0;

  if (flags & (O_CREAT | O_TMPFILE)) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);
  }

  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int fd = (flags & O_CREAT)
             ? REAL(open)(clear_id(pathname), flags, mode)
             : REAL(open)(clear_id(pathname), flags);

  ptsan_in_libc--;
  return fd;
}

/* opendir */
INTERCEPTOR(DIR *, opendir, const char *name) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  const char *clear_name = (const char *)clear_id(name);

  DIR *res = REAL(opendir)(clear_name);

  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, openat, int dirfd, const char *pathname, int flags, ...) {
  mode_t mode = 0;

  if (flags & (O_CREAT | O_TMPFILE)) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);
  }

  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int fd = (flags & O_CREAT)
             ? REAL(openat)(dirfd, clear_id(pathname), flags, mode)
             : REAL(openat)(dirfd, clear_id(pathname), flags);

  ptsan_in_libc--;
  return fd;
}

#include <dirent.h>

INTERCEPTOR(DIR *, __fdopendir, int fd) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  DIR *res = REAL(__fdopendir)(fd);

  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, creat, const char *pathname, mode_t mode) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int fd = REAL(creat)(clear_id(pathname), mode);
  ptsan_in_libc--;
  return fd;
}

INTERCEPTOR(int, open64, const char *pathname, int flags, ...) {
  mode_t mode = 0;

  if (flags & (O_CREAT | O_TMPFILE)) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);
  }

  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int fd = (flags & O_CREAT)
             ? REAL(open64)(clear_id(pathname), flags, mode)
             : REAL(open64)(clear_id(pathname), flags);

  ptsan_in_libc--;
  return fd;
}


INTERCEPTOR(int, openat64, int dirfd, const char *pathname, int flags, ...) {
  mode_t mode = 0;

  if (flags & (O_CREAT | O_TMPFILE)) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);
  }

  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int fd = (flags & O_CREAT)
             ? REAL(openat64)(dirfd, clear_id(pathname), flags, mode)
             : REAL(openat64)(dirfd, clear_id(pathname), flags);

  ptsan_in_libc--;
  return fd;
}

INTERCEPTOR(int, creat64, const char *pathname, mode_t mode) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int fd = REAL(creat64)(clear_id(pathname), mode);
  ptsan_in_libc--;
  return fd;
}

//mktemplikes

INTERCEPTOR(int, mkstemp, char *__template) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(mkstemp)(clear_id(__template));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, mkostemp, char *__template, int flags) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(mkostemp)(clear_id(__template), flags);
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, mkstemps, char *__template, int suffixlen) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(mkstemps)(clear_id(__template), suffixlen);
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, mkostemps, char *__template, int suffixlen, int flags) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(mkostemps)(clear_id(__template), suffixlen, flags);
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(char *, mktemp, char *__template) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  char *res = REAL(mktemp)(clear_id(__template));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(char *, mkdtemp, char *tmpl) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  char *res = REAL(mkdtemp)(clear_id(tmpl));
  res = copy_id(res, tmpl);
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, rmdir, const char *pathname) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(rmdir)(clear_id(pathname));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, chmod, const char *pathname, mode_t mode) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(chmod)(clear_id(pathname), mode);
  ptsan_in_libc--;
  return res;
}


INTERCEPTOR(int, closedir, DIR *dirp) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(closedir)(clear_id(dirp));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, link, const char *oldpath, const char *newpath) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(link)(clear_id(oldpath), clear_id(newpath));

  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, linkat, int olddirfd, const char *oldpath,
            int newdirfd, const char *newpath, int flags) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(linkat)(olddirfd, clear_id(oldpath),
                         newdirfd, clear_id(newpath), flags);

  ptsan_in_libc--;
  return res;
}


INTERCEPTOR(int, unlink, const char *pathname) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(unlink)(clear_id(pathname));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, unlinkat, int dirfd, const char *pathname, int flags) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(unlinkat)(dirfd, clear_id(pathname), flags);
  ptsan_in_libc--;
  return res;
}

// Search.h stuff

extern "C" {
  void *__ptsan_tfind(const void *key, void *const *rootp, int (*compar)(const void *, const void *));
  extern void *__ptsan_tsearch(const void *key, void **rootp, int (*compar)(const void *, const void *));
  void extern *__ptsan_tdelete(const void *key, void **rootp,  int (*compar)(const void *, const void *));
  extern void __ptsan_twalk(const void *root,  void (*action)(const void *node, VISIT v, int depth));
  extern void __ptsan_tdestroy(void *root, void (*free_node)(void *nodep));
}
/* tsearch(key, &root, compar) */
INTERCEPTOR(void *, tsearch,
            const void *key, void **rootp,
            int (*compar)(const void *, const void *)) {
  ENSURE_PTSAN_INITED();

  void *res = __ptsan_tsearch(key, rootp, compar);
  return res;
}

/* tfind(key, &root, compar) */
INTERCEPTOR(void *, tfind,
            const void *key, void * const *rootp,
            int (*compar)(const void *, const void *)) {
  ENSURE_PTSAN_INITED();

  void *res = __ptsan_tfind(key, rootp, compar);
  return res;
}

/* tdelete(key, &root, compar) */
INTERCEPTOR(void *, tdelete,
            const void *key, void **rootp,
            int (*compar)(const void *, const void *)) {
  ENSURE_PTSAN_INITED();

  void *res = __ptsan_tdelete(key, rootp, compar);
  return res;
}

/* twalk(root, action) */
INTERCEPTOR(void, twalk,
            const void *root,
            void (*action)(const void *node, VISIT v, int depth)) {
  ENSURE_PTSAN_INITED();

  __ptsan_twalk(root, action);
}

/* tdestroy(root, free_node) */
INTERCEPTOR(void, tdestroy,
            void *root,
            void (*free_node)(void *nodep)) {
  ENSURE_PTSAN_INITED();

  __ptsan_tdestroy(root, free_node);
}

typedef int (*qsort_r_compar_f)(const void *, const void *, void *);
extern "C" {
  void __ptsan_qsort (void *b, size_t n, size_t s, __compar_fn_t cmp);
  void __ptsan_qsort_r (void *const pbase, size_t total_elems, size_t size,
	   qsort_r_compar_f cmp, void *arg);
}

INTERCEPTOR(void, qsort,
            void *b, size_t n, size_t s, __compar_fn_t cmp) {
  __ptsan_qsort(b, n, s, cmp);
}

INTERCEPTOR(void, qsort_r,
            void *const pbase, size_t total_elems, size_t size,
	          qsort_r_compar_f cmp, void *arg) {
  __ptsan_qsort_r(pbase, total_elems, size, cmp, arg);
}


// longjmp and friends

    /* longjmp */
INTERCEPTOR(void, longjmp, jmp_buf env, int val) {
  ENSURE_PTSAN_INITED();
  REAL(longjmp)(clear_id(env), val);
}

INTERCEPTOR(void, _longjmp, jmp_buf env, int val) {
  ENSURE_PTSAN_INITED();
  REAL(_longjmp)(clear_id(env), val);
}

/* siglongjmp */
INTERCEPTOR(void, siglongjmp, jmp_buf env, int val) {
  ENSURE_PTSAN_INITED();
  REAL(siglongjmp)(clear_id(env), val);
}

// Cat funcs

INTERCEPTOR(nl_catd, catopen, const char *name, int oflag) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  nl_catd res = REAL(catopen)(clear_id(name), oflag);
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(char *, catgets, nl_catd catd, int set_id, int msg_id, const char *s) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  char *res = REAL(catgets)(catd, set_id, msg_id, clear_id(s));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, catclose, nl_catd catd) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(catclose)(catd);
  ptsan_in_libc--;
  return res;
}

/*-------------------------------- strcoll -------------------------------*/
INTERCEPTOR(int, strcoll, const char *s1, const char *s2) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(strcoll)(clear_id(s1), clear_id(s2));
  ptsan_in_libc--;
  return res;
}

/*------------------------------ strcoll_l -------------------------------*/
INTERCEPTOR(int, strcoll_l, const char *s1, const char *s2, locale_t loc) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(strcoll_l)(clear_id(s1), clear_id(s2), loc);
  ptsan_in_libc--;
  return res;
}

/*------------------------------ wcscoll ---------------------------------*/
INTERCEPTOR(int, wcscoll, const wchar_t *s1, const wchar_t *s2) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(wcscoll)(clear_id(s1), clear_id(s2));
  ptsan_in_libc--;
  return res;
}

// tmpfile stuff

/*---------------------------  tmpfile  ----------------------------*/
INTERCEPTOR(FILE *, tmpfile) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  FILE *res = REAL(tmpfile)();
  ptsan_in_libc--;
  /* TODO: tag res if necessary */
  return res;
}

/* Some libcs provide the large-file alias tmpfile64. */
INTERCEPTOR(FILE *, tmpfile64) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  FILE *res = REAL(tmpfile64)();
  ptsan_in_libc--;
  return res;
}



/* sigaction */
INTERCEPTOR(int, sigaction,
            int signum,
            const struct sigaction *act,
            struct sigaction *oldact) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(sigaction)(signum,
                            clear_id(act),
                            clear_id(oldact));
  ptsan_in_libc--;
  return res;
}


/* signal */
INTERCEPTOR(sighandler_t, signal,
            int signum,
            sighandler_t handler) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  sighandler_t res = REAL(signal)(signum,
                                  clear_id(handler));
  ptsan_in_libc--;
  return res;
}


INTERCEPTOR(int, pthread_key_create, pthread_key_t *key, void (*destructor)(void *)) {
  if (UNLIKELY(ptsan_init_is_running)) {
    CHECK(get_id(key) == 0);
    return REAL(pthread_key_create)(key, destructor);
  }

  ENSURE_PTSAN_INITED();

  ptsan_in_libc++;
  int res = REAL(pthread_key_create)(clear_id(key), destructor);
  ptsan_in_libc--;
  return res;
}
INTERCEPTOR(int, pthread_setschedparam, pthread_t thread, int policy, const struct sched_param *param) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_setschedparam)(thread, policy, clear_id(param));
  ptsan_in_libc--;
  return res;
}
INTERCEPTOR(int, pthread_attr_setschedparam, pthread_attr_t *attr, const struct sched_param *param) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pthread_attr_setschedparam)(
    clear_id(attr),
    clear_id(param)
  );
  ptsan_in_libc--;
  return res;
}

struct pthread_unwind_buf
{
  struct
  {
    __jmp_buf jmp_buf;
    int mask_was_saved;
  } cancel_jmp_buf[1];

  union
  {
    /* This is the placeholder of the public version.  */
    void *pad[4];

    struct
    {
      /* Pointer to the previous cleanup buffer.  */
      struct pthread_unwind_buf *prev;

      /* Backward compatibility: state of the old-style cleanup
	 handler at the time of the previous new-style cleanup handler
	 installment.  */
      struct _pthread_cleanup_buffer *cleanup;

      /* Cancellation type before the push call.  */
      int canceltype;
    } data;
  } priv;
};

INTERCEPTOR(void, __pthread_register_cancel, __pthread_unwind_buf_t *__buf) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  __pthread_unwind_buf_t *ibuf = clear_id(__buf);
  struct pthread_unwind_buf *clear_buf = (struct pthread_unwind_buf *) ibuf;
  clear_buf->priv.data.prev = clear_id(clear_buf->priv.data.prev);
  clear_buf->priv.data.cleanup = clear_id(clear_buf->priv.data.cleanup);
  // unsigned int count = 1;
  // while(ibuf) {
  //   printf ("lahtinen %u %p\n", count, ibuf);
  //   struct pthread_unwind_buf *clear_buf = (struct pthread_unwind_buf *) ibuf;
  //   if(clear_buf->priv.data.prev)
  //     clear_buf->priv.data.prev = clear_id(clear_buf->priv.data.prev);
  //   if(clear_buf->priv.data.cleanup)
  //     clear_buf->priv.data.cleanup = clear_id(clear_buf->priv.data.cleanup);
  //   ibuf = clear_buf->priv.data.prev;
  //   ++count;
  // }
  REAL(__pthread_register_cancel)(ibuf);
  ptsan_in_libc--;
}

INTERCEPTOR(void, ___pthread_unregister_cancel, __pthread_unwind_buf_t *buf) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  __pthread_unwind_buf_t *ibuf = clear_id(buf);
  struct pthread_unwind_buf *clear_buf = (struct pthread_unwind_buf *) ibuf;
  clear_buf->priv.data.prev = clear_id(clear_buf->priv.data.prev);
  clear_buf->priv.data.cleanup = clear_id(clear_buf->priv.data.cleanup);
  // while(ibuf) {
  //   struct pthread_unwind_buf *clear_buf = (struct pthread_unwind_buf *) ibuf;
  //   if(clear_buf->priv.data.prev)
  //     clear_buf->priv.data.prev = clear_id(clear_buf->priv.data.prev);
  //   if(clear_buf->priv.data.cleanup)
  //     clear_buf->priv.data.cleanup = clear_id(clear_buf->priv.data.cleanup);
  //   ibuf = clear_id(clear_buf->priv.data.prev);
  // }

  REAL(___pthread_unregister_cancel)(ibuf);
  ptsan_in_libc--;
}

INTERCEPTOR(void, __pthread_unregister_cancel, __pthread_unwind_buf_t *buf) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  __pthread_unwind_buf_t *ibuf = clear_id(buf);
  // struct pthread_unwind_buf *clear_buf = (struct pthread_unwind_buf *) ibuf;
  // clear_buf->priv.data.prev = clear_id(clear_buf->priv.data.prev);
  // clear_buf->priv.data.cleanup = clear_id(clear_buf->priv.data.cleanup);
  // printf("lahtinen 2__pthread_unregister_cancel:\n");
  // while(ibuf) {
  //   struct pthread_unwind_buf *clear_buf = (struct pthread_unwind_buf *) ibuf;
  //   if(clear_buf->priv.data.prev)
  //     clear_buf->priv.data.prev = clear_id(clear_buf->priv.data.prev);
  //   if(clear_buf->priv.data.cleanup)
  //     clear_buf->priv.data.cleanup = clear_id(clear_buf->priv.data.cleanup);
  //   ibuf = clear_id(clear_buf->priv.data.prev);
  // }

  REAL(__pthread_unregister_cancel)(ibuf);
  ptsan_in_libc--;
}

#include <time.h>

INTERCEPTOR(int, clock_nanosleep, clockid_t clock_id, int flags,
            const struct timespec *request, struct timespec *remain) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(clock_nanosleep)(
    clock_id,
    flags,
    clear_id(request),
    clear_id(remain)
  );

  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, setsockopt, int sockfd, int level, int optname,
            const void *optval, socklen_t optlen) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(setsockopt)(
    sockfd,
    level,
    optname,
    clear_id(optval),
    optlen
  );

  ptsan_in_libc--;
  return res;
}


INTERCEPTOR(int, timer_settime, timer_t timerid, int flags,
            const struct itimerspec *new_value, struct itimerspec *old_value) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(timer_settime)(
    timerid,
    flags,
    clear_id(new_value),
    clear_id(old_value)
  );

  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, select, int nfds,
            fd_set *readfds,
            fd_set *writefds,
            fd_set *exceptfds,
            struct timeval *timeout) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(select)(
    nfds,
    clear_id(readfds),
    clear_id(writefds),
    clear_id(exceptfds),
    clear_id(timeout)
  );

  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, sched_setaffinity, pid_t pid, size_t cpusetsize,
            const cpu_set_t *mask) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(sched_setaffinity)(
    pid,
    cpusetsize,
    clear_id(mask)
  );

  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_setaffinity_np, pthread_t thread, size_t cpusetsize,
            const cpu_set_t *cpuset) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(pthread_setaffinity_np)(
    thread,
    cpusetsize,
    clear_id(cpuset)
  );

  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, futex, int *uaddr, int futex_op, int val,
            const struct timespec *timeout, int *uaddr2, int val3) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(futex)(
    clear_id(uaddr),
    futex_op,
    val,
    clear_id(timeout),
    clear_id(uaddr2),
    val3
  );

  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_join, pthread_t thread, void **retval) {
    ENSURE_PTSAN_INITED();
    ptsan_in_libc++;
    int res = REAL(pthread_join)(thread, clear_id(retval));
    ptsan_in_libc--;
    return res;
}

INTERCEPTOR(int, ___pthread_join, pthread_t thread, void **retval) {
    ENSURE_PTSAN_INITED();
    ptsan_in_libc++;
    int res = REAL(___pthread_join)(thread, clear_id(retval));
    ptsan_in_libc--;
    return res;
}

INTERCEPTOR(int, __pthread_clockjoin_ex,
            pthread_t thread, void **retval,
            const struct timespec *abstime,
            clockid_t clockid) {
    ENSURE_PTSAN_INITED();
    ptsan_in_libc++;
    int res = REAL(__pthread_clockjoin_ex)(thread, clear_id(retval), clear_id(abstime), clockid);
    ptsan_in_libc--;

    return res;
}

INTERCEPTOR(void, sigcancel_handler, int sig, siginfo_t *si, void *uc) {
    ENSURE_PTSAN_INITED();
    ptsan_in_libc++;
    REAL(sigcancel_handler)(sig, clear_id(si), clear_id(uc));
    ptsan_in_libc--;
}

INTERCEPTOR(int, pthread_barrierattr_setpshared, pthread_barrierattr_t *attr, int pshared) {
    ENSURE_PTSAN_INITED();
    ptsan_in_libc++;
    int res = REAL(pthread_barrierattr_setpshared)(clear_id(attr), pshared);
    ptsan_in_libc--;
    return res;
}

/*------------- rename ------------------------------------------------*/
INTERCEPTOR(int, rename, const char *oldp, const char *newp) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(rename)(clear_id(oldp), clear_id(newp));
  ptsan_in_libc--;
  return res;
}

/*------------- renameat ----------------------------------------------*/
INTERCEPTOR(int, renameat,
            int olddirfd, const char *oldp,
            int newdirfd, const char *newp) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(renameat)(olddirfd,
                           clear_id(oldp),
                           newdirfd,
                           clear_id(newp));
  ptsan_in_libc--;
  return res;
}

/*------------- renameat2  (GNU/Linux extension) ----------------------*/
INTERCEPTOR(int, renameat2,
            int olddirfd, const char *oldp,
            int newdirfd, const char *newp,
            unsigned int flags) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(renameat2)(olddirfd,
                            clear_id(oldp),
                            newdirfd,
                            clear_id(newp),
                            flags);
  ptsan_in_libc--;
  return res;
}

/*------------- remove ------------------------------------------------*/
INTERCEPTOR(int, remove, const char *path) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(remove)(clear_id(path));
  ptsan_in_libc--;
  return res;
}


/*=================== wmemchr =========================================*/
INTERCEPTOR(wchar_t *, wmemchr,
            const wchar_t *s, wchar_t c, size_t n) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  wchar_t *res = REAL(wmemchr)(clear_id(s), c, n);
  ptsan_in_libc--;
  return copy_id(res, s);
}

/*=================== wmemcmp =========================================*/
INTERCEPTOR(int, wmemcmp,
            const wchar_t *s1, const wchar_t *s2, size_t n) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(wmemcmp)(clear_id(s1), clear_id(s2), n);
  ptsan_in_libc--;
  return res;
}

/*---------------------------  chdir  ---------------------------*/
INTERCEPTOR(int, chdir, const char *path) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(chdir)(clear_id(path));
  ptsan_in_libc--;
  return res;
}

/*---------------------------  fchdir ---------------------------*/
INTERCEPTOR(int, fchdir, int fd) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(fchdir)(fd);
  ptsan_in_libc--;
  return res;
}

/*------------------------------ pipe --------------------------------*/
INTERCEPTOR(int, pipe, int pipefd[2]) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pipe)(clear_id(pipefd));
  ptsan_in_libc--;
  return res;
}

/*------------------------------ pipe2 -------------------------------*/
INTERCEPTOR(int, pipe2, int pipefd[2], int flags) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(pipe2)(clear_id(pipefd), flags);
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, set_robust_list,
            struct robust_list_head *head, size_t len)
{
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(set_robust_list)(clear_id(head), len);
  ptsan_in_libc--;
  return res;
}

/* ----------------------------env---------------------- */

extern "C" {
  extern char *__ptsan_getenv(const char *name);
  extern int __ptsan_setenv(const char *name, const char *value, int overwrite);
  extern int __ptsan_putenv(char *string);
  extern int __ptsan_unsetenv(const char *name);
  extern int __ptsan_clearenv();
}

INTERCEPTOR(char *, getenv, const char *name) {
  return __ptsan_getenv(name);
}

INTERCEPTOR(int, setenv, const char *name, const char *value, int overwrite) {
  return __ptsan_setenv(name, value, overwrite);
}

INTERCEPTOR(int, putenv, char *string) {
  return __ptsan_putenv(string);
}

INTERCEPTOR(int, unsetenv, const char *name) {
  return __ptsan_unsetenv(name);
}

INTERCEPTOR(int, clearenv) {
  return __ptsan_clearenv();
}

extern char **environ;

/* ---------------------------- execve -------------------------- */
extern "C" {
  extern int __ptsan_execvpe(const char *file, char *const argv[], char *const envp[]);
}

INTERCEPTOR(int, execve,
            const char *pathname,
            char *const argv[],
            char *const envp[]) {
  ptsan_in_libc++;

  const char *clean_path = clear_id(pathname);

  size_t argc = ptsan_strv_len(argv);
  char **clean_argv = (char **)__builtin_alloca((argc + 1) * sizeof(char *));
  ptsan_strv_deep_untag_into(argv, clean_argv, argc);

  size_t envc = ptsan_strv_len(envp);
  char **clean_envp = (char **)__builtin_alloca((envc + 1) * sizeof(char *));
  ptsan_strv_deep_untag_into(envp, clean_envp, envc);

  int res = REAL(execve)(clean_path, clean_argv, clean_envp);

  ptsan_in_libc--;
  return res;
}


INTERCEPTOR(int, fexecve, int fd, char *const argv[], char *const envp[]) {
  ptsan_in_libc++;

  size_t argc = ptsan_strv_len(argv);
  char **clean_argv = (char **)__builtin_alloca((argc + 1) * sizeof(*clean_argv));
  ptsan_strv_deep_untag_into(argv, clean_argv, argc);

  size_t envc = ptsan_strv_len(envp);
  char **clean_envp = (char **)__builtin_alloca((envc + 1) * sizeof(*clean_envp));
  ptsan_strv_deep_untag_into(envp, clean_envp, envc);

  int res = REAL(fexecve)(fd, clean_argv, clean_envp);

  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, execv, const char *path, char *const argv[]) {
  ptsan_in_libc++;

  const char *clean_path = clear_id(path);

  size_t argc = ptsan_strv_len(argv);
  char **clean_argv = (char **)__builtin_alloca((argc + 1) * sizeof(*clean_argv));
  ptsan_strv_deep_untag_into(argv, clean_argv, argc);

  size_t envc = ptsan_strv_len(environ);
  char **clean_envp = (char **)__builtin_alloca((envc + 1) * sizeof(char *));
  ptsan_strv_deep_untag_into(environ, clean_envp, envc);

  int res = REAL(execve)(clean_path, clean_argv, clean_envp);

  ptsan_in_libc--;
  return res;
}

/* GNU extension */
INTERCEPTOR(int, execvpe, const char *file, char *const argv[], char *const envp[]) {
  ptsan_in_libc++;

  const char *clean_file = clear_id(file);

  size_t argc = ptsan_strv_len(argv);
  char **clean_argv = (char **)__builtin_alloca((argc + 1) * sizeof(*clean_argv));
  ptsan_strv_deep_untag_into(argv, clean_argv, argc);

  size_t envc = ptsan_strv_len(envp);
  char **clean_envp = (char **)__builtin_alloca((envc + 1) * sizeof(*clean_envp));
  ptsan_strv_deep_untag_into(envp, clean_envp, envc);
  ptsan_in_libc--;

  // Still have to clean envp (and maybe argv?), since it's passed into a syscall
  int res = __ptsan_execvpe(clean_file, clean_argv, clean_envp);

  return res;
}


INTERCEPTOR(int, execvp, const char *file, char *const argv[]) {
  ptsan_in_libc++;

  const char *clean_file = clear_id(file);

  size_t argc = ptsan_strv_len(argv);
  char **clean_argv = (char **)__builtin_alloca((argc + 1) * sizeof(*clean_argv));
  ptsan_strv_deep_untag_into(argv, clean_argv, argc);

  size_t envc = ptsan_strv_len(environ);
  char **clean_envp = (char **)__builtin_alloca((envc + 1) * sizeof(*clean_envp));
  ptsan_strv_deep_untag_into(environ, clean_envp, envc);
  ptsan_in_libc--;

  // Still have to clean envp (and maybe argv?), since it's passed into a syscall
  int res = __ptsan_execvpe(clean_file, clean_argv, clean_envp);

  return res;
}


INTERCEPTOR(int, execl, const char *path, const char *arg0, ...) {
  ptsan_in_libc++;

  va_list ap_count;
  va_start(ap_count, arg0);
  size_t extra = ptsan_count_varargs_until_null(ap_count);
  va_end(ap_count);

  size_t argc = 1 + extra;
  char **argv = (char **)__builtin_alloca((argc + 1) * sizeof(*argv));

  va_list ap_build;
  va_start(ap_build, arg0);
  ptsan_build_argv_from_varargs_into(arg0, ap_build, argv, argc);
  /* next va_arg would be NULL sentinel; execl has no envp here */
  va_end(ap_build);

  const char *clean_path = clear_id(path);

  size_t envc = ptsan_strv_len(environ);
  char **clean_envp = (char **)__builtin_alloca((envc + 1) * sizeof(*clean_envp));
  ptsan_strv_deep_untag_into(environ, clean_envp, envc);

  int res = REAL(execve)(clean_path, argv, clean_envp);

  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, execlp, const char *file, const char *arg0, ...) {
  ptsan_in_libc++;

  va_list ap_count;
  va_start(ap_count, arg0);
  size_t extra = ptsan_count_varargs_until_null(ap_count);
  va_end(ap_count);

  size_t argc = 1 + extra;
  char **argv = (char **)__builtin_alloca((argc + 1) * sizeof(*argv));

  va_list ap_build;
  va_start(ap_build, arg0);
  ptsan_build_argv_from_varargs_into(arg0, ap_build, argv, argc);
  va_end(ap_build);

  const char *clean_file = clear_id(file);

  size_t envc = ptsan_strv_len(environ);
  char **clean_envp = (char **)__builtin_alloca((envc + 1) * sizeof(*clean_envp));
  ptsan_strv_deep_untag_into(environ, clean_envp, envc);

  ptsan_in_libc--;

  /* Use execvpe so we can pass envp explicitly */
  int res = __ptsan_execvpe(clean_file, argv, clean_envp);
  return res;
}

// Execle's signature is: `path, args..., (void*)0, envp`. Why they
// did this is beyond me.
INTERCEPTOR(int, execle, const char *path, const char *arg0, ...) {
  ptsan_in_libc++;

  /* Count args to size argv. */
  va_list ap_count;
  va_start(ap_count, arg0);
  size_t extra = ptsan_count_varargs_until_null(ap_count);
  va_end(ap_count);

  size_t argc = extra + 1;
  char **argv = (char **)__builtin_alloca((argc + 1) * sizeof(*argv));

  /* Build argv, then fetch envp (follows the terminating NULL). */
  va_list ap_build;
  va_start(ap_build, arg0);
  ptsan_build_argv_from_varargs_into(arg0, ap_build, argv, argc);
  // We have now consumed up to (but not including) the null between
  // argv and envp;
  va_arg(ap_build, void*);
  // Now we've got envp properly
  char *const *envp = va_arg(ap_build, char *const *);
  va_end(ap_build);

  const char *clean_path = clear_id(path);

  size_t envc = ptsan_strv_len(envp);
  char **clean_envp = (char **)__builtin_alloca((envc + 1) * sizeof(*clean_envp));
  ptsan_strv_deep_untag_into(envp, clean_envp, envc);

  int res = REAL(execve)(clean_path, argv, clean_envp);

  ptsan_in_libc--;
  return res;
}

/* -------------------------- clone ------------------------------
   Generic wrapper that copes with the GNU/Musl prototype

      int clone(int (*fn)(void *), void *child_stack,
                int flags, void *arg, ...);

   The var-arg tail may contain:
      pid_t  *ptid,
      void   *newtls,
      pid_t  *ctid
   depending on the flag bits.  We forward up to three extra
   arguments after stripping tags when they are present.
   -------------------------------------------------------------- */
INTERCEPTOR(int, clone,
            int (*fn)(void *),
            void *child_stack,
            int flags,
            void *arg, ...)
{
  /* Extract optional parameters */
  void *ptid   = NULL;
  void *newtls = NULL;
  void *ctid   = NULL;

  if (flags & (CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | CLONE_SETTLS |
               CLONE_CHILD_CLEARTID))
  {
      va_list ap;
      va_start(ap, arg);

      if (flags & CLONE_PARENT_SETTID)
          ptid = va_arg(ap, void *);

      if (flags & CLONE_SETTLS)
          newtls = va_arg(ap, void *);

      if (flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID))
          ctid = va_arg(ap, void *);

      va_end(ap);
  }

  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(clone)(clear_id(fn),
                        clear_id(child_stack),
                        flags,
                        clear_id(arg),
                        clear_id(ptid),
                        clear_id(newtls),
                        clear_id(ctid));

  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, __cxa_atexit,
            void (*func)(void *), void *arg, void *dso_handle) {
  if(UNLIKELY(ptsan_init_is_running)) {
    return REAL(__cxa_atexit)(func, arg, dso_handle);
  }

  ptsan_in_libc++;
  int res = REAL(__cxa_atexit)(clear_id(func), clear_id(arg), clear_id(dso_handle));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, epoll_ctl, int epfd, int op, int fd,
            struct epoll_event *_Nullable event) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(epoll_ctl)(epfd,
                            op,
                            fd,
                            clear_id(event));

  /* execve() does not return on success */
  ptsan_in_libc--;
  return res;
}\

INTERCEPTOR(char *, strtok_r, char *str, const char *delim, char **saveptr) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  char *res = REAL(strtok_r)(
    (char *)clear_id(str),
    (const char *)clear_id((void *)delim),
    (char **)clear_id((void *)saveptr)
  );

  copy_id(*saveptr, str);
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_spin_init, pthread_spinlock_t *lock, int pshared) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(pthread_spin_init)(
    (pthread_spinlock_t *)clear_id((void *)lock),
    pshared
  );
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_spin_destroy, pthread_spinlock_t *lock) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(pthread_spin_destroy)(
    (pthread_spinlock_t *)clear_id((void *)lock)
  );

  ptsan_in_libc--;
  return res;
}


INTERCEPTOR(int, pthread_spin_trylock, pthread_spinlock_t *lock) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(pthread_spin_trylock)(
    (pthread_spinlock_t *)clear_id((void *)lock)
  );

  ptsan_in_libc--;
  return res;
}


INTERCEPTOR(int, pthread_spin_lock, pthread_spinlock_t *lock) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(pthread_spin_lock)(
    (pthread_spinlock_t *)clear_id((void *)lock)
  );

  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, pthread_spin_unlock, pthread_spinlock_t *lock) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(pthread_spin_unlock)(
    (pthread_spinlock_t *)clear_id((void *)lock)
  );

  ptsan_in_libc--;
  return res;
}


INTERCEPTOR(int, strcasecmp_l, const char *s1, const char *s2, locale_t loc) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(strcasecmp_l)(
    (const char *)clear_id((void *)s1),
    (const char *)clear_id((void *)s2),
    (locale_t)clear_id((void *)loc)
  );

  ptsan_in_libc--;
  return res;
}


INTERCEPTOR(int, strncasecmp_l, const char *s1, const char *s2, size_t n, locale_t loc) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  int res = REAL(strncasecmp_l)(
    (const char *)clear_id((void *)s1),
    (const char *)clear_id((void *)s2),
    n,
    (locale_t)clear_id((void *)loc)
  );

  ptsan_in_libc--;
  return res;
}


INTERCEPTOR(size_t, strlcpy, char *dst, const char *src, size_t size) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  size_t res = REAL(strlcpy)(
    (char *)clear_id((void *)dst),
    (const char *)clear_id((void *)src),
    size
  );

  ptsan_in_libc--;
  return res;
}


INTERCEPTOR(size_t, strlcat, char *dst, const char *src, size_t size) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  size_t res = REAL(strlcat)(
    (char *)clear_id((void *)dst),
    (const char *)clear_id((void *)src),
    size
  );

  ptsan_in_libc--;
  return res;
}


// Wide-string length & friends
INTERCEPTOR(wchar_t *, wcschr, const wchar_t *s, wchar_t c) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  wchar_t *res = REAL(wcschr)(clear_id(s), c);
  ptsan_in_libc--;
  return copy_id(res, s);
}

INTERCEPTOR(wchar_t *, wcsrchr, const wchar_t *s, wchar_t c) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  wchar_t *res = REAL(wcsrchr)(clear_id(s), c);
  ptsan_in_libc--;
  return copy_id(res, s);
}

INTERCEPTOR(wchar_t *, wcsstr, const wchar_t *haystack, const wchar_t *needle) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  wchar_t *res = REAL(wcsstr)(clear_id(haystack), clear_id(needle));
  ptsan_in_libc--;
  return copy_id(res, haystack);
}

INTERCEPTOR(wchar_t *, wcspbrk, const wchar_t *s, const wchar_t *accept) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  wchar_t *res = REAL(wcspbrk)(clear_id(s), clear_id(accept));
  ptsan_in_libc--;
  return copy_id(res, s);
}

// Spans

INTERCEPTOR(size_t, wcsspn, const wchar_t *s, const wchar_t *accept) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  size_t res = REAL(wcsspn)(clear_id(s), clear_id(accept));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(size_t, wcscspn, const wchar_t *s, const wchar_t *reject) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  size_t res = REAL(wcscspn)(clear_id(s), clear_id(reject));
  ptsan_in_libc--;
  return res;
}

// Compare


INTERCEPTOR(int, wcsncmp, const wchar_t *s1, const wchar_t *s2, size_t n) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(wcsncmp)(clear_id(s1), clear_id(s2), n);
  ptsan_in_libc--;
  return res;
}

// Copy / dup

INTERCEPTOR(wchar_t *, wcscpy, wchar_t *dst, const wchar_t *src) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  wchar_t *res = REAL(wcscpy)(clear_id(dst), clear_id(src));
  ptsan_in_libc--;
  return copy_id(res, dst);
}

INTERCEPTOR(wchar_t *, wcsncpy, wchar_t *dst, const wchar_t *src, size_t n) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  wchar_t *res = REAL(wcsncpy)(clear_id(dst), clear_id(src), n);
  ptsan_in_libc--;
  return copy_id(res, dst);
}

// Directory stuff

/* rewinddir */
INTERCEPTOR(void, rewinddir, DIR *dirp) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  REAL(rewinddir)(clear_id(dirp));
  ptsan_in_libc--;
}

/* seekdir */
INTERCEPTOR(void, seekdir, DIR *dirp, long loc) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  REAL(seekdir)(clear_id(dirp), loc);
  ptsan_in_libc--;
}

/* telldir */
INTERCEPTOR(long, telldir, DIR *dirp) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  long res = REAL(telldir)(clear_id(dirp));
  ptsan_in_libc--;
  return res;
}

/* dirfd */
INTERCEPTOR(int, dirfd, DIR *dirp) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(dirfd)(clear_id(dirp));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, scandirat,
            int dirfd,
            const char *dirp,
            struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **)) {
  ENSURE_PTSAN_INITED();

  ptsan_in_libc++;
  int res = REAL(scandirat)(dirfd, clear_id(dirp), clear_id(namelist), filter, compar);
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, scandirat64,
            int dirfd,
            const char *dirp,
            struct dirent64 ***namelist,
            int (*filter)(const struct dirent64 *),
            int (*compar)(const struct dirent64 **, const struct dirent64 **)) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(scandirat64)(dirfd, clear_id(dirp), clear_id(namelist), filter, compar);
  ptsan_in_libc--;
  return res;
}

#include <spawn.h>   // posix_spawn_file_actions_t and functions

// TODO(epl): not sure why I need to declare these here
enum spawn_action_tag
  {
    spawn_do_close,
    spawn_do_dup2,
    spawn_do_open,
    spawn_do_chdir,
    spawn_do_fchdir,
  };

struct __spawn_action
{
  spawn_action_tag tag;

  union
  {
    struct
    {
      int fd;
    } close_action;
    struct
    {
      int fd;
      int newfd;
    } dup2_action;
    struct
    {
      int fd;
      char *path;
      int oflag;
      mode_t mode;
    } open_action;
    struct
    {
      char *path;
    } chdir_action;
    struct
    {
      int fd;
    } fchdir_action;
  } action;
};


/* dup2 */
INTERCEPTOR(int, posix_spawn_file_actions_adddup2,
            posix_spawn_file_actions_t *fa, int fd, int newfd) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  posix_spawn_file_actions_t *clear_fa = clear_id(fa);
  clear_fa->__actions = clear_id(clear_fa->__actions);
  int res = REAL(posix_spawn_file_actions_adddup2)(clear_fa, fd, newfd);
  ptsan_in_libc--;
  return res;
}

/* open */
INTERCEPTOR(int, posix_spawn_file_actions_addopen,
            posix_spawn_file_actions_t *fa, int fd,
            const char *path, int oflag, mode_t mode) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  posix_spawn_file_actions_t *clear_fa = clear_id(fa);
  clear_fa->__actions = clear_id(clear_fa->__actions);
  // clear_fa->__actions = clear_id(&clear_fa->__actions[clear_fa->__used]);
  int res = REAL(posix_spawn_file_actions_addopen)(clear_fa,
                                                   fd,
                                                   clear_id(path),
                                                   oflag,
                                                   mode);
  ptsan_in_libc--;
  return res;
}

/* close */
INTERCEPTOR(int, posix_spawn_file_actions_addclose,
            posix_spawn_file_actions_t *fa, int fd) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  posix_spawn_file_actions_t *clear_fa = clear_id(fa);
  clear_fa->__actions = clear_id(clear_fa->__actions);
  // clear_fa->__actions = clear_id(&clear_fa->__actions[clear_fa->__used]);

  int res = REAL(posix_spawn_file_actions_addclose)(clear_fa, fd);
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, posix_spawn_file_actions_init, posix_spawn_file_actions_t *fa) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  posix_spawn_file_actions_t *clear_fa = clear_id(fa);
  clear_fa->__actions = clear_id(clear_fa->__actions);

  int res = REAL(posix_spawn_file_actions_init)(clear_fa);
  ptsan_in_libc--;
  return res;
}

/* closefrom */
INTERCEPTOR(int, posix_spawn_file_actions_addclosefrom_np,
            posix_spawn_file_actions_t *fa, int lowfd) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  posix_spawn_file_actions_t *clear_fa = clear_id(fa);
  clear_fa->__actions = clear_id(clear_fa->__actions);
  int res = REAL(posix_spawn_file_actions_addclosefrom_np)(clear_fa, lowfd);
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, posix_spawn_file_actions_destroy, posix_spawn_file_actions_t *fa) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  posix_spawn_file_actions_t *clear_fa = clear_id(fa);
  clear_fa->__actions = clear_id(clear_fa->__actions);
  for (int i = 0; i < clear_fa->__used; ++i)
    {
      struct __spawn_action *sa = &clear_fa->__actions[i];
      switch (sa->tag)
        {
        case spawn_do_open:
          sa->action.open_action.path = clear_id(sa->action.open_action.path);
          break;
        case spawn_do_chdir:
          sa->action.chdir_action.path = clear_id(sa->action.chdir_action.path);
          break;
        case spawn_do_close:
        case spawn_do_dup2:
        case spawn_do_fchdir:
          /* No cleanup required.  */
          break;
        }
    }

  int res = REAL(posix_spawn_file_actions_destroy)(clear_fa);
  ptsan_in_libc--;
  return res;
}

/* ---------- spawnattr ---------- */

/* init */
INTERCEPTOR(int, posix_spawnattr_init,
            posix_spawnattr_t *attr) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(posix_spawnattr_init)(clear_id(attr));
  ptsan_in_libc--;
  return res;
}

/* destroy */
INTERCEPTOR(int, posix_spawnattr_destroy,
            posix_spawnattr_t *attr) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(posix_spawnattr_destroy)(clear_id(attr));
  ptsan_in_libc--;
  return res;
}

/* setflags / getflags */
INTERCEPTOR(int, posix_spawnattr_setflags,
            posix_spawnattr_t *attr, short flags) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(posix_spawnattr_setflags)(clear_id(attr), flags);
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, posix_spawnattr_getflags,
            const posix_spawnattr_t *attr, short *flags) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(posix_spawnattr_getflags)(clear_id(attr), clear_id(flags));
  ptsan_in_libc--;
  return res;
}

/* setsigmask / getsigmask */
INTERCEPTOR(int, posix_spawnattr_setsigmask,
            posix_spawnattr_t *attr, const sigset_t *sigmask) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(posix_spawnattr_setsigmask)(clear_id(attr), clear_id(sigmask));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, posix_spawnattr_getsigmask,
            const posix_spawnattr_t *attr, sigset_t *sigmask) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(posix_spawnattr_getsigmask)(clear_id(attr), clear_id(sigmask));
  ptsan_in_libc--;
  return res;
}

/* setsigdefault / getsigdefault */
INTERCEPTOR(int, posix_spawnattr_setsigdefault,
            posix_spawnattr_t *attr, const sigset_t *sigdefault) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(posix_spawnattr_setsigdefault)(clear_id(attr), clear_id(sigdefault));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, posix_spawnattr_getsigdefault,
            const posix_spawnattr_t *attr, sigset_t *sigdefault) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(posix_spawnattr_getsigdefault)(clear_id(attr), clear_id(sigdefault));
  ptsan_in_libc--;
  return res;
}

/* setpgroup / getpgroup */
INTERCEPTOR(int, posix_spawnattr_setpgroup,
            posix_spawnattr_t *attr, pid_t pgroup) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(posix_spawnattr_setpgroup)(clear_id(attr), pgroup);
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, posix_spawnattr_getpgroup,
            const posix_spawnattr_t *attr, pid_t *pgroup) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(posix_spawnattr_getpgroup)(clear_id(attr), clear_id(pgroup));
  ptsan_in_libc--;
  return res;
}

/* setschedpolicy / getschedpolicy */
INTERCEPTOR(int, posix_spawnattr_setschedpolicy,
            posix_spawnattr_t *attr, int policy) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(posix_spawnattr_setschedpolicy)(clear_id(attr), policy);
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, posix_spawnattr_getschedpolicy,
            const posix_spawnattr_t *attr, int *policy) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(posix_spawnattr_getschedpolicy)(clear_id(attr), clear_id(policy));
  ptsan_in_libc--;
  return res;
}

/* setschedparam / getschedparam */
INTERCEPTOR(int, posix_spawnattr_setschedparam,
            posix_spawnattr_t *attr, const struct sched_param *param) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(posix_spawnattr_setschedparam)(clear_id(attr), clear_id(param));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(int, posix_spawnattr_getschedparam,
            const posix_spawnattr_t *attr, struct sched_param *param) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(posix_spawnattr_getschedparam)(clear_id(attr), clear_id(param));
  ptsan_in_libc--;
  return res;
}

#include <spawn.h>

/* posix_spawn */
INTERCEPTOR(int, posix_spawn,
            pid_t *pid, const char *path,
            const posix_spawn_file_actions_t *file_actions,
            const posix_spawnattr_t *attrp,
            char *const argv[], char *const envp[])
{
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  pid_t *clear_pid = (pid_t *)clear_id(pid);
  const char *clear_path = (const char *)clear_id(path);
  posix_spawn_file_actions_t *clear_fa =
      (posix_spawn_file_actions_t *)clear_id((void *)file_actions);
  posix_spawnattr_t *clear_attr =
      (posix_spawnattr_t *)clear_id((void *)attrp);

  char **clear_argv = (char **)clear_id(argv);
  if (clear_argv) {
    for (unsigned int i = 0; clear_argv[i]; ++i) {
      clear_argv[i] = (char *)clear_id(clear_argv[i]);
    }
  }

  char **clear_envp = (char **)clear_id(envp);
  if (clear_envp) {
    for (unsigned int i = 0; clear_envp[i]; ++i) {
      clear_envp[i] = (char *)clear_id(clear_envp[i]);
    }
  }

  int res = REAL(posix_spawn)(clear_pid, clear_path,
                              clear_fa, clear_attr,
                              clear_argv, clear_envp);
  ptsan_in_libc--;
  return res;
}


#include <ffi.h>

/* ffi_prep_cif */
INTERCEPTOR(ffi_status, ffi_prep_cif,
            ffi_cif *cif, ffi_abi abi,
            unsigned int nargs, ffi_type *rtype,
            ffi_type **atypes) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  ffi_cif *clear_cif   = (ffi_cif *)clear_id(cif);
  ffi_type *clear_rtype = (ffi_type *)clear_id(rtype);
  ffi_type **clear_atypes = (ffi_type **)clear_id(atypes);

  if (clear_atypes) {
    for (unsigned int i = 0; i < nargs; ++i) {
      clear_atypes[i] = (ffi_type *)clear_id(clear_atypes[i]);
    }
  }

  ffi_status res = REAL(ffi_prep_cif)(clear_cif, abi,
                                      nargs, clear_rtype, clear_atypes);
  ptsan_in_libc--;
  return res;
}

/* ffi_call */
INTERCEPTOR(void, ffi_call,
            ffi_cif *cif,
            void (*fn)(void),
            void *rvalue,
            void **avalue) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  ffi_cif *clear_cif = (ffi_cif *)clear_id(cif);
  void (*clear_fn)(void) = (void (*)(void))clear_id((void *)fn);
  void *clear_rvalue = (void *)clear_id(rvalue);
  void **clear_avalue = (void **)clear_id(avalue);

  if (clear_avalue && clear_cif) {
    for (unsigned int i = 0; i < clear_cif->nargs; ++i) {
      clear_avalue[i] = clear_id(clear_avalue[i]);
    }
  }

  REAL(ffi_call)(clear_cif, clear_fn, clear_rvalue, clear_avalue);

  ptsan_in_libc--;
}

/*----------------------- classic multibyte APIs -----------------------*/
INTERCEPTOR(int, mblen, const char *s, size_t n) {
  ENSURE_PTSAN_INITED();
  PTSAN_ENV_GUARD_BEGIN();
  int res = REAL(mblen)(clear_id(s), n);
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(int, mbtowc, wchar_t *pwc, const char *s, size_t n) {
  ENSURE_PTSAN_INITED();
  PTSAN_ENV_GUARD_BEGIN();
  int res = REAL(mbtowc)(clear_id(pwc), clear_id(s), n);
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(int, wctomb, char *s, wchar_t wc) {
  ENSURE_PTSAN_INITED();
  PTSAN_ENV_GUARD_BEGIN();
  int res = REAL(wctomb)(clear_id(s), wc);
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(size_t, mbstowcs, wchar_t *pwcs, const char *s, size_t n) {
  ENSURE_PTSAN_INITED();
  PTSAN_ENV_GUARD_BEGIN();
  size_t res = REAL(mbstowcs)(clear_id(pwcs), clear_id(s), n);
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(size_t, wcstombs, char *s, const wchar_t *pwcs, size_t n) {
  ENSURE_PTSAN_INITED();
  PTSAN_ENV_GUARD_BEGIN();
  size_t res = REAL(wcstombs)(clear_id(s), clear_id(pwcs), n);
  PTSAN_ENV_GUARD_END();
  return res;
}


/*---------------------- restartable-stateful APIs ---------------------*/
INTERCEPTOR(size_t, mbrlen, const char *s, size_t n, mbstate_t *ps) {
  ENSURE_PTSAN_INITED();
  PTSAN_ENV_GUARD_BEGIN();
  size_t res = REAL(mbrlen)(clear_id(s), n, clear_id(ps));
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(size_t, mbrtowc, wchar_t *pwc, const char *s, size_t n,
            mbstate_t *ps) {
  ENSURE_PTSAN_INITED();
  PTSAN_ENV_GUARD_BEGIN();
  size_t res = REAL(mbrtowc)(clear_id(pwc), clear_id(s), n, clear_id(ps));
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(size_t, wcrtomb, char *s, wchar_t wc, mbstate_t *ps) {
  ENSURE_PTSAN_INITED();
  PTSAN_ENV_GUARD_BEGIN();
  size_t res = REAL(wcrtomb)(clear_id(s), wc, clear_id(ps));
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(size_t, mbsrtowcs, wchar_t *dst, const char **src, size_t len,
            mbstate_t *ps) {
  ENSURE_PTSAN_INITED();
  PTSAN_ENV_GUARD_BEGIN();
  size_t res = REAL(mbsrtowcs)(clear_id(dst), clear_id(src), len,
                               clear_id(ps));
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(size_t, wcsrtombs, char *dst, const wchar_t **src, size_t len,
            mbstate_t *ps) {
  ENSURE_PTSAN_INITED();
  PTSAN_ENV_GUARD_BEGIN();
  size_t res = REAL(wcsrtombs)(clear_id(dst), clear_id(src), len,
                               clear_id(ps));
  PTSAN_ENV_GUARD_END();
  return res;
}


/*--------------------------- *_l variants -----------------------------*/
INTERCEPTOR(size_t, mbrlen_l, const char *s, size_t n, mbstate_t *ps, locale_t loc) {
  ENSURE_PTSAN_INITED();
  PTSAN_ENV_GUARD_BEGIN();
  size_t res = REAL(mbrlen_l)(clear_id(s), n, clear_id(ps), loc);
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(size_t, mbrtowc_l, wchar_t *pwc, const char *s, size_t n, mbstate_t *ps, locale_t loc) {
  ENSURE_PTSAN_INITED();
  PTSAN_ENV_GUARD_BEGIN();
  size_t res = REAL(mbrtowc_l)(clear_id(pwc), clear_id(s), n, clear_id(ps), loc);
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(size_t, wcrtomb_l, char *s, wchar_t wc, mbstate_t *ps, locale_t loc) {
  ENSURE_PTSAN_INITED();
  PTSAN_ENV_GUARD_BEGIN();
  size_t res = REAL(wcrtomb_l)(clear_id(s), wc, clear_id(ps), loc);
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(size_t, mbsrtowcs_l, wchar_t *dst, const char **src, size_t len, mbstate_t *ps, locale_t loc) {
  ENSURE_PTSAN_INITED();
  PTSAN_ENV_GUARD_BEGIN();
  size_t res = REAL(mbsrtowcs_l)(clear_id(dst), clear_id(src), len, clear_id(ps), loc);
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(size_t, wcsrtombs_l, char *dst, const wchar_t **src, size_t len, mbstate_t *ps, locale_t loc) {
  ENSURE_PTSAN_INITED();
  PTSAN_ENV_GUARD_BEGIN();
  size_t res = REAL(wcsrtombs_l)(clear_id(dst), clear_id(src), len, clear_id(ps), loc);
  PTSAN_ENV_GUARD_END();
  return res;
}

/* Some libcs also provide non-restartable *_l forms: */
INTERCEPTOR(int, mbtowc_l, wchar_t *pwc, const char *s, size_t n, locale_t loc) {
  ENSURE_PTSAN_INITED();
  PTSAN_ENV_GUARD_BEGIN();
  int res = REAL(mbtowc_l)(clear_id(pwc), clear_id(s), n, loc);
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(int, wctomb_l, char *s, wchar_t wc, locale_t loc) {
  ENSURE_PTSAN_INITED();
  PTSAN_ENV_GUARD_BEGIN();
  int res = REAL(wctomb_l)(clear_id(s), wc, loc);
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(size_t, mbstowcs_l, wchar_t *pwcs, const char *s, size_t n, locale_t loc) {
  PTSAN_ENV_GUARD_BEGIN();
  size_t res = REAL(mbstowcs_l)(clear_id(pwcs), clear_id(s), n, loc);
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(size_t, wcstombs_l, char *s, const wchar_t *pwcs, size_t n, locale_t loc) {
  ENSURE_PTSAN_INITED();
  PTSAN_ENV_GUARD_BEGIN();
  size_t res = REAL(wcstombs_l)(clear_id(s), clear_id(pwcs), n, loc);
  PTSAN_ENV_GUARD_END();
  return res;
}

// Time
/* -------- tz-sensitive interceptors -------- */

INTERCEPTOR(struct tm *, localtime, const time_t *timer) {
  PTSAN_ENV_GUARD_BEGIN();
  struct tm *res = REAL(localtime)(clear_id(timer));
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(struct tm *, localtime_r, const time_t *timer, struct tm *result) {
  PTSAN_ENV_GUARD_BEGIN();
  struct tm *res = REAL(localtime_r)(clear_id(timer), clear_id(result));
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(char *, ctime, const time_t *timer) {
  PTSAN_ENV_GUARD_BEGIN();
  char *res = REAL(ctime)(clear_id(timer));
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(char *, ctime_r, const time_t *timer, char *buf) {
  PTSAN_ENV_GUARD_BEGIN();
  char *res = REAL(ctime_r)(clear_id(timer), clear_id(buf));
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(time_t, mktime, struct tm *tm_ptr) {
  PTSAN_ENV_GUARD_BEGIN();
  time_t res = REAL(mktime)(clear_id(tm_ptr));
  PTSAN_ENV_GUARD_END();
  return res;
}

INTERCEPTOR(void, tzset, void) {
  PTSAN_ENV_GUARD_BEGIN();
  REAL(tzset)();
  PTSAN_ENV_GUARD_END();
}

/* UTC path: no TZ/env consult, so no swap needed */
INTERCEPTOR(struct tm *, gmtime, const time_t *timer) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  struct tm *res = REAL(gmtime)(clear_id(timer));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(struct tm *, gmtime_r, const time_t *timer, struct tm *result) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  struct tm *res = REAL(gmtime_r)(clear_id(timer), clear_id(result));
  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(char *, dlerror, void) {
  ptsan_in_libc++;
  char * out = REAL(dlerror)();
  ptsan_in_libc--;
  return out;
}

INTERCEPTOR(int, dladdr, const void * addr, Dl_info *info) {
  ptsan_in_libc++;
  int out = REAL(dladdr)(clear_id(addr), clear_id(info));
  ptsan_in_libc--;
  return out;
}



extern int   __ptsan_obstack_begin(struct obstack *h,
                                    int size, int alignment,
                                    void *(*chunkfun)(long),
                                    void (*freefun)(void *));
extern int   __ptsan_obstack_begin_1(struct obstack *h,
                                      int size, int alignment,
                                      void *(*chunkfun)(void *, long),
                                      void (*freefun)(void *, void *),
                                      void *arg);
extern void  __ptsan_obstack_newchunk(struct obstack *h, int length);
extern void  __ptsan_obstack_free(struct obstack *h, void *obj);
extern int   __ptsan_obstack_memory_used(struct obstack *h);


// ---- Wrappers (call PTSan functions; nothing else) ----

INTERCEPTOR(int, _obstack_begin,
            struct obstack *h, int size, int alignment,
            void *(*chunkfun)(long), void (*freefun)(void *)) {
  return __ptsan_obstack_begin(h, size, alignment, chunkfun, freefun);
}

INTERCEPTOR(int, _obstack_begin_1,
            struct obstack *h, int size, int alignment,
            void *(*chunkfun)(void *, long),
            void (*freefun)(void *, void *), void *arg) {
  return __ptsan_obstack_begin_1(h, size, alignment, chunkfun, freefun, arg);
}

INTERCEPTOR(void, _obstack_newchunk, struct obstack *h, int length) {
  __ptsan_obstack_newchunk(h, length);
}

INTERCEPTOR(void, __obstack_free, struct obstack *h, void *obj) {
  __ptsan_obstack_free(h, obj);
}

INTERCEPTOR(void, obstack_free, struct obstack *h, void *obj) {
  __ptsan_obstack_free(h, obj);
}


INTERCEPTOR(void, _obstack_free, struct obstack *h, void *obj) {
  __ptsan_obstack_free(h, obj);
}

INTERCEPTOR(int, _obstack_memory_used, struct obstack *h) {
  return __ptsan_obstack_memory_used(h);
}

// ferror
INTERCEPTOR(int, ferror, FILE *stream) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(ferror)(clear_id(stream));
  ptsan_in_libc--;
  return res;
}


INTERCEPTOR(char *, setlocale, int category, const char *locale) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  // Clean environment
  size_t envc = ptsan_strv_len(environ);
  char **clean_envp =
      (char **)__builtin_alloca((envc + 1) * sizeof(*clean_envp));
  ptsan_strv_deep_untag_into(environ, clean_envp, envc);
  char **old_environ = environ;
  environ = clean_envp;

  char *res = REAL(setlocale)(category, clear_id(locale));

  // Restore the original environment pointer.
  environ = old_environ;

  ptsan_in_libc--;
  return res;
}

INTERCEPTOR(SIZE_T, malloc_usable_size, void* ptr) {
  if(UNLIKELY(ptsan_init_is_running)) {
    return REAL(malloc_usable_size)(ptr);
  }

  ENSURE_PTSAN_INITED();

  SIZE_T out = REAL(malloc_usable_size)(clear_id(ptr));
  return out;
}

INTERCEPTOR(int, ioctl, int fd, unsigned long op, ...) {
  auto do_fn = [](int fd, unsigned long op, va_list ap) -> int {
    switch (op) {
      case FIONBIO: {
        int *argp = va_arg(ap, int *);
        argp = clear_id(argp);
        return REAL(ioctl)(fd, op, argp);
      }

      default: {
        Printf("ERROR: PTSAN: Handle ioctl %lu\n", op);
        Die();
      }
    }
  };

  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  va_list ap;
  va_start(ap, op);
  int out = do_fn(fd, op, ap);
  va_end(ap);

  ptsan_in_libc--;
  return out;
}


INTERCEPTOR(int, socketpair, int domain, int type, int protocol, int sv[2]) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(socketpair)(domain, type, protocol, clear_id(sv));
  ptsan_in_libc--;
  return res;
}

// Iovec-using functions. Need to strip ids on iovec->iov_base

static void ptsan_copy_clear_iovecs(struct iovec *dst,
                                    const struct iovec *src,
                                    size_t cnt) {
  for (size_t i = 0; i < cnt; i++) {
    dst[i].iov_base = clear_id(clear_id(src)[i].iov_base);
    dst[i].iov_len = clear_id(src)[i].iov_len;
  }
}

#define COPY_CLEAR_IOVEC(vec, n)                                               \
  if (vec && n > 0) {                                                          \
    struct iovec *tmp =                                                        \
        (struct iovec *)__builtin_alloca(sizeof(struct iovec) * n);            \
    ptsan_copy_clear_iovecs(tmp, vec, n);                                      \
    vec = tmp;                                                                 \
  }

/* process_vm_readv */
INTERCEPTOR(ssize_t, process_vm_readv,
            pid_t pid,
            const struct iovec *lvec, unsigned long liovcnt,
            const struct iovec *rvec, unsigned long riovcnt,
            unsigned long flags) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  COPY_CLEAR_IOVEC(lvec, liovcnt);
  COPY_CLEAR_IOVEC(rvec, riovcnt);

  ssize_t res = REAL(process_vm_readv)(pid,
                                       clear_id(lvec), liovcnt,
                                       clear_id(rvec), riovcnt,
                                       flags);
  ptsan_in_libc--;
  return res;
}

/* process_vm_writev */
INTERCEPTOR(ssize_t, process_vm_writev,
            pid_t pid,
            const struct iovec *lvec, unsigned long liovcnt,
            const struct iovec *rvec, unsigned long riovcnt,
            unsigned long flags) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  COPY_CLEAR_IOVEC(lvec, liovcnt);
  COPY_CLEAR_IOVEC(rvec, riovcnt);

  ssize_t res = REAL(process_vm_writev)(pid,
                                        clear_id(lvec), liovcnt,
                                        clear_id(rvec), riovcnt,
                                        flags);
  ptsan_in_libc--;
  return res;
}

/* readv */
INTERCEPTOR(ssize_t, readv,
            int fd, const struct iovec *iov, int iovcnt) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  COPY_CLEAR_IOVEC(iov, iovcnt);

  ssize_t res = REAL(readv)(fd, clear_id(iov), iovcnt);
  ptsan_in_libc--;
  return res;
}

/* writev */
INTERCEPTOR(ssize_t, writev,
            int fd, const struct iovec *iov, int iovcnt) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  COPY_CLEAR_IOVEC(iov, iovcnt);

  ssize_t res = REAL(writev)(fd, clear_id(iov), iovcnt);
  ptsan_in_libc--;
  return res;
}

/* preadv */
INTERCEPTOR(ssize_t, preadv,
            int fd, const struct iovec *iov, int iovcnt, off_t offset) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  COPY_CLEAR_IOVEC(iov, iovcnt);

  ssize_t res = REAL(preadv)(fd, clear_id(iov), iovcnt, offset);
  ptsan_in_libc--;
  return res;
}

/* pwritev */
INTERCEPTOR(ssize_t, pwritev,
            int fd, const struct iovec *iov, int iovcnt, off_t offset) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  COPY_CLEAR_IOVEC(iov, iovcnt);

  ssize_t res = REAL(pwritev)(fd, clear_id(iov), iovcnt, offset);
  ptsan_in_libc--;
  return res;
}

/* preadv2 */
INTERCEPTOR(ssize_t, preadv2,
            int fd, const struct iovec *iov, int iovcnt,
            off_t offset, int flags) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  COPY_CLEAR_IOVEC(iov, iovcnt);

  ssize_t res = REAL(preadv2)(fd, clear_id(iov), iovcnt, offset, flags);
  ptsan_in_libc--;
  return res;
}

/* pwritev2 */
INTERCEPTOR(ssize_t, pwritev2,
            int fd, const struct iovec *iov, int iovcnt,
            off_t offset, int flags) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  COPY_CLEAR_IOVEC(iov, iovcnt);

  ssize_t res = REAL(pwritev2)(fd, clear_id(iov), iovcnt, offset, flags);
  ptsan_in_libc--;
  return res;
}

/* preadv64 */
INTERCEPTOR(ssize_t, preadv64,
            int fd, const struct iovec *iov, int iovcnt, off64_t offset) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  COPY_CLEAR_IOVEC(iov, iovcnt);

  ssize_t res = REAL(preadv64)(fd, clear_id(iov), iovcnt, offset);
  ptsan_in_libc--;
  return res;
}

/* pwritev64 */
INTERCEPTOR(ssize_t, pwritev64,
            int fd, const struct iovec *iov, int iovcnt, off64_t offset) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;

  COPY_CLEAR_IOVEC(iov, iovcnt);

  ssize_t res = REAL(pwritev64)(fd, clear_id(iov), iovcnt, offset);
  ptsan_in_libc--;
  return res;
}
INTERCEPTOR(void, globfree, glob_t *pglob) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  REAL(globfree)(clear_id(pglob));
  ptsan_in_libc--;
}

// truncate(const char *path, off_t length)
INTERCEPTOR(int, truncate, const char *path, off_t length) {
  ENSURE_PTSAN_INITED();

  ptsan_in_libc++;
  int res = REAL(truncate)(clear_id(path), length);
  ptsan_in_libc--;

  return res;
}

// ftruncate(int fd, off_t length)
INTERCEPTOR(int, ftruncate, int fd, off_t length) {
  ENSURE_PTSAN_INITED();

  ptsan_in_libc++;
  int res = REAL(ftruncate)(fd, length);
  ptsan_in_libc--;

  return res;
}

INTERCEPTOR(int, truncate64, const char *path, off64_t length) {
  ENSURE_PTSAN_INITED();

  ptsan_in_libc++;
  int res = REAL(truncate64)(clear_id(path), length);
  ptsan_in_libc--;

  return res;
}

// ftruncate64(int fd, off64_t length)
INTERCEPTOR(int, ftruncate64, int fd, off64_t length) {
  ENSURE_PTSAN_INITED();

  ptsan_in_libc++;
  int res = REAL(ftruncate64)(fd, length);
  ptsan_in_libc--;

  return res;
}

// int ungetc(int c, FILE *stream);
INTERCEPTOR(int, ungetc, int c, FILE *stream) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(ungetc)(c, clear_id(stream));
  ptsan_in_libc--;
  return res;
}

// wint_t ungetwc(wint_t wc, FILE *stream);
INTERCEPTOR(wint_t, ungetwc, wint_t wc, FILE *stream) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  wint_t res = REAL(ungetwc)(wc, clear_id(stream));
  ptsan_in_libc--;
  return res;
}

// int ungetc_unlocked(int c, FILE *stream);
INTERCEPTOR(int, ungetc_unlocked, int c, FILE *stream) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(ungetc_unlocked)(c, clear_id(stream));
  ptsan_in_libc--;
  return res;
}

// wint_t ungetwc_unlocked(wint_t wc, FILE *stream);
INTERCEPTOR(wint_t, ungetwc_unlocked, wint_t wc, FILE *stream) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  wint_t res = REAL(ungetwc_unlocked)(wc, clear_id(stream));
  ptsan_in_libc--;
  return res;
}


// int fgetc_unlocked(FILE *stream);
INTERCEPTOR(int, fgetc_unlocked, FILE *stream) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(fgetc_unlocked)(clear_id(stream));
  ptsan_in_libc--;
  return res;
}

// int getc_unlocked(FILE *stream);
INTERCEPTOR(int, getc_unlocked, FILE *stream) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(getc_unlocked)(clear_id(stream));
  ptsan_in_libc--;
  return res;
}

// int getchar_unlocked(void);
INTERCEPTOR(int, getchar_unlocked, void) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(getchar_unlocked)();
  ptsan_in_libc--;
  return res;
}

// int feof(FILE *stream);
INTERCEPTOR(int, feof, FILE *stream) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(feof)(clear_id(stream));
  ptsan_in_libc--;
  return res;
}

// int feof_unlocked(FILE *stream);
INTERCEPTOR(int, feof_unlocked, FILE *stream) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(feof_unlocked)(clear_id(stream));
  ptsan_in_libc--;
  return res;
}

// int shm_open(const char *name, int oflag, mode_t mode);
INTERCEPTOR(int, shm_open, const char *name, int oflag, mode_t mode) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(shm_open)(clear_id(name), oflag, mode);
  ptsan_in_libc--;
  return res;
}

// int shm_unlink(const char *name);
INTERCEPTOR(int, shm_unlink, const char *name) {
  ENSURE_PTSAN_INITED();
  ptsan_in_libc++;
  int res = REAL(shm_unlink)(clear_id(name));
  ptsan_in_libc--;
  return res;
}

void __ptsan::InitializeLibCHeaderInterceptors() {
  INTERCEPT_FUNCTION(pthread_key_create);
  INTERCEPT_FUNCTION(pthread_setschedparam);
  INTERCEPT_FUNCTION(pthread_attr_setschedparam);

  INTERCEPT_FUNCTION(pthread_mutex_init);
  INTERCEPT_FUNCTION(pthread_mutex_destroy);
  INTERCEPT_FUNCTION(pthread_mutex_lock);
  INTERCEPT_FUNCTION(pthread_mutex_trylock);
  INTERCEPT_FUNCTION(pthread_mutex_timedlock);
  INTERCEPT_FUNCTION(pthread_mutex_unlock);

  INTERCEPT_FUNCTION(pthread_mutexattr_init);
  INTERCEPT_FUNCTION(pthread_mutexattr_setprotocol);
  INTERCEPT_FUNCTION(pthread_mutexattr_settype);
  INTERCEPT_FUNCTION(pthread_attr_init);
  INTERCEPT_FUNCTION(pthread_attr_setstack);
  INTERCEPT_FUNCTION(pthread_attr_setdetachstate);
  INTERCEPT_FUNCTION(pthread_attr_setstacksize);
  INTERCEPT_FUNCTION(pthread_attr_setinheritsched);
  INTERCEPT_FUNCTION(pthread_attr_setschedpolicy);
  INTERCEPT_FUNCTION(pthread_attr_destroy);
  INTERCEPT_FUNCTION(timer_create);
  INTERCEPT_FUNCTION(pthread_register_cancel);
  INTERCEPT_FUNCTION(__pthread_unwind_next);


  INTERCEPT_FUNCTION(pthread_cond_init);
  INTERCEPT_FUNCTION(pthread_cond_destroy);
  INTERCEPT_FUNCTION(pthread_cond_wait);
  INTERCEPT_FUNCTION(pthread_cond_timedwait);
  INTERCEPT_FUNCTION(pthread_cond_signal);
  INTERCEPT_FUNCTION(pthread_cond_broadcast);

  INTERCEPT_FUNCTION(pthread_rwlock_init);
  INTERCEPT_FUNCTION(pthread_rwlock_destroy);
  INTERCEPT_FUNCTION(pthread_rwlock_rdlock);
  INTERCEPT_FUNCTION(pthread_rwlock_tryrdlock);
  INTERCEPT_FUNCTION(pthread_rwlock_wrlock);
  INTERCEPT_FUNCTION(pthread_rwlock_trywrlock);
  INTERCEPT_FUNCTION(pthread_rwlock_unlock);

  INTERCEPT_FUNCTION(__cxa_allocate_exception);

  INTERCEPT_FUNCTION(sigaddset);
  INTERCEPT_FUNCTION(sigdelset);
  INTERCEPT_FUNCTION(sigismember);
  INTERCEPT_FUNCTION(sigisemptyset);
  INTERCEPT_FUNCTION(sigaddsetp);
  INTERCEPT_FUNCTION(sigdelsetp);

  INTERCEPT_FUNCTION(getrusage);
  INTERCEPT_FUNCTION(getrlimit);
  INTERCEPT_FUNCTION(setrlimit);
  INTERCEPT_FUNCTION(prlimit);

  INTERCEPT_FUNCTION(fseek);
  INTERCEPT_FUNCTION(ftell);
  INTERCEPT_FUNCTION(rewind);
  INTERCEPT_FUNCTION(fgetpos);
  INTERCEPT_FUNCTION(fsetpos);

  INTERCEPT_FUNCTION(getopt);
  INTERCEPT_FUNCTION(getopt_long);
  INTERCEPT_FUNCTION(getopt_long_only);
  INTERCEPT_FUNCTION(getsubopt);

  INTERCEPT_FUNCTION(mkdir);
  INTERCEPT_FUNCTION(mkdirat);

  INTERCEPT_FUNCTION(fgetc);
  INTERCEPT_FUNCTION(getc);
  INTERCEPT_FUNCTION(getchar);
  INTERCEPT_FUNCTION(fputc);
  INTERCEPT_FUNCTION(putc);
  INTERCEPT_FUNCTION(putchar);

  INTERCEPT_FUNCTION(setuid);
  INTERCEPT_FUNCTION(setgid);
  INTERCEPT_FUNCTION(geteuid);

  INTERCEPT_FUNCTION(open);
  INTERCEPT_FUNCTION(openat);
  INTERCEPT_FUNCTION(creat);
  INTERCEPT_FUNCTION(open64);
  INTERCEPT_FUNCTION(openat64);
  INTERCEPT_FUNCTION(creat64);


  INTERCEPT_FUNCTION(mkstemp);
  INTERCEPT_FUNCTION(mkostemp);
  INTERCEPT_FUNCTION(mkstemps);
  INTERCEPT_FUNCTION(mkostemps);
  INTERCEPT_FUNCTION(mktemp);
  INTERCEPT_FUNCTION(mkdtemp);
  INTERCEPT_FUNCTION(rmdir);

  INTERCEPT_FUNCTION(chmod);
  INTERCEPT_FUNCTION(closedir);

  INTERCEPT_FUNCTION(unlink);
  INTERCEPT_FUNCTION(unlinkat);
  INTERCEPT_FUNCTION(link);
  INTERCEPT_FUNCTION(linkat);

  INTERCEPT_FUNCTION(tsearch);
  INTERCEPT_FUNCTION(tfind);
  INTERCEPT_FUNCTION(tdelete);
  INTERCEPT_FUNCTION(twalk);
  INTERCEPT_FUNCTION(tdestroy);

  //INTERCEPT_FUNCTION(setjmp);
  INTERCEPT_FUNCTION(longjmp);
  //INTERCEPT_FUNCTION(_setjmp);
  INTERCEPT_FUNCTION(_longjmp);
  //INTERCEPT_FUNCTION(__sigsetjmp);
  INTERCEPT_FUNCTION(siglongjmp);

  INTERCEPT_FUNCTION(catopen);
  INTERCEPT_FUNCTION(catgets);
  INTERCEPT_FUNCTION(catclose);

  INTERCEPT_FUNCTION(strcoll);
  INTERCEPT_FUNCTION(strcoll_l);
  INTERCEPT_FUNCTION(wcscoll);

  INTERCEPT_FUNCTION(tmpfile);
  INTERCEPT_FUNCTION(tmpfile64);

  INTERCEPT_FUNCTION(sigaction);
  INTERCEPT_FUNCTION(signal);


  INTERCEPT_FUNCTION(sysconf);
  INTERCEPT_FUNCTION(pthread_cancel);
  INTERCEPT_FUNCTION(mq_open);
  INTERCEPT_FUNCTION(mq_unlink);
  INTERCEPT_FUNCTION(mq_receive);
  INTERCEPT_FUNCTION(mq_timedreceive);
  INTERCEPT_FUNCTION(mq_timedsend);
  INTERCEPT_FUNCTION(sendmsg);
  INTERCEPT_FUNCTION(pthread_exit);
  INTERCEPT_FUNCTION(__pthread_register_cancel);
  INTERCEPT_FUNCTION(___pthread_unregister_cancel);
  INTERCEPT_FUNCTION(__pthread_unregister_cancel);
  INTERCEPT_FUNCTION(clock_nanosleep);
  INTERCEPT_FUNCTION(sched_setaffinity);
  INTERCEPT_FUNCTION(pthread_setaffinity_np);
  INTERCEPT_FUNCTION(setsockopt);
  INTERCEPT_FUNCTION(timer_settime);
  INTERCEPT_FUNCTION(select);
  INTERCEPT_FUNCTION(futex);
  INTERCEPT_FUNCTION(pthread_join);
  INTERCEPT_FUNCTION(___pthread_join);
  INTERCEPT_FUNCTION(__pthread_clockjoin_ex);
  INTERCEPT_FUNCTION(sigcancel_handler);
  INTERCEPT_FUNCTION(pthread_barrierattr_setpshared);

  INTERCEPT_FUNCTION(rename);
  INTERCEPT_FUNCTION(renameat);
  INTERCEPT_FUNCTION(renameat2);
  INTERCEPT_FUNCTION(remove);

  INTERCEPT_FUNCTION(wmemchr);
  INTERCEPT_FUNCTION(wmemcmp);

  INTERCEPT_FUNCTION(chdir);
  INTERCEPT_FUNCTION(fchdir);

  INTERCEPT_FUNCTION(pipe);
  INTERCEPT_FUNCTION(pipe2);

  INTERCEPT_FUNCTION(getenv);
  INTERCEPT_FUNCTION(setenv);
  INTERCEPT_FUNCTION(putenv);

  INTERCEPT_FUNCTION(set_robust_list);
  INTERCEPT_FUNCTION(execve);
  INTERCEPT_FUNCTION(fexecve);
  INTERCEPT_FUNCTION(execv);
  INTERCEPT_FUNCTION(execvp);
  INTERCEPT_FUNCTION(execvpe);
  INTERCEPT_FUNCTION(execl);
  INTERCEPT_FUNCTION(execlp);
  INTERCEPT_FUNCTION(execle);

  INTERCEPT_FUNCTION(clone);

  INTERCEPT_FUNCTION(__cxa_atexit);
  INTERCEPT_FUNCTION(epoll_ctl);
  INTERCEPT_FUNCTION(strtok_r);
  INTERCEPT_FUNCTION(pthread_spin_destroy);
  INTERCEPT_FUNCTION(pthread_spin_init);
  INTERCEPT_FUNCTION(pthread_spin_trylock);
  INTERCEPT_FUNCTION(pthread_spin_lock);
  INTERCEPT_FUNCTION(pthread_spin_unlock);
  INTERCEPT_FUNCTION(pthread_condattr_init);
  INTERCEPT_FUNCTION(strcasecmp_l);
  INTERCEPT_FUNCTION(strncasecmp_l);
  INTERCEPT_FUNCTION(strlcpy);
  INTERCEPT_FUNCTION(strlcat);
  INTERCEPT_FUNCTION(pthread_condattr_destroy);
  INTERCEPT_FUNCTION(pthread_condattr_setclock);
  INTERCEPT_FUNCTION(pthread_getattr_np);
  INTERCEPT_FUNCTION(pthread_attr_setguardsize);


  INTERCEPT_FUNCTION(wcschr);
  INTERCEPT_FUNCTION(wcsrchr);
  INTERCEPT_FUNCTION(wcsstr);
  INTERCEPT_FUNCTION(wcspbrk);
  INTERCEPT_FUNCTION(wcsspn);
  INTERCEPT_FUNCTION(wcscspn);
  INTERCEPT_FUNCTION(wcsncmp);
  INTERCEPT_FUNCTION(wcscpy);
  INTERCEPT_FUNCTION(wcsncpy);
  INTERCEPT_FUNCTION(wmemchr);
  INTERCEPT_FUNCTION(wmemcmp);
  INTERCEPT_FUNCTION(posix_spawn_file_actions_addclosefrom_np);
  INTERCEPT_FUNCTION(posix_spawn_file_actions_adddup2);
  INTERCEPT_FUNCTION(posix_spawn_file_actions_init);
  INTERCEPT_FUNCTION(posix_spawn_file_actions_destroy);
  INTERCEPT_FUNCTION(posix_spawn_file_actions_addclose);
  INTERCEPT_FUNCTION(posix_spawn_file_actions_addopen);


  INTERCEPT_FUNCTION(posix_spawnattr_init);
  INTERCEPT_FUNCTION(posix_spawnattr_destroy);
  INTERCEPT_FUNCTION(posix_spawnattr_setflags);
  INTERCEPT_FUNCTION(posix_spawnattr_getflags);
  INTERCEPT_FUNCTION(posix_spawnattr_setsigmask);
  INTERCEPT_FUNCTION(posix_spawnattr_getsigmask);
  INTERCEPT_FUNCTION(posix_spawnattr_setsigdefault);
  INTERCEPT_FUNCTION(posix_spawnattr_getsigdefault);
  INTERCEPT_FUNCTION(posix_spawnattr_setpgroup);
  INTERCEPT_FUNCTION(posix_spawnattr_getpgroup);
  INTERCEPT_FUNCTION(posix_spawnattr_setschedpolicy);
  INTERCEPT_FUNCTION(posix_spawnattr_getschedpolicy);
  INTERCEPT_FUNCTION(posix_spawnattr_setschedparam);
  INTERCEPT_FUNCTION(posix_spawnattr_getschedparam);
  INTERCEPT_FUNCTION(posix_spawn);
  INTERCEPT_FUNCTION(opendir);
  INTERCEPT_FUNCTION(ffi_prep_cif);
  INTERCEPT_FUNCTION(ffi_call);
  INTERCEPT_FUNCTION(__fdopendir);

  INTERCEPT_FUNCTION(rewinddir);
  INTERCEPT_FUNCTION(seekdir);
  INTERCEPT_FUNCTION(telldir);
  INTERCEPT_FUNCTION(dirfd);
  INTERCEPT_FUNCTION(scandirat);
  INTERCEPT_FUNCTION(scandirat64);

  INTERCEPT_FUNCTION(mblen);
  INTERCEPT_FUNCTION(mbtowc);
  INTERCEPT_FUNCTION(wctomb);
  INTERCEPT_FUNCTION(mbstowcs);
  INTERCEPT_FUNCTION(wcstombs);

  INTERCEPT_FUNCTION(mbrlen);
  INTERCEPT_FUNCTION(mbrtowc);
  INTERCEPT_FUNCTION(wcrtomb);
  INTERCEPT_FUNCTION(mbsrtowcs);
  INTERCEPT_FUNCTION(wcsrtombs);

  INTERCEPT_FUNCTION(mbrlen_l);
  INTERCEPT_FUNCTION(mbrtowc_l);
  INTERCEPT_FUNCTION(wcrtomb_l);
  INTERCEPT_FUNCTION(mbsrtowcs_l);
  INTERCEPT_FUNCTION(wcsrtombs_l);

  INTERCEPT_FUNCTION(mbtowc_l);
  INTERCEPT_FUNCTION(wctomb_l);
  INTERCEPT_FUNCTION(mbstowcs_l);
  INTERCEPT_FUNCTION(wcstombs_l);

  INTERCEPT_FUNCTION(localtime);
  INTERCEPT_FUNCTION(localtime_r);
  INTERCEPT_FUNCTION(ctime);
  INTERCEPT_FUNCTION(ctime_r);
  INTERCEPT_FUNCTION(mktime);
  INTERCEPT_FUNCTION(tzset);
  INTERCEPT_FUNCTION(gmtime);
  INTERCEPT_FUNCTION(gmtime_r);

  INTERCEPT_FUNCTION(_obstack_begin);
  INTERCEPT_FUNCTION(_obstack_begin_1);
  INTERCEPT_FUNCTION(_obstack_newchunk);
  INTERCEPT_FUNCTION(obstack_free);
  INTERCEPT_FUNCTION(__obstack_free);
  INTERCEPT_FUNCTION(_obstack_memory_used);

  INTERCEPT_FUNCTION(ferror);

  INTERCEPT_FUNCTION(setlocale);

  INTERCEPT_FUNCTION(malloc_usable_size);

  INTERCEPT_FUNCTION(ioctl);
  INTERCEPT_FUNCTION(socketpair);

  INTERCEPT_FUNCTION(process_vm_readv);
  INTERCEPT_FUNCTION(process_vm_writev);
  INTERCEPT_FUNCTION(readv);
  INTERCEPT_FUNCTION(writev);
  INTERCEPT_FUNCTION(preadv);
  INTERCEPT_FUNCTION(pwritev);
  INTERCEPT_FUNCTION(preadv2);
  INTERCEPT_FUNCTION(pwritev2);
  INTERCEPT_FUNCTION(preadv64);
  INTERCEPT_FUNCTION(pwritev64);
  INTERCEPT_FUNCTION(globfree);

  INTERCEPT_FUNCTION(truncate);
  INTERCEPT_FUNCTION(ftruncate);
  INTERCEPT_FUNCTION(truncate64);
  INTERCEPT_FUNCTION(ftruncate64);
  INTERCEPT_FUNCTION(dlerror);
  INTERCEPT_FUNCTION(dladdr);

  INTERCEPT_FUNCTION(ungetc);
  INTERCEPT_FUNCTION(ungetwc);
  INTERCEPT_FUNCTION(ungetc_unlocked);
  INTERCEPT_FUNCTION(ungetwc_unlocked);

  INTERCEPT_FUNCTION(fgetc_unlocked);
  INTERCEPT_FUNCTION(getc_unlocked);
  INTERCEPT_FUNCTION(getchar_unlocked);

  INTERCEPT_FUNCTION(feof);
  INTERCEPT_FUNCTION(feof_unlocked);

  INTERCEPT_FUNCTION(shm_open);
  INTERCEPT_FUNCTION(shm_unlink);
}
