// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clangxx_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> out.txt
// RUN: cat out.txt | FileCheck %s

// CHECK: Found bad

// test_inv_map.cpp ---------------------------------------------------------
// Unit-test harness for Mesh::initInvMap()

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

namespace Memory {
template <typename T>
inline T *alloc(int n) { return static_cast<T *>(std::malloc(n * sizeof(T))); }
} // namespace Memory

struct Mesh {
  /* geometry */
  int nump;  // number of points
  int numc;  // number of cells
  int nums;  // == numc for this simple test

  /* input  */
  int *mapsp1;      // size == numc  (cell → point 1)
  /* outputs */
  int *mappcfirst;  // size == nump  (point → first cell)
  int *mapccnext;   // size == nums  (cell  → next cell with same point)

  /* --------------------------------------------------------------------- */
  __attribute__((noinline))
  void initInvMap() {
    mappcfirst = Memory::alloc<int>(nump);
    mapccnext  = Memory::alloc<int>(nums);

    std::vector<std::pair<int, int>> pcpair(nums);
    for (int c = 0; c < numc; ++c)
      pcpair[c] = std::make_pair(mapsp1[c], c);

    std::sort(pcpair.begin(), pcpair.end());

    for (int i = 0; i < numc; ++i) {
      int p  = pcpair[i].first;
      int pp = pcpair[i + 1].first;
      int pm = (i == 0) ? 0 : pcpair[i - 1].first;
      int c  = pcpair[i].second;
      int cp = pcpair[i + 1].second;

      if (i == 0 || p != pm) mappcfirst[p] = c;
      mapccnext[c] = (i + 1 == numc || p != pp) ? -1 : cp;
    }
  }
};

__attribute__((noinline))
void run_test() {
  /* Example mesh: 3 points, 5 cells
     cell -> point mapping = {1,0,1,2,1}  */
  Mesh M;
  M.nump = 3;
  M.numc = 5;
  M.nums = 5;
  int cell_to_point[5] = {1, 0, 1, 2, 1};
  M.mapsp1 = cell_to_point;

  M.initInvMap();

  /* expected results */
  const int exp_first[3] = {1, 0, 3};
  const int exp_next[5]  = {2, -1, 4, -1, -1};

  for (int p = 0; p < M.nump; ++p)
    assert(M.mappcfirst[p] == exp_first[p]);

  for (int c = 0; c < M.nums; ++c)
    assert(M.mapccnext[c] == exp_next[c]);

  std::cout << "initInvMap() passed\n";

  std::free(M.mappcfirst);
  std::free(M.mapccnext);
}

int main() {
  run_test();
  return 0;
}
