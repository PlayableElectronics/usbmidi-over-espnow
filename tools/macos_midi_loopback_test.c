#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  int index;
  int packets;
  int bytes;
} InputContext;

static void read_proc(const MIDIPacketList *list, void *read_ref, void *src_ref) {
  (void)src_ref;
  InputContext *ctx = (InputContext *)read_ref;
  const MIDIPacket *packet = &list->packet[0];
  for (UInt32 i = 0; i < list->numPackets; ++i) {
    ++ctx->packets;
    ctx->bytes += (int)packet->length;
    printf("RX source=%d", ctx->index);
    for (UInt16 j = 0; j < packet->length; ++j) printf(" %02X", packet->data[j]);
    putchar('\n');
    packet = MIDIPacketNext(packet);
  }
  fflush(stdout);
}

static void endpoint_name(MIDIEndpointRef endpoint, char *out, size_t size) {
  CFStringRef value = NULL;
  out[0] = '\0';
  if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyName, &value) == noErr && value) {
    CFStringGetCString(value, out, (CFIndex)size, kCFStringEncodingUTF8);
    CFRelease(value);
  }
}

static void list_endpoints(void) {
  ItemCount sources = MIDIGetNumberOfSources();
  ItemCount destinations = MIDIGetNumberOfDestinations();
  printf("sources=%lu destinations=%lu\n", (unsigned long)sources,
         (unsigned long)destinations);
  for (ItemCount i = 0; i < sources; ++i) {
    char name[256];
    endpoint_name(MIDIGetSource(i), name, sizeof(name));
    printf("source[%lu]=%s\n", (unsigned long)i, name);
  }
  for (ItemCount i = 0; i < destinations; ++i) {
    char name[256];
    endpoint_name(MIDIGetDestination(i), name, sizeof(name));
    printf("destination[%lu]=%s\n", (unsigned long)i, name);
  }
}

static int send_message(MIDIPortRef port, MIDIEndpointRef destination,
                         const Byte *data, UInt16 length) {
  Byte storage[1024];
  MIDIPacketList *list = (MIDIPacketList *)storage;
  MIDIPacket *packet = MIDIPacketListInit(list);
  if (!MIDIPacketListAdd(list, sizeof(storage), packet, 0, length, data)) return -1;
  return MIDISend(port, destination, list);
}

int main(int argc, char **argv) {
  if (argc != 2 || strcmp(argv[1], "--test") != 0) {
    fprintf(stderr, "usage: %s --test\n", argv[0]);
    list_endpoints();
    return 2;
  }
  ItemCount sources = MIDIGetNumberOfSources();
  ItemCount destinations = MIDIGetNumberOfDestinations();
  if (sources != 2 || destinations != 2) {
    fprintf(stderr, "expected exactly two bridge MIDI sources and destinations\n");
    list_endpoints();
    return 1;
  }
  MIDIClientRef client = 0;
  MIDIPortRef output = 0;
  MIDIPortRef inputs[2] = {0, 0};
  InputContext contexts[2] = {{0, 0, 0}, {1, 0, 0}};
  if (MIDIClientCreate(CFSTR("ESP-NOW bridge test"), NULL, NULL, &client) != noErr ||
      MIDIOutputPortCreate(client, CFSTR("output"), &output) != noErr) {
    fprintf(stderr, "could not create CoreMIDI client/output\n");
    return 1;
  }
  for (int i = 0; i < 2; ++i) {
    if (MIDIInputPortCreate(client, CFSTR("input"), read_proc, &contexts[i], &inputs[i]) != noErr ||
        MIDIPortConnectSource(inputs[i], MIDIGetSource(i), NULL) != noErr) {
      fprintf(stderr, "could not connect source %d\n", i);
      return 1;
    }
  }
  const Byte note_on[] = {0x90, 60, 100};
  const Byte clock[] = {0xF8};
  const Byte note_off[] = {0x80, 60, 0};
  for (int destination = 0; destination < 2; ++destination) {
    int before0 = contexts[0].packets;
    int before1 = contexts[1].packets;
    if (send_message(output, MIDIGetDestination(destination), note_on, sizeof(note_on)) != noErr ||
        send_message(output, MIDIGetDestination(destination), clock, sizeof(clock)) != noErr ||
        send_message(output, MIDIGetDestination(destination), note_off, sizeof(note_off)) != noErr) {
      fprintf(stderr, "send failed for destination %d\n", destination);
      return 1;
    }
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.35, false);
    int remote = 1 - destination;
    int local_delta = contexts[destination].packets - (destination ? before1 : before0);
    int remote_delta = contexts[remote].packets - (remote ? before1 : before0);
    printf("RESULT destination=%d local_packets=%d remote_packets=%d\n",
           destination, local_delta, remote_delta);
    if (remote_delta < 2) {
      fprintf(stderr, "did not receive note traffic on opposite Atom for destination %d\n", destination);
      return 1;
    }
  }
  printf("PASS bidirectional MIDI forwarding\n");
  return 0;
}
