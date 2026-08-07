#pragma once

// Per-vector test executor: configure the target over CDC, drive the bus,
// read back and verify.

#include <cstdint>

#include "testrig/jsonclient.hpp"
#include "testrig/vectors.hpp"

namespace testrig {

struct ExecResult {
  bool pass = false;
  const char* stage = "";      // failed stage label ("" when passed)
  uint32_t mismatchCount = 0;  // pixel/text mismatches
  uint16_t badX = 0, badY = 0;
  uint16_t got = 0, want = 0;
};

// Progress callback: percent 0..100; return false to cancel the vector.
using ExecProgressFn = bool (*)(void* ctx, int percent);

// Runs one vector start to finish (blocking). Returns res->pass.
bool executorRunVector(const TestVector& vec, JsonClient& client,
                       ExecResult* res, ExecProgressFn progress, void* ctx);

}  // namespace testrig
