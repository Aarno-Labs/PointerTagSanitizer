// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsanxx_musl_nonstrict -o %t %s
// RUN: %run %t

#include <cassert>
#include <numeric>
#include <string>
#include <vector>

int main() {
  std::vector<int> values{1, 2, 3, 4};
  std::string message = "PTSan";
  assert(std::accumulate(values.begin(), values.end(), 0) == 10);
  assert(message.size() == 5);
}
