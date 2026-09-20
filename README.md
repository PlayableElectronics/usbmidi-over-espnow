# AtomS3 Lite ESP-NOW MIDI bridge

This subproject builds one identical firmware image for two M5Stack AtomS3 Lite
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
cd m5stack/espnow-midi-bridge
./build.sh
python3 -m unittest discover -s tests -p 'test_*.py'
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
during repository verification, so enumeration and radio operation remain
hardware-pending.

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
radio frame. Remote event packets are delivered to the USB MIDI input endpoint
without being re-forwarded, preventing internal echo loops. Realtime CIN 0xF
events are serviced ahead of normal traffic. SysEx is transported as the
ordered sequence of USB-MIDI event packets, with bounded 250-byte radio frames,
bounded queues, a 65,535-byte reassembly ceiling in the reference tooling and
a one-second incomplete-stream timeout in the firmware design. Sequence gaps,
duplicates, malformed packets and queue drops are counted internally.

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
ESP-NOW send callbacks only update counters; USB and protocol work runs in the
main loop, so callbacks do not block on USB or acknowledgements.

## Hardware test plan

These tests are planned, not passed:

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

For troubleshooting, first check that both boards are in download mode during
flashing, that both use the same image hash, that neither is joined to another
Wi-Fi network, and that both are on channel 6. A third nearby bridge can still
be selected after a forget operation because the first compatible discovery is
accepted; reset and pair the intended two units in isolation when possible.

## License

The bridge source in this directory is MIT-licensed; see `LICENSE`.
Arduino-ESP32, ESP-NOW and Adafruit NeoPixel remain subject to their
respective upstream licenses.
