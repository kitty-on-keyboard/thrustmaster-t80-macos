// Thrustmaster T80 -> UDP bridge.
//
// The T80 publishes a generic gamepad HID descriptor whose declared axes never
// move: X/Y/Z/Rz sit at 0x80 and the pedals at 0 forever, which is why Godot,
// SDL and every controller mapping see a wheel with buttons and no steering.
// Apple also reports GameControllerSupportedHIDDevice=No, so Godot 4.6's
// joypad list can be empty while the wheel is still on USB. Its real data is
// in the vendor block of the same report, decoded here and measured on the
// device rather than guessed:
//
//   bytes 43..44  steering  uint16 LE, centre ~32900, full lock 0 .. 65535
//   bytes 45..46  throttle  uint16 LE, 65535 released -> 0 floored (inverted)
//   bytes 47..48  brake     uint16 LE, 65535 released -> 0 floored (inverted)
//   byte  5       low nibble = hat (0=N .. 7=NW, 8=neutral);
//                 high nibble = Triangle, Cross, Circle, Square
//   byte  6       L1 R1 L2 R2 Select Start L3 R3
//
// Button bits were reverse-engineered against this same VID/PID (Linux
// hid-t80). Godot never enumerates the device, so analog AND buttons have to
// ride this socket -- the HID gamepad path does not reach menus.
//
// Emits "steer throttle brake buttons hat" as text to 127.0.0.1:7654.
#include <IOKit/hid/IOHIDManager.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 7654
#define STEER_LO 43
#define GAS_LO   45
#define BRAKE_LO 47
#define TM_VID   0x044F

static int sock;
static struct sockaddr_in dest;
static int centre = 32768;
static int centred = 0;
static int samples = 0;
static long centre_acc = 0;
static int verbose = 0;
static IOHIDDeviceRef device = NULL;
static uint8_t buf[64];

static inline int le16(const uint8_t *b, int i) { return b[i] | (b[i + 1] << 8); }

static int last_buttons = -1;
static int last_hat = -1;

static void emit_state(float steer, float gas, float brake, unsigned buttons, unsigned hat) {
    char msg[80];
    int n = snprintf(msg, sizeof(msg), "%.4f %.4f %.4f %u %u",
                     steer, gas, brake, buttons, hat);
    sendto(sock, msg, n, 0, (struct sockaddr *)&dest, sizeof(dest));
    if (verbose && ((int)buttons != last_buttons || (int)hat != last_hat)) {
        fprintf(stderr, "[t80] buttons 0x%x hat %u  steer %+.3f gas %.3f brake %.3f\n",
                buttons, hat, steer, gas, brake);
        last_buttons = (int)buttons;
        last_hat = (int)hat;
    } else if (verbose) {
        fprintf(stderr, "\r[t80] steer %+.3f  gas %.3f  brake %.3f   ",
                steer, gas, brake);
        fflush(stderr);
    }
}

static void decode_buttons(const uint8_t *rep, CFIndex len, unsigned *buttons, unsigned *hat) {
    *hat = 8;
    *buttons = 0;
    if (len < 7) return;
    *hat = (unsigned)(rep[5] & 0x0F);
    if (*hat > 8) *hat = 8;
    // High nibble of byte 5, then all of byte 6. Same packing hid-t80 uses.
    *buttons = (unsigned)((rep[5] >> 4) & 0x0F) | ((unsigned)rep[6] << 4);
}

static void on_report(void *c, IOReturn r, void *s, IOHIDReportType t,
                      uint32_t id, uint8_t *rep, CFIndex len) {
    (void)c; (void)r; (void)s; (void)t; (void)id;
    if (len < BRAKE_LO + 2) return;

    unsigned buttons = 0, hat = 8;
    decode_buttons(rep, len, &buttons, &hat);

    int raw = le16(rep, STEER_LO);
    // The wheel's rest position is not exactly 0x8000, and a fixed centre puts
    // a permanent lean on the steering. Average the first samples instead --
    // the operator is not touching it while the bridge starts.
    if (!centred) {
        centre_acc += raw;
        if (++samples >= 60) { centre = (int)(centre_acc / samples); centred = 1;
            fprintf(stderr, "[t80] centre calibrated at %d\n", centre); }
        emit_state(0.0f, 0.0f, 0.0f, buttons, hat);
        return;
    }

    float steer = (float)(raw - centre) / 32768.0f;
    if (steer > 1.0f) steer = 1.0f;
    if (steer < -1.0f) steer = -1.0f;
    // Kills the low-byte sensor noise that otherwise reads as a constant creep.
    if (steer > -0.012f && steer < 0.012f) steer = 0.0f;

    float gas   = (65535.0f - le16(rep, GAS_LO))   / 65535.0f;
    float brake = (65535.0f - le16(rep, BRAKE_LO)) / 65535.0f;
    if (gas < 0.004f) gas = 0.0f;
    if (brake < 0.004f) brake = 0.0f;

    emit_state(steer, gas, brake, buttons, hat);
}

static void on_removal(void *c, IOReturn r, void *s, IOHIDDeviceRef dev) {
    (void)c; (void)r; (void)s;
    if (dev != device) return;
    fprintf(stderr, "[t80] wheel detached; waiting to reconnect\n");
    device = NULL;
    centred = 0;
    samples = 0;
    centre_acc = 0;
}

static void on_match(void *c, IOReturn res, void *sender, IOHIDDeviceRef dev) {
    (void)c; (void)res; (void)sender;
    if (device != NULL) return;

    IOReturn o = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone);
    if (o != kIOReturnSuccess) {
        fprintf(stderr, "[t80] IOHIDDeviceOpen failed (%#x)\n", (unsigned)o);
        return;
    }
    device = dev;
    centred = 0;
    samples = 0;
    centre_acc = 0;
    IOHIDDeviceRegisterInputReportCallback(dev, buf, sizeof(buf), on_report, NULL);
    IOHIDDeviceScheduleWithRunLoop(dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

    CFStringRef product = (CFStringRef)IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDProductKey));
    char name[128] = "Thrustmaster";
    if (product) CFStringGetCString(product, name, sizeof(name), kCFStringEncodingUTF8);
    fprintf(stderr, "[t80] bridging %s -> 127.0.0.1:%d (hands off while it centres)\n",
            name, PORT);
}

int main(int argc, char **argv) {
    verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("[t80] socket"); return 1; }
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(PORT);
    dest.sin_addr.s_addr = inet_addr("127.0.0.1");

    IOHIDManagerRef m = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    int vid = TM_VID;
    CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(d, CFSTR(kIOHIDVendorIDKey), v);
    IOHIDManagerSetDeviceMatching(m, d);
    IOHIDManagerRegisterDeviceMatchingCallback(m, on_match, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(m, on_removal, NULL);
    IOHIDManagerScheduleWithRunLoop(m, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    IOReturn opened = IOHIDManagerOpen(m, kIOHIDOptionsTypeNone);
    if (opened != kIOReturnSuccess) {
        fprintf(stderr, "[t80] IOHIDManagerOpen failed (%#x) -- Input Monitoring?\n",
                (unsigned)opened);
        return 1;
    }
    fprintf(stderr, "[t80] waiting for a Thrustmaster wheel on 127.0.0.1:%d\n", PORT);
    CFRunLoopRun();
    return 0;
}
