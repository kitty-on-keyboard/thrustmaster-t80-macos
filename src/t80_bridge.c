// Thrustmaster T80 -> UDP bridge.
//
// The T80 publishes a generic gamepad HID descriptor whose declared axes never
// move: X/Y/Z/Rz sit at 0x80 and the pedals at 0 forever, which is why Godot,
// SDL and every controller mapping see a wheel with buttons and no steering.
// Its real data is in the vendor block of the same report, decoded here and
// measured on the device rather than guessed:
//
//   bytes 43..44  steering  uint16 LE, centre ~32900, full lock 0 .. 65535
//   bytes 45..46  throttle  uint16 LE, 65535 released -> 0 floored (inverted)
//   bytes 47..48  brake     uint16 LE, 65535 released -> 0 floored (inverted)
//   byte  5       hat + buttons (already handled by the normal HID path)
//
// Emits "steer throttle brake" as text to 127.0.0.1:7654, which the game's
// WheelInput autoload reads. Text rather than packed floats so the stream can
// be watched with nc while debugging.
#include <IOKit/hid/IOHIDManager.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 7654
#define STEER_LO 43
#define GAS_LO   45
#define BRAKE_LO 47

static int sock;
static struct sockaddr_in dest;
static int centre = 32768;
static int centred = 0;
static int samples = 0;
static long centre_acc = 0;
static int verbose = 0;

static inline int le16(const uint8_t *b, int i) { return b[i] | (b[i + 1] << 8); }

static void on_report(void *c, IOReturn r, void *s, IOHIDReportType t,
                      uint32_t id, uint8_t *rep, CFIndex len) {
    if (len < BRAKE_LO + 2) return;

    int raw = le16(rep, STEER_LO);
    // The wheel's rest position is not exactly 0x8000, and a fixed centre puts
    // a permanent lean on the steering. Average the first samples instead --
    // the operator is not touching it while the bridge starts.
    if (!centred) {
        centre_acc += raw;
        if (++samples >= 60) { centre = (int)(centre_acc / samples); centred = 1;
            fprintf(stderr, "[t80] centre calibrated at %d\n", centre); }
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

    char msg[64];
    int n = snprintf(msg, sizeof(msg), "%.4f %.4f %.4f", steer, gas, brake);
    sendto(sock, msg, n, 0, (struct sockaddr *)&dest, sizeof(dest));
    if (verbose) { fprintf(stderr, "\r[t80] steer %+.3f  gas %.3f  brake %.3f   ",
                           steer, gas, brake); fflush(stderr); }
}

int main(int argc, char **argv) {
    verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(PORT);
    dest.sin_addr.s_addr = inet_addr("127.0.0.1");

    IOHIDManagerRef m = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    int vid = 0x044F;
    CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(d, CFSTR(kIOHIDVendorIDKey), v);
    IOHIDManagerSetDeviceMatching(m, d);
    IOHIDManagerOpen(m, kIOHIDOptionsTypeNone);
    CFSetRef devs = IOHIDManagerCopyDevices(m);
    if (!devs || CFSetGetCount(devs) == 0) {
        fprintf(stderr, "[t80] no Thrustmaster wheel connected\n"); return 1;
    }
    IOHIDDeviceRef arr[8]; CFSetGetValues(devs, (const void **)arr);
    IOHIDDeviceOpen(arr[0], kIOHIDOptionsTypeNone);
    static uint8_t buf[64];
    IOHIDDeviceRegisterInputReportCallback(arr[0], buf, sizeof(buf), on_report, NULL);
    IOHIDDeviceScheduleWithRunLoop(arr[0], CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    fprintf(stderr, "[t80] bridging wheel -> 127.0.0.1:%d (hands off while it centres)\n", PORT);
    CFRunLoopRun();
    return 0;
}
