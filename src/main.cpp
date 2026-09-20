#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <USB.h>
#include "esp32-hal-tinyusb.h"
#include "tusb.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <array>
#include <cstring>

namespace {
constexpr uint8_t kButtonPin = 41;
constexpr uint8_t kRgbPin = 35;
constexpr uint8_t kWifiChannel = 6;
constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kMagic[4] = {'E', 'N', 'M', '1'};
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

enum MessageType : uint8_t { MSG_DISCOVERY = 1, MSG_DISCOVERY_ACK = 2, MSG_MIDI = 3 };

struct MidiEvent {
  struct {
    uint8_t header = 0;
    uint8_t byte1 = 0;
    uint8_t byte2 = 0;
    uint8_t byte3 = 0;
  } packet;
  uint32_t received_at = 0;
};

struct RadioFrame {
  uint8_t data[kRadioLimit]{};
  size_t length = 0;
};

struct Counters {
  uint32_t sent = 0;
  uint32_t received = 0;
  uint32_t send_failed = 0;
  uint32_t duplicate = 0;
  uint32_t malformed = 0;
  uint32_t sequence_gaps = 0;
  uint32_t queue_drops = 0;
  uint32_t usb_in = 0;
  uint32_t usb_out = 0;
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

std::array<RadioFrame, kRadioQueueDepth> radio_queue{};
volatile size_t radio_head = 0;
volatile size_t radio_tail = 0;
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

bool mac_equal(const uint8_t *a, const uint8_t *b) { return std::memcmp(a, b, 6) == 0; }

bool mac_less(const uint8_t *a, const uint8_t *b) {
  return std::memcmp(a, b, 6) < 0;
}

bool queue_radio(const uint8_t *data, size_t length) {
  if (length > kRadioLimit || ((radio_head + 1) % kRadioQueueDepth) == radio_tail) {
    counters.queue_drops++;
    return false;
  }
  std::memcpy(radio_queue[radio_head].data, data, length);
  radio_queue[radio_head].length = length;
  radio_head = (radio_head + 1) % kRadioQueueDepth;
  return true;
}

bool pop_radio(RadioFrame &frame) {
  if (radio_tail == radio_head) return false;
  frame = radio_queue[radio_tail];
  radio_tail = (radio_tail + 1) % kRadioQueueDepth;
  return true;
}

template <size_t N>
bool queue_midi(std::array<MidiEvent, N> &queue, size_t &head, size_t tail,
                const MidiEvent &event) {
  size_t next = (head + 1) % N;
  if (next == tail) {
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

void pulse_led() { led_pulse_until = millis() + 30; }

void render_led() {
  uint32_t now = millis();
  bool lost = paired && (now - last_peer_seen > kPeerTimeoutMs);
  bool blink = lost ? ((now / 120) % 2 == 0) : (!paired && ((now / 600) % 2 == 0));
  bool pulse = now < led_pulse_until;
  uint8_t red = 8, green = 8;
  if (paired) {
    if (mac_less(local_mac, peer_mac)) green = 64;
    else red = 64;
  } else {
    red = green = 12;
  }
  if (blink) red = green = 0;
  if (pulse) {
    if (mac_less(local_mac, peer_mac)) green = 160;
    else if (paired) red = 160;
    else red = green = 96;
  }
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
  preferences.clear();
  paired = false;
  have_peer_sequence = false;
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

bool send_raw(const uint8_t *destination, const uint8_t *data, size_t length) {
  if (!destination || length > kRadioLimit) return false;
  return esp_now_send(destination, data, length) == ESP_OK;
}

bool send_packet(uint8_t type, const uint8_t *payload, size_t payload_length,
                 const uint8_t *destination = nullptr) {
  if (!paired && type == MSG_MIDI) return false;
  uint8_t packet[kRadioLimit]{};
  size_t length = 0;
  if (!build_packet(type, payload, payload_length, packet, length)) return false;
  if (destination) return send_raw(destination, packet, length);
  if (!paired) return false;
  return send_raw(peer_mac, packet, length);
}

void send_discovery(uint8_t type = MSG_DISCOVERY, const uint8_t *destination = nullptr) {
  uint8_t payload[10]{};
  std::memcpy(payload, local_mac, 6);
  write_u32(payload + 6, session_id);
  uint8_t packet[kRadioLimit]{};
  size_t length = 0;
  if (!build_packet(type, payload, sizeof(payload), packet, length)) return;
  if (destination) send_raw(destination, packet, length);
  else esp_now_send(nullptr, packet, length);
}

void send_midi_event(const MidiEvent &event) {
  uint8_t payload[4] = {event.packet.header, event.packet.byte1, event.packet.byte2,
                        event.packet.byte3};
  send_packet(MSG_MIDI, payload, sizeof(payload));
}

void send_panic() {
  for (uint8_t channel = 0; channel < 16; ++channel) {
    for (uint8_t controller : {static_cast<uint8_t>(123), static_cast<uint8_t>(120)}) {
      MidiEvent event{};
      event.packet.header = 0x0B;
      event.packet.byte1 = static_cast<uint8_t>(0xB0 | channel);
      event.packet.byte2 = controller;
      event.packet.byte3 = 0;
      MIDI.writePacket(&event.packet);
      send_midi_event(event);
    }
  }
  pulse_led();
}

void on_send(const uint8_t *, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) counters.sent++;
  else counters.send_failed++;
}

void on_receive(const uint8_t *, const uint8_t *data, int length) {
  if (length <= 0 || length > static_cast<int>(kRadioLimit)) {
    counters.malformed++;
    return;
  }
  queue_radio(data, static_cast<size_t>(length));
}

bool newer_sequence(uint32_t candidate, uint32_t previous) {
  return static_cast<int32_t>(candidate - previous) > 0;
}

void handle_discovery(uint8_t type, uint32_t session, const uint8_t *payload,
                     size_t payload_length) {
  if (payload_length != 10 || read_u32(payload + 6) != session || mac_equal(payload, local_mac)) {
    counters.malformed++;
    return;
  }
  if (paired && !mac_equal(payload, peer_mac)) return;
  if (!paired) {
    std::memcpy(peer_mac, payload, 6);
    if (!add_peer(peer_mac)) return;
    paired = true;
    save_peer();
  }
  last_peer_seen = millis();
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
    if (type == MSG_DISCOVERY || type == MSG_DISCOVERY_ACK) {
      handle_discovery(type, session, frame.data + kHeaderSize, payload_length);
      continue;
    }
    if (type != MSG_MIDI || !paired) {
      counters.malformed++;
      continue;
    }
    last_peer_seen = millis();
    if (session != peer_session) {
      peer_session = session;
      have_peer_sequence = false;
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
    if (!pop_midi(remote_realtime_queue, remote_realtime_tail, remote_realtime_head,
                 remote_realtime_tail, event)) break;
    MIDI.writePacket(&event.packet);
    counters.usb_out++;
  }
  for (uint8_t i = 0; i < 8; ++i) {
    if (!pop_midi(remote_normal_queue, remote_normal_tail, remote_normal_head,
                 remote_normal_tail, event)) break;
    MIDI.writePacket(&event.packet);
    counters.usb_out++;
  }
}

void process_local_radio() {
  MidiEvent event;
  for (uint8_t i = 0; i < 8; ++i) {
    if (pop_midi(local_realtime_queue, local_realtime_tail, local_realtime_head,
                 local_realtime_tail, event)) send_midi_event(event);
  }
  if (pop_midi(local_normal_queue, local_normal_tail, local_normal_head,
               local_normal_tail, event)) send_midi_event(event);
}

void handle_button() {
  bool down = digitalRead(kButtonPin) == LOW;
  uint32_t now = millis();
  if (down && !button_was_down) button_down_at = now;
  if (!down && button_was_down) {
    uint32_t held = now - button_down_at;
    if (held >= kLongPressMs) {
      clear_peer();
      for (uint8_t i = 0; i < 3; ++i) {
        rgb.setPixelColor(0, rgb.Color(40, 40, 40));
        rgb.show();
        delay(80);
        rgb.clear();
        rgb.show();
        delay(80);
      }
    } else {
      send_panic();
    }
  }
  button_was_down = down;
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
  USB.begin();
}

void loop_impl() {
  process_radio();
  process_usb_input();
  process_usb_output();
  process_local_radio();
  handle_button();
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
