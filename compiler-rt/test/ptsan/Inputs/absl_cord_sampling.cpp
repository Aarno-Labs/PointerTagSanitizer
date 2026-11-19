//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Integration reproducer: link against matching Abseil Cord/stacktrace libraries.
// Force sampling so a short successful run cannot miss the native frame walker.
#include "absl/strings/cord.h"
#include "absl/strings/internal/cordz_functions.h"
#include <assert.h>
#include <string>

int main() {
  absl::cord_internal::set_cordz_mean_interval(1);
  assert(absl::cord_internal::get_cordz_mean_interval() == 1);
  for (int i = 0; i != 128; ++i) {
    std::string input(4096 + i, 'a' + i % 26);
    absl::Cord cord(input);
    cord.Append(input);
    assert(cord.size() == input.size() * 2);
    assert(std::string(cord) == input + input);
  }
}
