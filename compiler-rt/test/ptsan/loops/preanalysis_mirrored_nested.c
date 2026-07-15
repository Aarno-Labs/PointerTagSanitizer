// RUN: %clang_raw -fsanitize=ptsan -g -O3 -fno-discard-value-names -mllvm -ptsan-runtime-validate -mllvm -ptsan-print-stats -S -emit-llvm -o - %s | FileCheck %s --check-prefix=IR --implicit-check-not=SimpleAccessRange
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -g -O3 -fno-discard-value-names -mllvm -ptsan-runtime-validate -mllvm -ptsan-print-stats -o %exe %s %S/../Inputs/preanalysis_obsequi_globals.c
// RUN: ./%exe 8 2> err && FileCheck %s --check-prefix=OUT < err

// IR: PREANALYZED_LOOP
// IR: LoopAccessRange

// OUT-NOT: Found bad
// OUT: Checks called:
// OUT: PREANALYZED_LOOP: 8

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern int g_first_move[2][32][32];
extern int g_board_size[2];
volatile int sink;

static void set_position_value(int player, int row, int col, int value) {
  int num_rows = g_board_size[player] + 1;
  int num_cols = g_board_size[player ^ 1];

  g_first_move[player][row][col] = value;
  g_first_move[player][num_rows - row][col] = value;
  g_first_move[player][num_rows - row][num_cols - col] = value;
  g_first_move[player][row][num_cols - col] = value;
}

__attribute__((noinline))
void set_position_values(void) {
  for (int i = 0; i < 2; i++)
    for (int j = 0; j < 32; j++)
      for (int k = 0; k < 32; k++)
        g_first_move[i][j][k] = 0;

  for (int i = 0; i < 2; i++) {
    int count = 127;
    for (int j = 2; j < (g_board_size[i] + 3) / 2; j += 2)
      for (int k = 1; k < (g_board_size[i ^ 1] / 2) + 1; k += 2)
        set_position_value(i, j, k, --count);
  }
}

int main(int argc, char **argv) {
  int size = argc > 1 ? atoi(argv[1]) : 8;
  g_board_size[0] = size;
  g_board_size[1] = size;
  set_position_values();
  sink = g_first_move[0][2][1] + g_first_move[1][2][1];
  fprintf(stderr, "%d\n", sink);
  return 0;
}
