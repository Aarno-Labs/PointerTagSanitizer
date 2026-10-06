//===-- ptsan_interceptors_format.cpp -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PointerTagSanitizer.
//
// PTSan interceptors for formatted output.
//
// Derived from the DataFlowSanitizer interceptors.
//
//===----------------------------------------------------------------------===//

#include "ptsan.h"
#include "ptsan_interceptors.h"
#include "ptsan_bounds.h"
#include "ptsan_thread.h"
#include "ptsan_ptr_utils.h"

#include "interception/interception.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "sanitizer_common/sanitizer_linux.h"
#include "sanitizer_common/sanitizer_type_traits.h"
#include "sanitizer_common/sanitizer_allocator_internal.h"

// Internal libc helpers that handle PTSan-tagged pointers.
#include "ptsan_libc.inc"

using namespace __sanitizer;
using namespace __ptsan;

FILE *get_stdin() {
  return reinterpret_cast<FILE *>(stdin);
}

// Re-implementing printfs/scanfs, because I guess there's
// no way to just intercept a va_list >.<

// Largely taken from dfsan

DECLARE_REAL(int, snprintf, char *str, SIZE_T size, const char *format, ...)
DECLARE_REAL(int, printf, const char *format, ...)
DECLARE_REAL(int, fprintf, FILE *stream, const char *format, ...)
DECLARE_REAL(int, fprintf, FILE *stream, const char *format, ...)
DECLARE_REAL(void, free, void *ptr)
DECLARE_REAL(void *, malloc, SIZE_T size)

// Formats a chunk either a constant string or a single format directive (e.g.,
// '%.3f').
class Formatter {
public:
  Formatter(bool dry_run, char *str_, const char *fmt_, SIZE_T size_)
      : dry_run(dry_run),
        str(clear_id(str_)),
        // XXX: save id when checking?
        str_off(0),
        size(size_),
        fmt_start(clear_id(fmt_)),
        fmt_cur(clear_id(fmt_)),
        width(-1),
        precision(-1) {}

  int format_txt() {
    if (dry_run) {
      return 0;
    }
    char *tmp_fmt = build_text_string();
    int retval =
        REAL(snprintf)(str + str_off, str_off < size ? size - str_off : 0, tmp_fmt,
                 0 /* used only to avoid warnings */);
    REAL(free)(tmp_fmt);
    return retval;
  }

  template <typename T> int format_ctrl(T arg) {
    if (dry_run) {
      precision = -1;
      width = -1;
      return 0;
    }
    char *tmp_fmt = build_ctrl_string();
    int retval;
    // XXX: check accesses here?
    // This could be more efficient, but I don't really care.
    if (width >= 0 && precision >= 0) {
      retval = REAL(snprintf)(str + str_off, str_off < size ? size - str_off : 0,
                        tmp_fmt, width, precision, stripID(arg));
      width = -1;
      precision = -1;
    } else if (width >= 0) {
      retval = REAL(snprintf)(str + str_off, str_off < size ? size - str_off : 0,
                        tmp_fmt, width, stripID(arg));
      width = -1;
    } else if (precision >= 0) {
      retval = REAL(snprintf)(str + str_off, str_off < size ? size - str_off : 0,
                        tmp_fmt, precision, stripID(arg));
      precision = -1;
    } else {
      retval = REAL(snprintf)(str + str_off, str_off < size ? size - str_off : 0,
                        tmp_fmt, stripID(arg));
    }
    REAL(free)(tmp_fmt);
    return retval;
  }

  bool dry_run;
  char *str;
  SIZE_T str_off;
  SIZE_T size;
  const char *fmt_start;
  const char *fmt_cur;
  int width;
  int precision;

private:
  char *build_text_string() {
    SIZE_T fmt_size = fmt_cur - fmt_start + 1;
    char *new_fmt = (char *)REAL(malloc)(fmt_size + 1);
    CHECK(new_fmt);
    internal_memcpy(new_fmt, fmt_start, fmt_size);
    new_fmt[fmt_size] = '\0';
    return new_fmt;
  }

  char *build_ctrl_string() {
    SIZE_T fmt_size = fmt_cur - fmt_start + 1;
    char *new_fmt = (char *)REAL(malloc)(fmt_size + 2);
    CHECK(new_fmt);
    internal_memcpy(new_fmt + 1, fmt_start, fmt_size);
    new_fmt[fmt_size + 1] = '\0';
    new_fmt[0] = '%';
    return new_fmt;
  }
};

using Eater = void(*)(va_list *);

// A va_list parameter is represented as a pointer on x86-64, but as a struct
// passed indirectly on AArch64. In either case an instrumented caller may pass
// the address of its tagged va_list storage to this uninstrumented interceptor.
// Strip that address at the interceptor boundary before va_copy/va_arg access
// the storage. For an indirect struct parameter, &ap is the ABI-provided
// pointer; for a pointer parameter, ap is the pointer itself.
template <typename T>
static decltype(auto) stripVaListID(T &ap) {
  if constexpr (is_pointer<T>::value) {
    return clear_id(ap);
  } else {
    return *clear_id(&ap);
  }
}

class ArgHandler {
  va_list ap;
  va_list ap_0; // not modified
  Eater arg_eaters[32];

public:
  bool dry_run;

  ArgHandler(va_list in) {
    va_copy(ap, in);
    va_copy(ap_0, in);
    dry_run = false;
  }
  ~ArgHandler() {
    va_end(ap);
    va_end(ap_0);
  }

  // No copies!
  ArgHandler(const ArgHandler&) = delete;
  ArgHandler& operator=(const ArgHandler&) = delete;
  ArgHandler(ArgHandler&&) = delete;
  ArgHandler& operator=(ArgHandler&&) = delete;

  template<typename T> T getArg(size_t idx) {
    if (dry_run && idx > 0) {
      // Only have so much space here and I'm too lazy to do dynamic/realloc
      CHECK(idx < 32);

      auto omnom = [](va_list *l) {
        va_arg(*l, T);
      };

      arg_eaters[idx] = omnom;
      return (T)0;
    }

    if (dry_run) {
      return (T)0;
    }

    // 0 isn't a valid index -- they're one-indexed
    if (idx > 0) {
      CHECK(idx < 32);

      va_list l;
      va_copy(l, ap_0);
      // idx is 1-indexed, so we never hit/use 0;
      for (size_t i = 1; i < idx; i++) {
        arg_eaters[i](&l);
      }
      T out = va_arg(l, T);
      va_end(l);
      return out;
    } else {
      return va_arg(ap, T);
    }
  }
};

// Formats the input and propagates the input labels to the output. The output
// is stored in 'str'. 'size' bounds the number of output bytes. 'format' and
// 'ap' are the format string and the list of arguments for formatting. Returns
// the return value vsnprintf would return.
//
// The function tokenizes the format string in chunks representing either a
// constant string or a single format directive (e.g., '%.3f') and formats each
// chunk independently into the output string. This approach allows to figure
// out which bytes of the output string depends on which argument and thus to
// propagate labels more precisely.
//
static int format_buffer(char *str, SIZE_T size, const char *fmt, va_list ap) {
  ArgHandler args(ap);
  bool isKnownPositional = false;
  bool isKnownNonpositional = false;

  // At the end of this, f.fmt_cur points to the last element of the
  // noncontrol sequence
  auto handle_ordinary = [&](Formatter &f) -> int {
    // parse ordinary until we hit a format char or \0
    while (true) {
      if (*f.fmt_cur == 0 || *f.fmt_cur == '%') {
        break;
      }
      f.fmt_cur++;
    }
    CHECK(*f.fmt_cur == 0 || *f.fmt_cur == '%');
    // We want to point at the last valid character, which the end isn't.
    f.fmt_cur--;
    return f.format_txt();
  };


  auto handle_index = [&](Formatter &f) -> size_t {
    const char *p = f.fmt_cur;
    size_t pos = 0;

    while (true) {
      char c = *p;
      if (c >= '0' && c <= '9') {
        unsigned d = (unsigned)(c - '0');
        pos = pos * 10 + d;
        ++p;
        continue;
      }
      if (c == '$' && pos > 0) {
        // Now actually consume this many chars. We point
        // to one-after the number
        f.fmt_cur = p + 1;
        // If we've failed to get a position berfore, we should *not*
        // be finding one now.
        CHECK(!isKnownNonpositional);
        isKnownPositional = true;
        return pos;
      }
      if (c == '$') {
        Printf("%%<non-positive-number>$ -- this is not allowed\n");
        Die();
      }
      if (c == '\0') {
        Printf("format string with unfinished control sequence!\n");
        Die();
      }

      // If we've gotten a position before, we must always find one
      CHECK(!isKnownPositional);
      isKnownNonpositional = true;
      return 0;
    }
  };

  // At the end of this. f.fmt_cur points to the last element of the control
  // sequence
  auto handle_ctrl = [&](Formatter &f, size_t idx) -> int {
    CHECK((idx == 0 && isKnownNonpositional) ||
           idx > 0 && isKnownPositional);
    // Conversion directive. Consume all the characters until a conversion
    // specifier or the end of the string.
    while (true) {
      switch (*f.fmt_cur) {
      case 'd':
      case 'i':
      case 'o':
      case 'u':
      case 'x':
      case 'X':
        // Look back at size specifier
        switch (*(f.fmt_cur - 1)) {
        case 'h':
          // Also covers the 'hh' case (since the size of the arg is still
          // an int).
          return f.format_ctrl(args.getArg<int>(idx));
        case 'l':
          if (f.fmt_cur - f.fmt_start >= 2 &&
              *(f.fmt_cur - 2) == 'l') {
            return f.format_ctrl(args.getArg<long long int>(idx));
          } else {
            return f.format_ctrl(args.getArg<long int>(idx));
          }
          break;
        case 'q':
          return f.format_ctrl(args.getArg<long long int>(idx));
        case 'j':
          return f.format_ctrl(args.getArg<INTMAX_T>(idx));
        case 'z':
        case 't':
          return f.format_ctrl(args.getArg<SIZE_T>(idx));
        default:
          return f.format_ctrl(args.getArg<int>(idx));
        }

      case 'a':
      case 'A':
      case 'e':
      case 'E':
      case 'f':
      case 'F':
      case 'g':
      case 'G':
        if (*(f.fmt_cur - 1) == 'L') {
          return f.format_ctrl(args.getArg<long double>(idx));
        } else {
          return f.format_ctrl(args.getArg<double>(idx));
        }

      case 'c':
        return f.format_ctrl(args.getArg<int>(idx));

      case 's': {
        char *arg = args.getArg<char *>(idx);
        if (arg == nullptr) {
          return f.format_ctrl(arg);
        }

        // In theory, sprintf-ing from a buffer into itself is UB, but in practice, at least
        // one spec benchmark (507) relies on `sprintf(buf, "%s %d", buf, n);` working. Strdup-ing
        // here makes that case work
        char *str = internal_strdup(arg);
        int out = f.format_ctrl(str);
        InternalFree(str);
        return out;
      }

      case 'p': {
        void *arg = args.getArg<void *>(idx);
        return f.format_ctrl(arg);
      }

      case 'n': {
        int *ptr = args.getArg<int *>(idx);
        int *real_ptr = clear_id(ptr);
        *real_ptr = (int)f.str_off;
        return 0; // 0 printed chars
      }

      // This is just a normal character: '%%' -> '%'
      case '%':
        // "normal" text includes the first character, while
        // control text doesn't. This is because control text needs to
        // cut out any positional characters or else when we chop them up,
        // they point to the wrong spot. So here, we have to push back one
        // character again
        f.fmt_start -= 1;
        return f.format_txt();

      case '\0':
        Printf("format string with unfinished control sequence!\n");
        Die();

      case '*':
        CHECK(isKnownNonpositional && "Need to support positional width/positions");
        if (*(f.fmt_cur - 1) == '.') {
          CHECK(f.precision == -1 && "Precision already set?");
          f.precision = args.getArg<int>(idx);
        } else {
          CHECK(f.precision == -1 && "Width should always be set before precision");
          CHECK(f.width == -1 && "Width already set?");
          f.width = args.getArg<int>(idx);
        }
        f.fmt_cur++;
        break;

      default:
        f.fmt_cur++;
      }
    }
  };

  auto parse_string = [&] (bool dry_run) -> size_t {
    Formatter f(dry_run, str, fmt, size);
    args.dry_run = dry_run;

    while (*f.fmt_cur != '\0') {
      f.fmt_start = f.fmt_cur;

      int retval;
      if (*f.fmt_cur == '%') {
        f.fmt_cur++;
        size_t index = handle_index(f);
        // Format start does *not* include the '%' or any positional-ness
        f.fmt_start = f.fmt_cur;
        retval = handle_ctrl(f, index);
      } else {
        retval = handle_ordinary(f);
      }

      // Before this, fmt_cur was the last character of whatever
      // sequence. Now this is the first character of the next
      // sequence, which is what we want.
      CHECK(*f.fmt_cur != '\0');
      f.fmt_cur++;
      if (retval < 0) {
        return retval;
      }

      f.str_off += retval;
    }

    // If we get to the end sucessfully, return the total number of
    // characters written
    return f.str_off;
  };

  // Do a dry run first to figure out how to parse our
  // arguments
  parse_string(true);

  // Now run for real
  return parse_string(false);
}

// Scans a chunk either a constant string or a single format directive (e.g.,
// '%.3f').
template <typename SrcT>
class Scanner {
public:
  Scanner(SrcT src_,
          int (*ScanFn)(SrcT /*src*/, const char * /*fmt*/, ...),
          const char *fmt_)
      : src(clear_id(src_)),
        ScanFn(ScanFn),
        // XXX: Save string id when checking?
        str_off(0),
        fmt_start(clear_id(fmt_)),
        fmt_cur(clear_id(fmt_)),
        num_scanned(0),
        skip(false) {}

  // Consumes a chunk of ordinary characters.
  // Returns number of matching ordinary characters.
  // Returns -1 if the match failed.
  // In format strings, a space will match multiple spaces.
  int check_match_ordinary() {
    char *tmp_fmt = build_format_string_with_n();
    int read_count = 0;
    //Printf("About to scan for ordinary `%s`\n", tmp_fmt);
    //Printf("src = `%s`\n", current_src());
    int retval = ScanFn(current_src(), tmp_fmt, &read_count);
    //Printf("Retval = %d\n", retval);
    //Printf("Read_count = %d\n", read_count);

    REAL(free)(tmp_fmt);
    if (retval >= 0) {
      str_off += read_count;
      return read_count;
    } else {
      // correctly return an error
      CHECK(read_count == 0);
      return retval;
    }
  }

  int scan() {
    char *tmp_fmt = build_format_string_with_n();
    int read_count = 0;
    int retval = ScanFn(current_src(), tmp_fmt, &read_count);
    REAL(free)(tmp_fmt);
    if (retval >= 0) {
      num_scanned += retval;
      return read_count;
    } else {
      // correctly return an error
      CHECK(read_count == 0);
      return retval;
    }
  }

  template <typename T>
  int scan(T arg) {
    char *tmp_fmt = build_format_string_with_n();
    //Printf("About to scan for format string `%s`\n", tmp_fmt);
    //Printf("src = `%s`\n", current_src());
    int read_count = 0;
    // XXX: check overflow
    int retval = ScanFn(current_src(), tmp_fmt, stripID(arg), &read_count);
    //Printf("Retval = %d\n", retval);
    REAL(free)(tmp_fmt);
    if (retval >= 0) {
      num_scanned += retval;
      return read_count;
    } else {
      // correctly return an error
      CHECK(read_count == 0);
      return retval;
    }
  }

  // Adds %n onto current format string to measure length.
  char *build_format_string_with_n() {
    SIZE_T fmt_size = fmt_cur - fmt_start + 1;
    // +2 for %n, +1 for \0
    char *new_fmt = (char *)REAL(malloc)(fmt_size + 2 + 1);
    CHECK(new_fmt);
    internal_memcpy(new_fmt, fmt_start, fmt_size);
    new_fmt[fmt_size] = '%';
    new_fmt[fmt_size + 1] = 'n';
    new_fmt[fmt_size + 2] = '\0';
    return new_fmt;
  }

  SrcT src;
  int (*ScanFn)(SrcT /*src*/, const char * /*fmt*/, ...);
  SIZE_T str_off;
  const char *fmt_start;
  const char *fmt_cur;
  int num_scanned;
  bool skip;

  private:
  /* current_src(): pointer the backend must start scanning at */
  SrcT current_src() const {
    if constexpr (is_same<SrcT, const char *>::value)
      return src + str_off;         /* sscanf: advance by offset */
    else
      return src;                   /* fscanf: FILE* ignores offset */
  }
};

// This function is an inverse of format_buffer: we take the input buffer,
// scan it in search for format strings and store the results in the varargs.
// The labels are propagated from the input buffer to the varargs.
template <typename SrcT>
static int scan_buffer(SrcT src,
                       int (*ScanFn)(SrcT /*src*/, const char * /*fmt*/, ...),
                       const char *fmt,
                       va_list ap) {
  Scanner<SrcT> scanner(src, ScanFn, fmt);
  bool hasMatched = false;
  while (*scanner.fmt_cur) {
    scanner.fmt_start = scanner.fmt_cur;
    scanner.skip = false;
    int read_count = 0;
    void *dst_ptr = 0;
    SIZE_T write_size = 0;
    if (*scanner.fmt_cur != '%') {
      // Ordinary character and spaces.
      // Consume all the characters until a '%' or the end of the string.
      for (; *(scanner.fmt_cur + 1) && *(scanner.fmt_cur + 1) != '%';
           ++scanner.fmt_cur) {
      }

      int matched_chars = scanner.check_match_ordinary();
      //Printf("Matched %d chars\n", matched_chars);
      if (matched_chars == EOF) {
        // The ordinary characters eof-d
        return EOF;
        break;
      }

      hasMatched = true;

      if (matched_chars == 0) {
        // didn't match, no eof
        break;
      }

      CHECK(matched_chars > 0);

    } else {
      // Conversion directive. Consume all the characters until a conversion
      // specifier or the end of the string.
      bool end_fmt = false;
      bool in_scanset = false;
      for (; *scanner.fmt_cur && !end_fmt;) {
        const char c = *++scanner.fmt_cur;

        // In start, we don't break on ']', and
        // if we see '^', 'start' extends another character
        if (in_scanset) {
          // Scanset ends on ']', exceeeept:
          if (c == ']') {
            // If ']' is the first char, it counts as a char not an end
            char prev = *(scanner.fmt_cur - 1);
            if (prev == '[') {
              continue;
            }
            // If '^' is the first char, it's a negation, so '[' is a second char
            // not an end
            if (prev == '^' and *(scanner.fmt_cur - 2) == '[') {
              continue;
            }
            // Otherwise we're done
            if (scanner.skip) {
              read_count = scanner.scan();
            } else {
              dst_ptr = va_arg(ap, char *);
              read_count = scanner.scan((char *)dst_ptr);
            }
            in_scanset = false;
            end_fmt = true;
            continue;
          } else {
            // If we haven't hit the end of the scanset, just continue
            continue;
          }
        }
        switch (c) {
          case 'd':
          case 'i':
          case 'o':
          case 'u':
          case 'x':
          case 'X':
            if (scanner.skip) {
              read_count = scanner.scan();
            } else {
              switch (*(scanner.fmt_cur - 1)) {
                case 'h':
                  // Also covers the 'hh' case (since the size of the arg is
                  // still an int).
                  dst_ptr = va_arg(ap, int *);
                  read_count = scanner.scan((int *)dst_ptr);
                  write_size = sizeof(int);
                  break;
                case 'l':
                  if (scanner.fmt_cur - scanner.fmt_start >= 2 &&
                      *(scanner.fmt_cur - 2) == 'l') {
                    dst_ptr = va_arg(ap, long long int *);
                    read_count = scanner.scan((long long int *)dst_ptr);
                    write_size = sizeof(long long int);
                  } else {
                    dst_ptr = va_arg(ap, long int *);
                    read_count = scanner.scan((long int *)dst_ptr);
                    write_size = sizeof(long int);
                  }
                  break;
                case 'q':
                  dst_ptr = va_arg(ap, long long int *);
                  read_count = scanner.scan((long long int *)dst_ptr);
                  write_size = sizeof(long long int);
                  break;
                case 'j':
                  dst_ptr = va_arg(ap, INTMAX_T *);
                  read_count = scanner.scan((INTMAX_T *)dst_ptr);
                  write_size = sizeof(INTMAX_T);
                  break;
                case 'z':
                case 't':
                  dst_ptr = va_arg(ap, SIZE_T *);
                  read_count = scanner.scan((SIZE_T *)dst_ptr);
                  write_size = sizeof(SIZE_T);
                  break;
                default:
                  dst_ptr = va_arg(ap, int *);
                  read_count = scanner.scan((int *)dst_ptr);
                  write_size = sizeof(int);
              }
            }
            end_fmt = true;

            break;

          case 'a':
          case 'A':
          case 'e':
          case 'E':
          case 'f':
          case 'F':
          case 'g':
          case 'G':
            if (scanner.skip) {
              read_count = scanner.scan();
            } else {
              if (*(scanner.fmt_cur - 1) == 'L') {
                dst_ptr = va_arg(ap, long double *);
                read_count = scanner.scan((long double *)dst_ptr);
                write_size = sizeof(long double);
              } else if (*(scanner.fmt_cur - 1) == 'l') {
                dst_ptr = va_arg(ap, double *);
                read_count = scanner.scan((double *)dst_ptr);
                write_size = sizeof(double);
              } else {
                dst_ptr = va_arg(ap, float *);
                read_count = scanner.scan((float *)dst_ptr);
                write_size = sizeof(float);
              }
            }
            end_fmt = true;
            break;

          case 'c':
            if (scanner.skip) {
              read_count = scanner.scan();
            } else {
              dst_ptr = va_arg(ap, char *);
              read_count = scanner.scan((char *)dst_ptr);
              write_size = sizeof(char);
            }
            end_fmt = true;
            break;

          case 's': {
            if (scanner.skip) {
              read_count = scanner.scan();
            } else {
              dst_ptr = va_arg(ap, char *);
              read_count = scanner.scan((char *)dst_ptr);
              if (1 == read_count) {
                // special case: we have parsed a single string and we need to
                // update read_count with the string size
                read_count = internal_strlen((char *)dst_ptr);
              }
            }
            end_fmt = true;
            break;
          }

          case 'p':
            if (scanner.skip) {
              read_count = scanner.scan();
            } else {
              dst_ptr = va_arg(ap, void *);
              read_count =
                  scanner.scan((int *)dst_ptr);  // note: changing void* to int*
                                                 // since we need to call sizeof
              write_size = sizeof(int);
            }
            end_fmt = true;
            break;

          case 'n': {
            if (!scanner.skip) {
              int *ptr = stripID(va_arg(ap, int *));
              *ptr = (int)scanner.str_off;
            }
            end_fmt = true;
            break;
          }

          case '%':
            read_count = scanner.scan();
            end_fmt = true;
            break;

          case '*':
            scanner.skip = true;
            break;

          case '[':
            in_scanset = true;
            break;

          default:
            break;
        }
      }
    }

    //Printf("Read %d characters\n", read_count);

    if (read_count == EOF) {
      if (hasMatched) {
        return scanner.num_scanned;
      } else {
        return EOF;
      }
    }

    hasMatched = true;
    scanner.fmt_cur++;
    scanner.str_off += read_count;
  }

  // Number of items scanned in total.
  return scanner.num_scanned;
}

////////////////////////
// vprintf family
///////////////////////

// Defintions

DECLARE_REAL(SIZE_T, fwrite, const void *p, uptr size, uptr nmemb, void *file)

int __ptsan_vfprintf(FILE *stream, const char *format, va_list ap) {
  char str[4096];
  InLibcGuard _ig;
  int ret = format_buffer(str, 4096, format, ap);


  if (ret < 0) {
    return ret;
  }

  CHECK(ret < 4096);

  size_t written = REAL(fwrite)(str, 1, ret, stream);

  CHECK((size_t)ret == written);
  return ret;
}

int __ptsan_vprintf(const char *format, va_list ap) {
  return __ptsan_vfprintf(stdout, format, ap);
}


int __ptsan_vsprintf(char *str, const char *format, va_list ap) {
  InLibcGuard _ig;
  int ret = format_buffer(str, INT32_MAX, format, ap);
  return ret;
}

int __ptsan_vsnprintf(char *str, SIZE_T size, const char *format, va_list ap) {
  InLibcGuard _ig;
  int ret = format_buffer(str, size, format, ap);
  return ret;
}

int __ptsan_vasprintf(char **strp_tagged, const char *format, va_list ap) {
  CHECK(internal_strlen(format) < 512);
  char **strp = clear_id(strp_tagged);
  // XXX: Figure this out correctly instead of this BS
  // Should give a tagged malloc
  *strp = (char*)malloc(10000);

  if (!*strp) {
    return -1;
  }

  CHECK(get_id(*strp) != 0);

  int ret = __ptsan_vsnprintf(*clear_id(strp), 10000, format, ap);
  CHECK(ret < 10000);
  return ret;
}


// Interceptors
INTERCEPTOR(int, vprintf, const char *format, va_list ap) {
  return __ptsan_vprintf(format, stripVaListID(ap));
}

INTERCEPTOR(int, vfprintf, FILE *stream, const char *format, va_list ap) {
  return __ptsan_vfprintf(stream, format, stripVaListID(ap));
}

INTERCEPTOR(int, vsprintf, char *str, const char *format, va_list ap) {
  return __ptsan_vsprintf(str, format, stripVaListID(ap));
}

INTERCEPTOR(int, vsnprintf, char *str, SIZE_T size, const char *format, va_list ap) {
  return __ptsan_vsnprintf(str, size, format, stripVaListID(ap));
}

// XXX: Properly calculate the desired size
INTERCEPTOR(int, vasprintf, char **strp, const char *format, va_list ap) {
  return __ptsan_vasprintf(strp, format, stripVaListID(ap));
}

////////////////////////
// printf family
///////////////////////

INTERCEPTOR(int, printf, const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  int ret = __ptsan_vprintf(format, ap);
  va_end(ap);
  return ret;
}

INTERCEPTOR(int, fprintf, FILE *stream, const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  int ret = __ptsan_vfprintf(stream, format, ap);
  va_end(ap);
  return ret;
}

INTERCEPTOR(int, sprintf, char *str, const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  int ret = __ptsan_vsprintf(str, format, ap);
  va_end(ap);
  return ret;
}


INTERCEPTOR(int, asprintf, char **strp, const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  int ret = __ptsan_vasprintf(strp, format, ap);
  va_end(ap);
  return ret;
}

INTERCEPTOR(int, snprintf, char *str, SIZE_T size, const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  int ret = __ptsan_vsnprintf(str, size, format, ap);
  va_end(ap);
  return ret;
}

////////////////////////
// scanf family
///////////////////////

#define SCANF_BODY(src, fn)                                                    \
  va_list ap;                                                                  \
  va_start(ap, format);                                                        \
  InLibcGuard _ig;                                                             \
  int ret = scan_buffer(src, REAL(fn), format, ap);                            \
  va_end(ap);                                                                  \
  return ret;

INTERCEPTOR(int, sscanf, const char *str, const char *format, ...) {
  SCANF_BODY(str, sscanf);
}

INTERCEPTOR(int, __isoc99_sscanf, const char *str, const char *format, ...) {
  SCANF_BODY(str, __isoc99_sscanf);
}

INTERCEPTOR(int, __isoc23_sscanf, const char *str, const char *format, ...) {
  SCANF_BODY(str, __isoc23_sscanf);
}

INTERCEPTOR(int, fscanf, FILE *F, const char *format, ...) {
  SCANF_BODY(F, fscanf);
}

INTERCEPTOR(int, __isoc99_fscanf, FILE *F, const char *format, ...) {
  SCANF_BODY(F, __isoc99_fscanf);
}

INTERCEPTOR(int, __isoc23_fscanf, FILE *F, const char *format, ...) {
  SCANF_BODY(F, __isoc23_fscanf);
}

INTERCEPTOR(int, scanf, const char *format, ...) {
  SCANF_BODY(get_stdin(), fscanf);
}

INTERCEPTOR(int, __isoc99_scanf, const char *format, ...) {
  SCANF_BODY(get_stdin(), __isoc99_fscanf);
}

INTERCEPTOR(int, __isoc23_scanf, const char *format, ...) {
  SCANF_BODY(get_stdin(), __isoc23_fscanf);
}


////////////////////////
// vscanf family
///////////////////////

#define VSCANF_BODY(src, fn)                                                   \
  InLibcGuard _ig;                                                             \
  int ret = scan_buffer(src, REAL(fn), format, stripVaListID(ap));             \
  return ret;

INTERCEPTOR(int, vsscanf, const char *str, const char *format, va_list ap) {
  VSCANF_BODY(str, sscanf);
}

INTERCEPTOR(int, __isoc99_vsscanf, const char *str, const char *format, va_list ap) {
  VSCANF_BODY(str, __isoc99_sscanf);
}

INTERCEPTOR(int, __isoc23_vsscanf, const char *str, const char *format, va_list ap) {
  VSCANF_BODY(str, __isoc23_sscanf);
}

INTERCEPTOR(int, vfscanf, FILE *F, const char *format, va_list ap) {
  VSCANF_BODY(F, fscanf);
}

INTERCEPTOR(int, __isoc99_vfscanf, FILE *F, const char *format, va_list ap) {
  VSCANF_BODY(F, __isoc99_fscanf);
}

INTERCEPTOR(int, __isoc23_vfscanf, FILE *F, const char *format, va_list ap) {
  VSCANF_BODY(F, __isoc23_fscanf);
}

INTERCEPTOR(int, vscanf, const char *format, va_list ap) {
  VSCANF_BODY(get_stdin(), fscanf);
}

INTERCEPTOR(int, __isoc99_vscanf, const char *format, va_list ap) {
  VSCANF_BODY(get_stdin(), __isoc99_fscanf);
}

INTERCEPTOR(int, __isoc23_vscanf, const char *format, va_list ap) {
  VSCANF_BODY(get_stdin(), __isoc23_fscanf);
}


// FIXME: support *wprintf in common format interceptors.
INTERCEPTOR(int, vswprintf, void *str, uptr size, void *format, va_list ap) {
  Printf("Implement vswpritf!\n");
  Die();
}

INTERCEPTOR(int, swprintf, void *str, uptr size, void *format, ...) {
  Printf("Implement swpritf!\n");
  Die();
}

void __ptsan::InitializeFormatInterceptors() {
  INTERCEPT_FUNCTION(vprintf);
  INTERCEPT_FUNCTION(vfprintf);
  INTERCEPT_FUNCTION(vsprintf);
  INTERCEPT_FUNCTION(vsnprintf);
  INTERCEPT_FUNCTION(vasprintf);
  INTERCEPT_FUNCTION(printf);
  INTERCEPT_FUNCTION(fprintf);
  INTERCEPT_FUNCTION(sprintf);
  INTERCEPT_FUNCTION(snprintf);
  INTERCEPT_FUNCTION(asprintf);

  // sscanf family
  INTERCEPT_FUNCTION(sscanf);
  INTERCEPT_FUNCTION(__isoc99_sscanf);
  INTERCEPT_FUNCTION(__isoc23_sscanf);

  // fscanf family
  INTERCEPT_FUNCTION(fscanf);
  INTERCEPT_FUNCTION(__isoc99_fscanf);
  INTERCEPT_FUNCTION(__isoc23_fscanf);

  // scanf family
  INTERCEPT_FUNCTION(scanf);
  INTERCEPT_FUNCTION(__isoc99_scanf);
  INTERCEPT_FUNCTION(__isoc23_scanf);

  // vsscanf family
  INTERCEPT_FUNCTION(vsscanf);
  INTERCEPT_FUNCTION(__isoc99_vsscanf);
  INTERCEPT_FUNCTION(__isoc23_vsscanf);

  // vfscanf family
  INTERCEPT_FUNCTION(vfscanf);
  INTERCEPT_FUNCTION(__isoc99_vfscanf);
  INTERCEPT_FUNCTION(__isoc23_vfscanf);

  // vscanf family
  INTERCEPT_FUNCTION(vscanf);
  INTERCEPT_FUNCTION(__isoc99_vscanf);
  INTERCEPT_FUNCTION(__isoc23_vscanf);
}
