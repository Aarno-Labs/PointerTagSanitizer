// Check that the selected runtime and C++ dependencies use the requested ID width.
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" uint32_t ptsan_id_bits(void);
int main() {
  assert(ptsan_id_bits() == PTSAN_EXPECTED_ID_BITS);
  std::vector<std::string> values(8, "PTSan dependency smoke test");
  assert(values.at(7) == values.at(0));
  try {
    throw std::runtime_error(values.at(0));
  } catch (const std::runtime_error &error) {
    assert(std::string(error.what()) == values.at(0));
  }
}
