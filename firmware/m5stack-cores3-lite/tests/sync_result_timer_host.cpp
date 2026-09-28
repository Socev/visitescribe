#include <cassert>
#include <cstdio>
#include "../src/sync_result_timer.h"
int main() {
  VsSyncResultTimer t;
  assert(!t.update(false, 0));
  assert(!t.update(true, 100));
  assert(!t.update(true, 10099));
  assert(t.update(true, 10100));
  assert(!t.update(false, 10101));
  assert(!t.update(true, 50000));
  assert(!t.update(false, 55000)); // leave result, error, or active sync resets
  assert(!t.update(true, 60000));
  assert(!t.update(true, 69999));
  assert(t.update(true, 70000));
  t = VsSyncResultTimer();
  assert(!t.update(true, UINT32_MAX - 5000));
  assert(!t.update(true, 4998));
  assert(t.update(true, 4999));
  puts("PASS: sync result returns at 10s, resets on ineligible state, fresh deadline on new result, clock wrap.");
}
