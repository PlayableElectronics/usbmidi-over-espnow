#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <USB.h>
#include "esp32-hal-tinyusb.h"
#include "bridge_policy.h"
#include "tusb.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <array>
#include <atomic>
#include <cstring>

namespace {
constexpr uint8_t kButtonPin = 41;
constexpr uint8_t kRgbPin = 35;
constexpr uint8_t kWifiChannel = 6;
constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kMagic[4] = {'E', 'N', 'M', '1'};
constexpr uint8_t kBroadcastMac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
constexpr size_t kHeaderSize = 18;
constexpr size_t kRadioLimit = ESP_NOW_MAX_DATA_LEN;
constexpr size_t kMaxPayload = kRadioLimit - kHeaderSize;
constexpr size_t kRadioQueueDepth = 32;
constexpr size_t kUsbQueueDepth = 64;
constexpr uint32_t kDiscoveryPeriodMs = 500;
constexpr uint32_t kPeerTimeoutMs = 2500;
constexpr uint32_t kLongPressMs = 1500;
constexpr uint32_t kSysexTimeoutMs = 1000;
constexpr uint32_t kMaxSysexBytes = 65535;
constexpr uint32_t kTxRetryDelayMs = 2;
constexpr uint8_t kMaxImmediateRetries = 3;

enum MessageType : uint8_t { MSG_DISCOVERY = 1, MSG_DISCOVERY_ACK = 2, MSG_MIDI = 3 };

struct MidiEvent {
  struct {
    uint8_t header = 0;
    uint8_t byte1 = 0;
    uint8_t byte2 = 0;
    uint8_t byte3 = 0;
  } packet;
  uint32_t received_at = 0;
  bool forward = true;
};

struct RadioFrame {
  uint8_t data[kRadioLimit]{};
  size_t length = 0;
  uint8_t source_mac[6]{};
};

struct Counters {
  std::atomic<uint32_t> received{0};
  std::atomic<uint32_t> duplicate{0};
  std::atomic<uint32_t> malformed{0};
  std::atomic<uint32_t> sequence_gaps{0};
  std::atomic<uint32_t> queue_drops{0};
  std::atomic<uint32_t> usb_in{0};
  std::atomic<uint32_t> usb_out{0};
  std::atomic<uint32_t> radio_accepted{0};
  std::atomic<uint32_t> radio_immediate_failures{0};
  std::atomic<uint32_t> radio_async_failures{0};
  std::atomic<uint32_t> radio_retries{0};
  std::atomic<uint32_t> usb_busy{0};
  std::atomic<uint32_t> foreign_packets{0};
};

uint16_t midi_descriptor(uint8_t *dst, uint8_t *itf) {
  uint8_t str_index = tinyusb_add_string_descriptor("ESP-NOW MIDI Bridge");
  uint8_t ep_in = tinyusb_get_free_in_endpoint();
  uint8_t ep_out = tinyusb_get_free_out_endpoint();
  if (ep_out == 0 || ep_in == 0) return 0;
  uint8_t descriptor[TUD_MIDI_DESC_LEN] = {
      TUD_MIDI_DESCRIPTOR(*itf, str_index, ep_out, static_cast<uint8_t>(0x80 | ep_in), 64)};
  *itf += 2;
  std::memcpy(dst, descriptor, TUD_MIDI_DESC_LEN);
  return TUD_MIDI_DESC_LEN;
}

class UsbMidi {
 public:
  void begin() {
    if (tinyusb_enable_interface(USB_INTERFACE_MIDI, TUD_MIDI_DESC_LEN, midi_descriptor) != ESP_OK) {
      while (true) delay(1000);
    }
    USB.productName("ESP-NOW MIDI Bridge");
    USB.begin();
  }

  bool readPacket(decltype(MidiEvent::packet) *packet) {
    uint8_t raw[4]{};
    if (!tud_midi_packet_read(raw)) return false;
    packet->header = raw[0];
    packet->byte1 = raw[1];
    packet->byte2 = raw[2];
    packet->byte3 = raw[3];
    return true;
  }

  bool writePacket(const decltype(MidiEvent::packet) *packet) {
    uint8_t raw[4] = {packet->header, packet->byte1, packet->byte2, packet->byte3};
    return tud_midi_packet_write(raw);
  }
};

UsbMidi MIDI;
Adafruit_NeoPixel rgb(1, kRgbPin, NEO_GRB + NEO_KHZ800);
Preferences preferences;
Counters counters;

StaticQueue_t radio_queue_storage;
uint8_t radio_queue_storage_buffer[kRadioQueueDepth * sizeof(RadioFrame)] __attribute__((aligned(4)));
QueueHandle_t radio_queue_handle = nullptr;
std::array<MidiEvent, kUsbQueueDepth> remote_realtime_queue{};
size_t remote_realtime_head = 0;
size_t remote_realtime_tail = 0;
std::array<MidiEvent, kUsbQueueDepth> remote_normal_queue{};
size_t remote_normal_head = 0;
size_t remote_normal_tail = 0;
std::array<MidiEvent, kUsbQueueDepth> local_realtime_queue{};
size_t local_realtime_head = 0;
size_t local_realtime_tail = 0;
std::array<MidiEvent, kUsbQueueDepth> local_normal_queue{};
size_t local_normal_head = 0;
size_t local_normal_tail = 0;

uint8_t local_mac[6]{};
uint8_t peer_mac[6]{};
bool paired = false;
bool have_peer_sequence = false;
uint32_t peer_session = 0;
uint32_t last_peer_sequence = 0;
uint32_t session_id = 0;
uint32_t next_sequence = 0;
uint32_t last_peer_seen = 0;
uint32_t last_discovery = 0;
uint32_t led_pulse_until = 0;
uint32_t button_down_at = 0;
bool button_was_down = false;
bool sysex_active = false;
uint32_t sysex_last_at = 0;
uint32_t sysex_bytes = 0;
bool peer_timed_out = false;
bool timeout_panic_sent = false;
uint32_t last_panic_session = 0;
uint32_t pairing_reset_until = 0;

enum class TxResult : uint8_t { None, Success, Failure };
enum class TxKind : uint8_t { None, Midi, Discovery };
portMUX_TYPE tx_mux = portMUX_INITIALIZER_UNLOCKED;
volatile TxResult tx_result = TxResult::None;
volatile TxKind tx_kind = TxKind::None;
bool tx_in_flight = false;
uint8_t tx_immediate_retries = 0;
uint32_t tx_retry_after = 0;
uint8_t consecutive_realtime_sends = 0;

bool deadline_active(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(deadline - now) > 0;
}

uint32_t read_u32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

void write_u32(uint8_t *p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

bool mac_equal(const uint8_t *a, const uint8_t *b) { return bridge_mac_equal(a, b); }

bool mac_less(const uint8_t *a, const uint8_t *b) {
  return std::memcmp(a, b, 6) < 0;
}

bool queue_radio(const uint8_t *source_mac, const uint8_t *data, size_t length) {
  if (length > kRadioLimit || !radio_queue_handle) {
    counters.queue_drops++;
    return false;
  }
  RadioFrame frame{};
  std::memcpy(frame.source_mac, source_mac, sizeof(frame.source_mac));
  std::memcpy(frame.data, data, length);
  frame.length = length;
  if (xQueueSend(radio_queue_handle, &frame, 0) != pdTRUE) {
    counters.queue_drops++;
    return false;
  }
  return true;
}

bool pop_radio(RadioFrame &frame) {
  return radio_queue_handle && xQueueReceive(radio_queue_handle, &frame, 0) == pdTRUE;
}

template <size_t N>
bool queue_midi(std::array<MidiEvent, N> &queue, size_t &head, size_t tail,
                const MidiEvent &event) {
  size_t next = (head + 1) % N;
  if (bridge_queue_full<N>(head, tail)) {
    counters.queue_drops++;
    return false;
  }
  queue[head] = event;
  head = next;
  return true;
}

template <size_t N>
bool pop_midi(std::array<MidiEvent, N> &queue, size_t tail, size_t head,
              size_t &new_tail, MidiEvent &event) {
  if (tail == head) return false;
  event = queue[tail];
  new_tail = (tail + 1) % N;
  return true;
}

template <size_t N>
bool peek_midi(const std::array<MidiEvent, N> &queue, size_t tail, size_t head,
              MidiEvent &event) {
  if (tail == head) return false;
  event = queue[tail];
  return true;
}

template <size_t N>
bool drop_midi(const std::array<MidiEvent, N> &queue, size_t tail, size_t head,
              size_t &new_tail) {
  (void)queue;
  (void)head;
  if (tail == head) return false;
  new_tail = (tail + 1) % N;
  return true;
}

void pulse_led() { led_pulse_until = millis() + 30; }

void render_led() {
  uint32_t now = millis();
  bool lost = paired && (now - last_peer_seen > kPeerTimeoutMs);
  bool blink = lost ? ((now / 120) % 2 == 0) : (!paired && ((now / 600) % 2 == 0));
  bool pulse = deadline_active(now, led_pulse_until);
  uint8_t red = 0, green = 0;
  if (paired) {
    if (mac_less(local_mac, peer_mac)) green = pulse ? 160 : 64;
    else red = pulse ? 160 : 64;
  } else {
    red = green = 12;
    if (pulse) red = green = 96;
  }
  if (blink) red = green = 0;
  if (deadline_active(now, pairing_reset_until) && ((now / 80) % 2 == 0)) red = green = 64;
  rgb.setPixelColor(0, rgb.Color(red, green, 0));
  rgb.show();
}

bool add_peer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t info{};
  std::memcpy(info.peer_addr, mac, 6);
  info.channel = kWifiChannel;
  info.encrypt = false;
  return esp_now_add_peer(&info) == ESP_OK;
}

void save_peer() {
  preferences.putBytes("peer", peer_mac, sizeof(peer_mac));
  preferences.putBool("paired", true);
}

void clear_peer() {
  if (paired && esp_now_is_peer_exist(peer_mac)) esp_now_del_peer(peer_mac);
  preferences.clear();
  paired = false;
  have_peer_sequence = false;
  peer_session = 0;
  last_peer_sequence = 0;
  last_peer_seen = 0;
  peer_timed_out = false;
  timeout_panic_sent = false;
  last_panic_session = 0;
  portENTER_CRITICAL(&tx_mux);
  tx_in_flight = false;
  tx_result = TxResult::None;
  tx_kind = TxKind::None;
  portEXIT_CRITICAL(&tx_mux);
  tx_immediate_retries = 0;
  tx_retry_after = 0;
  consecutive_realtime_sends = 0;
  local_realtime_head = local_realtime_tail = 0;
  local_normal_head = local_normal_tail = 0;
  remote_realtime_head = remote_realtime_tail = 0;
  remote_normal_head = remote_normal_tail = 0;
  if (radio_queue_handle) xQueueReset(radio_queue_handle);
  std::memset(peer_mac, 0, sizeof(peer_mac));
}

bool build_packet(uint8_t type, const uint8_t *payload, size_t payload_length,
                  uint8_t *out, size_t &out_length) {
  if (payload_length > kMaxPayload) return false;
  std::memcpy(out, kMagic, 4);
  out[4] = kProtocolVersion;
  out[5] = type;
  out[6] = 0;
  out[7] = 0;
  write_u32(out + 8, session_id);
  write_u32(out + 12, next_sequence++);
  out[16] = static_cast<uint8_t>(payload_length >> 8);
  out[17] = static_cast<uint8_t>(payload_length);
  if (payload_length) std::memcpy(out + kHeaderSize, payload, payload_length);
  out_length = kHeaderSize + payload_length;
  return true;
}

void send_discovery(uint8_t type = MSG_DISCOVERY, const uint8_t *destination = nullptr) {
  if (tx_in_flight) return;
  uint8_t payload[10]{};
  std::memcpy(payload, local_mac, 6);
  write_u32(payload + 6, session_id);
  uint8_t packet[kRadioLimit]{};
  size_t length = 0;
  if (!build_packet(type, payload, sizeof(payload), packet, length)) return;
  const uint8_t *target = destination ? destination : kBroadcastMac;
  portENTER_CRITICAL(&tx_mux);
  tx_result = TxResult::None;
  tx_kind = TxKind::Discovery;
  tx_in_flight = true;
  portEXIT_CRITICAL(&tx_mux);
  if (esp_now_send(target, packet, length) != ESP_OK) {
    portENTER_CRITICAL(&tx_mux);
    tx_in_flight = false;
    tx_kind = TxKind::None;
    portEXIT_CRITICAL(&tx_mux);
    counters.radio_immediate_failures++;
  }
}

void queue_panic(bool forward) {
  for (uint8_t channel = 0; channel < 16; ++channel) {
    for (uint8_t controller : {static_cast<uint8_t>(123), static_cast<uint8_t>(120)}) {
      MidiEvent event{};
      event.packet.header = 0x0B;
      event.packet.byte1 = static_cast<uint8_t>(0xB0 | channel);
      event.packet.byte2 = controller;
      event.packet.byte3 = 0;
      event.forward = forward;
      // Always deliver the panic to the locally attached USB host. Only the
      // manual form is also placed on the outbound radio queue.
      queue_midi(remote_normal_queue, remote_normal_head, remote_normal_tail, event);
      if (forward) queue_midi(local_normal_queue, local_normal_head, local_normal_tail, event);
    }
  }
  pulse_led();
}

void on_send(const uint8_t *, esp_now_send_status_t status) {
  portENTER_CRITICAL(&tx_mux);
  if (tx_in_flight) tx_result = status == ESP_NOW_SEND_SUCCESS ? TxResult::Success : TxResult::Failure;
  portEXIT_CRITICAL(&tx_mux);
}

void on_receive(const uint8_t *source_mac, const uint8_t *data, int length) {
  if (!source_mac || !data || length <= 0 || length > static_cast<int>(kRadioLimit)) {
    counters.malformed++;
    return;
  }
  queue_radio(source_mac, data, static_cast<size_t>(length));
}

bool newer_sequence(uint32_t candidate, uint32_t previous) {
  return bridge_sequence_is_newer(candidate, previous);
}

void handle_discovery(const uint8_t *source_mac, uint8_t type, uint32_t session,
                      const uint8_t *payload, size_t payload_length) {
  if (payload_length != 10 || read_u32(payload + 6) != session || mac_equal(payload, local_mac)) {
    counters.malformed++;
    return;
  }
  if (!mac_equal(source_mac, payload)) {
    counters.foreign_packets++;
    return;
  }
  if (paired && !mac_equal(payload, peer_mac)) return;
  if (!paired) {
    std::memcpy(peer_mac, payload, 6);
    if (!add_peer(peer_mac)) return;
    paired = true;
    save_peer();
  }
  if (peer_session != 0 && peer_session != session && last_panic_session != session) {
    queue_panic(false);
    last_panic_session = session;
  }
  peer_session = session;
  last_peer_seen = millis();
  if (peer_timed_out) timeout_panic_sent = false;
  peer_timed_out = false;
  have_peer_sequence = false;
  if (type == MSG_DISCOVERY) send_discovery(MSG_DISCOVERY_ACK, peer_mac);
  pulse_led();
}

void handle_midi(const uint8_t *payload, size_t payload_length) {
  if (payload_length != 4) {
    counters.malformed++;
    return;
  }
  MidiEvent event{};
  event.packet.header = payload[0];
  event.packet.byte1 = payload[1];
  event.packet.byte2 = payload[2];
  event.packet.byte3 = payload[3];
  event.received_at = millis();
  uint8_t cin = event.packet.header & 0x0F;
  uint32_t now = millis();
  if (sysex_active && now - sysex_last_at > kSysexTimeoutMs) {
    sysex_active = false;
    sysex_bytes = 0;
  }
  if (cin == 0x04) {
    if (!sysex_active) {
      sysex_active = true;
      sysex_bytes = 0;
    }
    if (sysex_bytes + 3 > kMaxSysexBytes) {
      sysex_active = false;
      sysex_bytes = 0;
      counters.malformed++;
      return;
    }
    sysex_bytes += 3;
    sysex_last_at = now;
  } else if (cin == 0x05 || cin == 0x06 || cin == 0x07) {
    if (sysex_active) {
      sysex_bytes += cin == 0x05 ? 1 : (cin == 0x06 ? 2 : 3);
      sysex_last_at = now;
    }
    sysex_active = false;
    sysex_bytes = 0;
  }
  bool realtime = cin == 0x0F && event.packet.byte1 >= 0xF8;
  if (realtime) {
    queue_midi(remote_realtime_queue, remote_realtime_head, remote_realtime_tail, event);
  } else {
    queue_midi(remote_normal_queue, remote_normal_head, remote_normal_tail, event);
  }
  pulse_led();
}

void process_radio() {
  RadioFrame frame;
  while (pop_radio(frame)) {
    counters.received++;
    if (frame.length < kHeaderSize || std::memcmp(frame.data, kMagic, 4) != 0 ||
        frame.data[4] != kProtocolVersion) {
      counters.malformed++;
      continue;
    }
    uint8_t type = frame.data[5];
    uint32_t session = read_u32(frame.data + 8);
    uint32_t sequence = read_u32(frame.data + 12);
    size_t payload_length = (static_cast<size_t>(frame.data[16]) << 8) | frame.data[17];
    if (payload_length != frame.length - kHeaderSize || payload_length > kMaxPayload) {
      counters.malformed++;
      continue;
    }
    if (!bridge_source_allowed(paired, frame.source_mac, peer_mac)) {
      counters.foreign_packets++;
      continue;
    }
    if (type == MSG_DISCOVERY || type == MSG_DISCOVERY_ACK) {
      handle_discovery(frame.source_mac, type, session, frame.data + kHeaderSize, payload_length);
      continue;
    }
    if (type != MSG_MIDI || !paired) {
      counters.malformed++;
      continue;
    }
    last_peer_seen = millis();
    if (peer_timed_out) timeout_panic_sent = false;
    peer_timed_out = false;
    if (session != peer_session) {
      peer_session = session;
      have_peer_sequence = false;
      if (session != 0 && last_panic_session != session) {
        queue_panic(false);
        last_panic_session = session;
      }
    }
    if (have_peer_sequence) {
      if (!newer_sequence(sequence, last_peer_sequence)) {
        counters.duplicate++;
        continue;
      }
      uint32_t distance = sequence - last_peer_sequence;
      if (distance > 1) counters.sequence_gaps += distance - 1;
    }
    have_peer_sequence = true;
    last_peer_sequence = sequence;
    handle_midi(frame.data + kHeaderSize, payload_length);
  }
}

void process_usb_input() {
  decltype(MidiEvent::packet) packet{};
  while (MIDI.readPacket(&packet)) {
    MidiEvent event{};
    event.packet = packet;
    event.received_at = millis();
    counters.usb_in++;
    bool realtime = (packet.header & 0x0F) == 0x0F && packet.byte1 >= 0xF8;
    if (realtime) queue_midi(local_realtime_queue, local_realtime_head, local_realtime_tail, event);
    else queue_midi(local_normal_queue, local_normal_head, local_normal_tail, event);
    pulse_led();
  }
}

void process_usb_output() {
  MidiEvent event;
  for (uint8_t i = 0; i < 8; ++i) {
    if (!peek_midi(remote_realtime_queue, remote_realtime_tail, remote_realtime_head, event)) break;
    if (!bridge_usb_write_commit(MIDI.writePacket(&event.packet))) {
      counters.usb_busy++;
      break;
    }
    drop_midi(remote_realtime_queue, remote_realtime_tail, remote_realtime_head,
              remote_realtime_tail);
    counters.usb_out++;
  }
  for (uint8_t i = 0; i < 8; ++i) {
    if (!peek_midi(remote_normal_queue, remote_normal_tail, remote_normal_head, event)) break;
    if (!bridge_usb_write_commit(MIDI.writePacket(&event.packet))) {
      counters.usb_busy++;
      break;
    }
    drop_midi(remote_normal_queue, remote_normal_tail, remote_normal_head,
              remote_normal_tail);
    counters.usb_out++;
  }
}

void process_radio_tx() {
  TxResult result;
  TxKind kind;
  portENTER_CRITICAL(&tx_mux);
  result = tx_result;
  kind = tx_kind;
  if (result != TxResult::None) tx_result = TxResult::None;
  if (result != TxResult::None) tx_kind = TxKind::None;
  portEXIT_CRITICAL(&tx_mux);
  if (tx_in_flight && result != TxResult::None) {
    tx_in_flight = false;
    if (kind == TxKind::Discovery) {
      if (result == TxResult::Failure) counters.radio_async_failures++;
      return;
    }
    if (result == TxResult::Success) {
      counters.radio_accepted++;
      tx_immediate_retries = 0;
      tx_retry_after = 0;
    } else {
      counters.radio_async_failures++;
      tx_retry_after = millis() + kTxRetryDelayMs;
      tx_immediate_retries = kMaxImmediateRetries;
      // The frame was accepted by the radio, so retrying could duplicate a note.
    }
  }
  if (!paired || tx_in_flight || deadline_active(millis(), tx_retry_after)) return;

  MidiEvent event;
  bool have_normal = peek_midi(local_normal_queue, local_normal_tail, local_normal_head, event);
  bool use_realtime = bridge_use_realtime(
      consecutive_realtime_sends,
      peek_midi(local_realtime_queue, local_realtime_tail, local_realtime_head, event),
      have_normal);
  if (!use_realtime && !have_normal) {
    if (!peek_midi(local_realtime_queue, local_realtime_tail, local_realtime_head, event)) return;
    use_realtime = true;
  } else if (!use_realtime) {
    peek_midi(local_normal_queue, local_normal_tail, local_normal_head, event);
  }
  if (!event.forward) {
    if (use_realtime) drop_midi(local_realtime_queue, local_realtime_tail, local_realtime_head,
                             local_realtime_tail);
    else drop_midi(local_normal_queue, local_normal_tail, local_normal_head, local_normal_tail);
    return;
  }

  uint8_t payload[4] = {event.packet.header, event.packet.byte1, event.packet.byte2,
                        event.packet.byte3};
  uint8_t packet[kRadioLimit]{};
  size_t length = 0;
  if (!build_packet(MSG_MIDI, payload, sizeof(payload), packet, length)) return;
  portENTER_CRITICAL(&tx_mux);
  tx_result = TxResult::None;
  tx_in_flight = true;
  portEXIT_CRITICAL(&tx_mux);
  esp_err_t status = esp_now_send(peer_mac, packet, length);
  if (status == ESP_OK) {
    if (use_realtime) drop_midi(local_realtime_queue, local_realtime_tail, local_realtime_head,
                             local_realtime_tail);
    else drop_midi(local_normal_queue, local_normal_tail, local_normal_head, local_normal_tail);
    tx_immediate_retries = 0;
  } else {
    portENTER_CRITICAL(&tx_mux);
    tx_in_flight = false;
    portEXIT_CRITICAL(&tx_mux);
    counters.radio_immediate_failures++;
    if (tx_immediate_retries < kMaxImmediateRetries) {
      tx_immediate_retries++;
      counters.radio_retries++;
      tx_retry_after = millis() + kTxRetryDelayMs;
    } else {
      if (use_realtime) drop_midi(local_realtime_queue, local_realtime_tail, local_realtime_head,
                               local_realtime_tail);
      else drop_midi(local_normal_queue, local_normal_tail, local_normal_head, local_normal_tail);
      counters.queue_drops++;
      tx_immediate_retries = 0;
    }
  }
  if (status == ESP_OK) {
    if (use_realtime) consecutive_realtime_sends++;
    else consecutive_realtime_sends = 0;
  }
}

void process_local_radio() {
  process_radio_tx();
}

void handle_button() {
  bool down = digitalRead(kButtonPin) == LOW;
  uint32_t now = millis();
  if (down && !button_was_down) button_down_at = now;
  if (!down && button_was_down) {
    uint32_t held = now - button_down_at;
    if (held >= kLongPressMs) {
      clear_peer();
      pairing_reset_until = millis() + 600;
    } else {
      queue_panic(true);
    }
  }
  button_was_down = down;
}

void handle_connection_timeout() {
  if (!paired) return;
  bool timed_out = millis() - last_peer_seen > kPeerTimeoutMs;
  if (timed_out && !peer_timed_out) {
    peer_timed_out = true;
    if (!timeout_panic_sent) {
      queue_panic(false);
      timeout_panic_sent = true;
    }
  }
}

void setup_radio() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_channel(kWifiChannel, WIFI_SECOND_CHAN_NONE);
  WiFi.macAddress(local_mac);
  if (esp_now_init() != ESP_OK) {
    while (true) {
      rgb.setPixelColor(0, rgb.Color(50, 0, 50));
      rgb.show();
      delay(250);
      rgb.clear();
      rgb.show();
      delay(250);
    }
  }
  radio_queue_handle = xQueueCreateStatic(kRadioQueueDepth, sizeof(RadioFrame),
                                          radio_queue_storage_buffer, &radio_queue_storage);
  if (!radio_queue_handle) {
    while (true) delay(1000);
  }
  add_peer(kBroadcastMac);
  esp_now_register_send_cb(on_send);
  esp_now_register_recv_cb(on_receive);
  size_t length = preferences.getBytesLength("peer");
  if (length == sizeof(peer_mac) && preferences.getBool("paired", false)) {
    preferences.getBytes("peer", peer_mac, sizeof(peer_mac));
    paired = add_peer(peer_mac);
  }
}

void setup_impl() {
  pinMode(kButtonPin, INPUT_PULLUP);
  rgb.begin();
  rgb.setBrightness(64);
  rgb.clear();
  rgb.show();
  preferences.begin("midi-bridge", false);
  session_id = esp_random();
  setup_radio();
  MIDI.begin();
}

void loop_impl() {
  process_radio();
  process_usb_input();
  process_usb_output();
  process_local_radio();
  handle_button();
  handle_connection_timeout();
  uint32_t now = millis();
  if (now - last_discovery >= kDiscoveryPeriodMs &&
      (!paired || now - last_peer_seen > kPeerTimeoutMs)) {
    send_discovery();
    last_discovery = now;
  }
  if (sysex_active && now - sysex_last_at > kSysexTimeoutMs) sysex_active = false;
  render_led();
  delay(1);
}
}  // namespace

void setup() { setup_impl(); }
void loop() { loop_impl(); }
