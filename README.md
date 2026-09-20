# AtomS3 Lite ESP-NOW MIDI bridge

This repository builds one identical firmware image for two M5Stack AtomS3 Lite
boards. Each board is a native USB MIDI 1.0 device and an ESP-NOW peer; the
same binary works on either side of the link.

## Board facts and sources

The pin assignments are taken from the official M5Unified AtomS3/S3Lite board
table and the official M5AtomS3 Lite example, not inferred from package shape:

- native USB D−: GPIO19; native USB D+: GPIO20;
- RGB LED: GPIO35, one SK6812-family addressable LED;
- user button A: GPIO41, used as active-low with the internal pull-up.

References: [M5Unified board pin table](https://github.com/m5stack/M5Unified#gpio-pinouts),
[official AtomS3 repository/example](https://github.com/m5stack/M5AtomS3),
[Arduino-ESP32 native USB MIDI example](https://github.com/espressif/arduino-esp32/tree/master/libraries/USB/examples/MIDI/MidiInterface).
The board's official ROM-download procedure is: hold the reset button for
about two seconds until the internal LED turns green, then release; the board
enters the ESP32-S3 download mode.

PlatformIO uses the official `m5stack-atoms3` board definition (the AtomS3
Lite shares this ESP32-S3/8 MB/native-USB target); the build overrides only the
USB mode to select the native TinyUSB device path and disables CDC. The
project uses the maintained TinyUSB MIDI class shipped with
Arduino-ESP32, through its native TinyUSB hooks, over the ESP32-S3 native USB
peripheral. CDC is disabled at boot, so the runtime interface is MIDI-only
rather than an unnecessary composite serial device.

## Build and flash

Prerequisites are Python 3, PlatformIO Core 6.x, and a USB data cable. macOS
and Linux use the same commands. The build script places PlatformIO's cache in
the repository's ignored `.local/` directory instead of assuming a writable
global PlatformIO home.

```sh
./build.sh
python3 -m unittest discover -s tests -p 'test_*.py'
./tests/run_cpp_tests.sh
```

The generated image is `.pio/build/atoms3-lite/firmware.bin`. Both boards
must be flashed with this exact file; verify its hash before using it on the
second board:

```sh
shasum -a 256 .pio/build/atoms3-lite/firmware.bin
./flash.sh /dev/cu.usbmodemXXXX       # macOS
./flash.sh /dev/ttyACM0               # Linux
```

`flash.sh` performs a normal PlatformIO upload only. Put an Atom into ROM
download mode first using the reset procedure above. No hardware was attached
during this reliability-hardening build, so the changes below remain pending
hardware regression testing.

## Pairing and operation

On first boot both units listen and broadcast discovery on fixed Wi-Fi channel
6 without joining an access point. The first compatible unpaired peer is stored
in NVS and added as an ESP-NOW unicast peer. A rebooted peer is rediscovered by
its stored MAC. A long button press (at least 1.5 seconds) clears the stored
peer and returns to discovery. A short press sends, locally and over the link,
CC 123 (All Notes Off) and CC 120 (All Sound Off) on all 16 MIDI channels.

The lower numerical MAC address uses dim green and the higher uses dim red.
Discovery slowly blinks; a lost peer fast-blinks; MIDI activity briefly pulses
the assigned colour. Color never selects MIDI direction. The initial protocol
does not authenticate or encrypt ESP-NOW payloads; pairing is therefore
convenience pairing, not a security boundary.

USB input packets are queued locally and sent one USB-MIDI event packet per
radio frame. A fixed FreeRTOS queue carries complete received radio frames,
including their source MAC, from the ESP-NOW callback to the main loop. Remote
event packets are delivered to the USB MIDI input endpoint without being
re-forwarded, preventing internal echo loops. A remote USB event remains at the
head of its queue until `tud_midi_packet_write()` accepts it; a busy endpoint
therefore causes bounded retry rather than loss.

At most one MIDI unicast is in flight. The event is removed from the local
queue only after `esp_now_send()` accepts the frame. Immediate submission
errors retain the event for up to three short retries; an asynchronous delivery
failure is not blindly retried because the MAC acknowledgement is ambiguous for
Note On duplication. Realtime CIN 0xF events are prioritized, with one normal
event admitted after at most four realtime sends so normal traffic cannot be
permanently starved. All queues are bounded and drops, USB busy results,
immediate radio errors, asynchronous radio errors, accepted sends, retries and
foreign-source packets are counted in firmware diagnostics.

After pairing, only frames whose source MAC equals the stored peer are accepted.
Unpaired discovery validates that the payload MAC matches the actual radio
source. Discovery and MIDI packets share the same bounded send gate, so a
discovery callback cannot corrupt MIDI transmit state.

SysEx is transported as the ordered sequence of USB-MIDI event packets, with
bounded 250-byte radio frames, bounded queues, a 65,535-byte reassembly ceiling
in the reference tooling and a one-second incomplete-stream timeout in the
firmware design. Sequence gaps, duplicates, malformed packets and queue drops
are counted internally.

When a paired peer times out, the bridge queues one local panic: CC 123 All
Notes Off and CC 120 All Sound Off on all 16 channels. The same local-only
panic is queued once when a returning peer advertises a new session ID. A short
button press queues the same 32 messages locally and for radio forwarding; it
does not write directly to USB or radio. A long press removes the stored
unicast peer, resets session/sequence/in-flight state, flushes queues, and
returns to discovery without blocking LED animation.

Wire header, in network byte order, is:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `ENM1` |
| 4 | 1 | protocol version `1` |
| 5 | 1 | message type |
| 6 | 2 | flags/reserved |
| 8 | 4 | boot/session identifier |
| 12 | 4 | sequence number |
| 16 | 2 | payload length |
| 18 | N | payload |

The payload is either discovery identity/session data or one four-byte
`midiEventPacket_t` preserving its USB-MIDI header/CIN and three MIDI bytes.
ESP-NOW callbacks only copy fixed-size frames to the FreeRTOS receive queue or
publish a small transmit completion state; USB, Preferences and protocol work
runs in the main loop. No callback allocates memory, writes USB, or waits for a
radio acknowledgement.

## Hardware test plan

Previously hardware-observed:

- both AtomS3 Lite boards enumerate as `ESP-NOW MIDI Bridge`;
- the two-node pairing fix was observed to make the opposite activity LED
  flash when MIDI was sent from Pure Data.

The reliability changes in this revision still require the following physical
regression tests:

1. Flash the identical image to two boards; confirm both enumerate as
   `ESP-NOW MIDI Bridge` with a class-compliant MIDI interface.
2. Pair them on a fixed channel and test Mac→NerdSEQ notes, NerdSEQ→Mac notes,
   simultaneous notes/CC, dense CC, MIDI clock/start/stop/continue, realtime
   active sensing/reset, and long SysEx in both directions.
3. Reboot either peer during traffic; verify rediscovery, no duplicate Note On,
   and panic recovery after interference. Test long-press reset and re-pair.
4. Measure MIDI clock jitter and one-way/round-trip latency with a GPIO toggle
  or timestamped MIDI test generator. The design target is approximately
  1–3 ms practical bridge latency; it is not a measured result.

For dense-traffic testing, send a sustained CC stream while interleaving MIDI
clock and start/stop, unplug one USB host briefly, reboot one Atom, and send a
long SysEx. Confirm that realtime traffic continues, no stuck notes remain after
the timeout panic, no duplicate Note On is heard after recovery, and the two
devices can be long-press reset and paired again without changing the firmware
image.

For troubleshooting, first check that both boards are in download mode during
flashing, that both use the same image hash, that neither is joined to another
Wi-Fi network, and that both are on channel 6. A third nearby bridge can still
be selected after a forget operation because the first compatible discovery is
accepted; reset and pair the intended two units in isolation when possible.

On macOS, the two bridge devices can be tested without NerdSEQ using the
tracked CoreMIDI loopback utility:

```sh
clang -framework CoreFoundation -framework CoreMIDI \
  tools/macos_midi_loopback_test.c -o /tmp/espnow-midi-loopback-test
/tmp/espnow-midi-loopback-test --test
```

It sends one note-on, MIDI clock and note-off to each destination in turn,
waits 350 ms, and requires the opposite Atom's source to receive the traffic.
It is a one-shot test and does not transmit anything if exactly two bridge MIDI
sources and destinations are not present.

## License

The bridge source in this directory is MIT-licensed; see `LICENSE`.
Arduino-ESP32, ESP-NOW and Adafruit NeoPixel remain subject to their
respective upstream licenses.
