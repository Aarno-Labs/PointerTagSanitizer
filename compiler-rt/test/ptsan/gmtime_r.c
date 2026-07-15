// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -I%crt_src/include -I%crt_src/lib -o %exe %s
// RUN: ./%exe > out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=OUT
// RUN: ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR

// OUT: Size: 56
// OUT: Got date:
// ERR-NOT: Found bad
// ERR: Unreclaimed IDs at exit: 0
// Thanks, chatgpt!

#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>


#define get_id(ptr) \
  (uint16_t)((uint64_t)ptr >> 48)

extern uint64_t ptsan_get_bound(uint16_t id);
extern uint64_t ptsan_get_base(uint16_t id);
extern uint64_t ptsan_get_size(uint16_t id);

/* Drop-in replacements for the MHD helpers */
static size_t MHD_uint8_to_str_pad(uint8_t v, unsigned width,
                                   char *buf, size_t buflen) {
  if (buflen == 0) { return 0; }
  int n = snprintf(buf, buflen, "%0*u", width, (unsigned)v);
  return n < 0 ? 0u : (size_t)n;  // returns digits written (no NUL)
}

static size_t MHD_uint16_to_str(uint16_t v,
                                char *buf, size_t buflen) {
  if (buflen == 0) { return 0; }
  int n = snprintf(buf, buflen, "%u", (unsigned)v);
  return n < 0 ? 0u : (size_t)n;  // returns digits written (no NUL)
}


bool get_date_str (char *date)
{
  static const char *const days[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
  };
  static const char *const mons[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  };
  static const size_t buf_len = 29;
  struct tm now;
  time_t t;
  const char *src;

  if ((time_t) -1 == time (&t))
    return false;

  struct tm *nptr = &now;

  uint16_t id = get_id(nptr);
  printf("ID: %u\n", id);
  printf("Base: %lu\n", ptsan_get_base(id));
  printf("Bound: %lu\n", ptsan_get_bound(id));
  printf("Size: %lu\n", ptsan_get_size(id));

  if (NULL == gmtime_r (&t, nptr))
    return false;

  /* Day of the week */
  src = days[now.tm_wday % 7];
  date[0] = src[0];
  date[1] = src[1];
  date[2] = src[2];
  date[3] = ',';
  date[4] = ' ';
  /* Day of the month */
  if (2 != MHD_uint8_to_str_pad ((uint8_t) now.tm_mday, 2,
                                 date + 5, buf_len - 5))
    return false;
  date[7] = ' ';
  /* Month */
  src = mons[now.tm_mon % 12];
  date[8] = src[0];
  date[9] = src[1];
  date[10] = src[2];
  date[11] = ' ';
  /* Year */
  if (4 != MHD_uint16_to_str ((uint16_t) (1900 + now.tm_year), date + 12,
                              buf_len - 12))
    return false;
  date[16] = ' ';
  /* Time */
  MHD_uint8_to_str_pad ((uint8_t) now.tm_hour, 2, date + 17, buf_len - 17);
  date[19] = ':';
  MHD_uint8_to_str_pad ((uint8_t) now.tm_min, 2, date + 20, buf_len - 20);
  date[22] = ':';
  MHD_uint8_to_str_pad ((uint8_t) now.tm_sec, 2, date + 23, buf_len - 23);
  date[25] = ' ';
  date[26] = 'G';
  date[27] = 'M';
  date[28] = 'T';

  return true;
}

int main(void) {
  char date[50];
  if (get_date_str(date)) {
    printf("Got date: %s\n", date);
  } else {
    printf("get_date_str failed\n");
  }
  return 0;
}