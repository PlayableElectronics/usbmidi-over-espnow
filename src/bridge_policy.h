#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

inline bool bridge_mac_equal(const uint8_t *a, const uint8_t *b) {
  return a && b && memcmp(a, b, 6) == 0;
}

inline bool bridge_source_allowed(bool paired, const uint8_t *source,
                                  const uint8_t *peer) {
  return !paired || bridge_mac_equal(source, peer);
}

inline bool bridge_sequence_is_newer(uint32_t candidate, uint32_t previous) {
  uint32_t delta = candidate - previous;
  return delta != 0 && delta < 0x80000000u;
}

template <size_t N>
inline bool bridge_queue_full(size_t head, size_t tail) {
  return ((head + 1) % N) == tail;
}

inline bool bridge_use_realtime(uint8_t consecutive_realtime, bool realtime_available,
                                bool normal_available) {
  return realtime_available && (consecutive_realtime < 4 || !normal_available);
}

inline bool bridge_usb_write_commit(bool write_accepted) {
  return write_accepted;
}

enum class BridgeTxDecision : uint8_t { Idle, Accepted, ImmediateRetry, Drop };

class BridgeTxPolicy {
 public:
  explicit BridgeTxPolicy(uint8_t max_retries = 3) : max_retries_(max_retries) {}

  BridgeTxDecision try_accept(bool in_flight, bool send_accepted) {
    if (in_flight) return BridgeTxDecision::Idle;
    if (send_accepted) {
      retries_ = 0;
      return BridgeTxDecision::Accepted;
    }
    if (retries_ < max_retries_) {
      ++retries_;
      return BridgeTxDecision::ImmediateRetry;
    }
    retries_ = 0;
    return BridgeTxDecision::Drop;
  }

  uint8_t retries() const { return retries_; }

 private:
  uint8_t max_retries_;
  uint8_t retries_ = 0;
};

class BridgePanicPolicy {
 public:
  bool timeout_transition(bool connected, bool timed_out) {
    if (!connected || !timed_out || timeout_seen_) return false;
    timeout_seen_ = true;
    return true;
  }

  bool new_session(uint32_t session) {
    if (session == 0 || session == last_session_) return false;
    last_session_ = session;
    return true;
  }

  void reconnected() { timeout_seen_ = false; }
  void reset() { timeout_seen_ = false; last_session_ = 0; }

 private:
  bool timeout_seen_ = false;
  uint32_t last_session_ = 0;
};
