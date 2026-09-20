#include "../src/bridge_policy.h"
#include <assert.h>

int main() {
  const uint8_t a[6] = {1, 2, 3, 4, 5, 6};
  const uint8_t b[6] = {1, 2, 3, 4, 5, 7};
  assert(bridge_source_allowed(false, b, a));
  assert(bridge_source_allowed(true, a, a));
  assert(!bridge_source_allowed(true, b, a));

  assert(bridge_sequence_is_newer(0, 0xffffffffu));
  assert(!bridge_sequence_is_newer(4, 4));
  assert(!bridge_sequence_is_newer(3, 4));

  assert(bridge_queue_full<4>(3, 0));
  assert(!bridge_queue_full<4>(2, 0));
  assert(bridge_use_realtime(0, true, true));
  assert(!bridge_use_realtime(4, true, true));
  assert(bridge_use_realtime(4, true, false));
  assert(!bridge_usb_write_commit(false));
  assert(bridge_usb_write_commit(true));
  BridgeTxPolicy tx(2);
  assert(tx.try_accept(false, false) == BridgeTxDecision::ImmediateRetry);
  assert(tx.try_accept(false, false) == BridgeTxDecision::ImmediateRetry);
  assert(tx.try_accept(false, false) == BridgeTxDecision::Drop);
  assert(tx.try_accept(false, true) == BridgeTxDecision::Accepted);
  assert(tx.try_accept(true, true) == BridgeTxDecision::Idle);

  BridgePanicPolicy panic;
  assert(panic.timeout_transition(true, true));
  assert(!panic.timeout_transition(true, true));
  panic.reconnected();
  assert(panic.timeout_transition(true, true));
  assert(panic.new_session(42));
  assert(!panic.new_session(42));
  assert(panic.new_session(43));
  panic.reset();
  assert(panic.new_session(1));
  return 0;
}
