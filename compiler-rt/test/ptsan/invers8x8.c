// XFAIL: *
// RUN: %clang_ptsan -g -O2 -o -march=native %exe %s
// This will fail on an assertion that we don't support masked_gather
// In order to support masked_gather, we would need to:
// - Vectorize stripping ids  (easy)
// - Vectorize getting ids    (easy)
// - Vectorize checking       (harder)
// - Figure out that the vectorization of this is still optimizable (????)

// Mostly that last one is a sticking point. You can disable this kind of vectorization
// with `-mno-avx2`

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define BLOCK_SIZE_8x8 8

/* Prototype of the function under test (same signature) */
__attribute__((noinline))
void inverse8x8(int **tblock, int **block, int pos_y, int pos_x)
{
  int i, ii;
  int tmp[64];
  int *pTmp = tmp, *pblock;
  int a0, a1, a2, a3;
  int p0, p1, p2, p3, p4, p5 ,p6, p7;
  int b0, b1, b2, b3, b4, b5, b6, b7;

  // Horizontal
  for (i=pos_y; i < pos_y + BLOCK_SIZE_8x8; i++)
  {
    pblock = &tblock[i][pos_x];
    p0 = *(pblock++);
    p1 = *(pblock++);
    p2 = *(pblock++);
    p3 = *(pblock++);
    p4 = *(pblock++);
    p5 = *(pblock++);
    p6 = *(pblock++);
    p7 = *(pblock  );

    a0 = p0 + p4;
    a1 = p0 - p4;
    a2 = p6 - (p2 >> 1);
    a3 = p2 + (p6 >> 1);

    b0 =  a0 + a3;
    b2 =  a1 - a2;
    b4 =  a1 + a2;
    b6 =  a0 - a3;

    a0 = -p3 + p5 - p7 - (p7 >> 1);
    a1 =  p1 + p7 - p3 - (p3 >> 1);
    a2 = -p1 + p7 + p5 + (p5 >> 1);
    a3 =  p3 + p5 + p1 + (p1 >> 1);


    b1 =  a0 + (a3>>2);
    b3 =  a1 + (a2>>2);
    b5 =  a2 - (a1>>2);
    b7 =  a3 - (a0>>2);

    *(pTmp++) = b0 + b7;
    *(pTmp++) = b2 - b5;
    *(pTmp++) = b4 + b3;
    *(pTmp++) = b6 + b1;
    *(pTmp++) = b6 - b1;
    *(pTmp++) = b4 - b3;
    *(pTmp++) = b2 + b5;
    *(pTmp++) = b0 - b7;
  }
}

/* ── helpers ─────────────────────────────────────────── */
static void dump_8x8(const char *title, int **buf,
                     int pos_y, int pos_x)
{
  printf("%s @ (%d,%d)\n", title, pos_y, pos_x);
  for (int y = 0; y < BLOCK_SIZE_8x8; ++y) {
    for (int x = 0; x < BLOCK_SIZE_8x8; ++x)
      printf("%6d", buf[pos_y + y][pos_x + x]);
    putchar('\n');
  }
  puts("");
}

/* ── test driver ─────────────────────────────────────── */
int main(void)
{
  /* allocate a 16×16 int matrix on the heap and build pointer rows */
  const int SIZE = 16;
  int  *raw   = malloc(sizeof(int) * SIZE * SIZE);
  int **rows  = malloc(sizeof(int*) * SIZE);
  for (int y = 0; y < SIZE; ++y)
    rows[y] = raw + y * SIZE;

  /* second buffer (the destination) */
  int  *raw2  = calloc(SIZE * SIZE, sizeof(int));
  int **rows2 = malloc(sizeof(int*) * SIZE);
  for (int y = 0; y < SIZE; ++y)
    rows2[y] = raw2 + y * SIZE;

  /* fill source with a reproducible pattern */
  for (int y = 0; y < SIZE; ++y)
    for (int x = 0; x < SIZE; ++x)
      rows[y][x] = (y * SIZE + x) % 97 - 48;   /* some signed values */

  /* top-left 8×8 ------------------------------------------------ */
  dump_8x8("Input (0,0)", rows, 0, 0);
  inverse8x8(rows, rows2, 0, 0);
  dump_8x8("Output (0,0)", rows2, 0, 0);

  /* bottom-right 8×8 ------------------------------------------- */
  dump_8x8("Input (8,8)", rows, 8, 8);
  inverse8x8(rows, rows2, 8, 8);
  dump_8x8("Output (8,8)", rows2, 8, 8);

  free(raw);   free(rows);
  free(raw2);  free(rows2);
  return 0;
}
