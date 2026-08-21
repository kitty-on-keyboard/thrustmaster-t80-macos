// Attribute vendor-blob bytes to physical controls by phase.
//
// A byte that changes while nobody is touching the wheel is a packet counter,
// not an input -- that is what phase 0 is for. Bytes that only move during
// their own phase are the real controls.
#include <IOKit/hid/IOHIDManager.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXLEN 64
#define NPHASE 4
static int phase = 0;
static long lo[NPHASE][MAXLEN], hi[NPHASE][MAXLEN];
static int seen[NPHASE][MAXLEN];
static int reports[NPHASE];

static void on_report(void *c, IOReturn r, void *s, IOHIDReportType t,
                      uint32_t id, uint8_t *rep, CFIndex len) {
    if (len > MAXLEN) len = MAXLEN;
    reports[phase]++;
    for (int i = 0; i < len; i++) {
        if (!seen[phase][i]) { seen[phase][i] = 1; lo[phase][i] = hi[phase][i] = rep[i]; }
        if (rep[i] < lo[phase][i]) lo[phase][i] = rep[i];
        if (rep[i] > hi[phase][i]) hi[phase][i] = rep[i];
    }
}

int main(int argc, char **argv) {
    IOHIDManagerRef m = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    // Any vendor: pass it as the first argument, e.g. 0x044F for Thrustmaster.
    // `ioreg -c IOHIDDevice -r -d 1 -l | grep -E '"Product"|"VendorID"'` lists
    // what is attached.
    int vid = 0x044F;
    if (argc > 1) vid = (int)strtol(argv[1], NULL, 0);
    CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(d, CFSTR(kIOHIDVendorIDKey), v);
    IOHIDManagerSetDeviceMatching(m, d);
    IOHIDManagerOpen(m, kIOHIDOptionsTypeNone);
    CFSetRef devs = IOHIDManagerCopyDevices(m);
    if (!devs || CFSetGetCount(devs) == 0) { printf("no device matching vendor 0x%04X\n", vid); return 1; }
    IOHIDDeviceRef arr[8]; CFSetGetValues(devs, (const void **)arr);
    IOHIDDeviceRef dev = arr[0];
    IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone);
    static uint8_t buf[MAXLEN];
    IOHIDDeviceRegisterInputReportCallback(dev, buf, MAXLEN, on_report, NULL);
    IOHIDDeviceScheduleWithRunLoop(dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

    const char *names[NPHASE] = {
        ">>> PHASE 1/4: HANDS COMPLETELY OFF  (8s)",
        ">>> PHASE 2/4: TURN THE WHEEL lock to lock, nothing else  (8s)",
        ">>> PHASE 3/4: THROTTLE pedal only, floor it a few times  (8s)",
        ">>> PHASE 4/4: BRAKE pedal only, floor it a few times  (8s)" };
    for (phase = 0; phase < NPHASE; phase++) {
        printf("\n%s\n", names[phase]); fflush(stdout);
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 8.0, false);
        printf("    ...done (%d reports)\n", reports[phase]); fflush(stdout);
    }

    printf("\n\n%-6s %-14s %-14s %-14s %-14s  %s\n",
           "BYTE", "idle", "wheel", "throttle", "brake", "CONCLUSION");
    printf("%s\n", "-------------------------------------------------------------------------------------");
    for (int i = 0; i < MAXLEN; i++) {
        int moves[NPHASE];
        int any = 0;
        for (int p = 0; p < NPHASE; p++) {
            moves[p] = seen[p][i] && (hi[p][i] - lo[p][i] > 3);
            any |= moves[p];
        }
        if (!any) continue;
        char cell[NPHASE][16];
        for (int p = 0; p < NPHASE; p++) {
            if (!seen[p][i]) snprintf(cell[p], 16, "-");
            else snprintf(cell[p], 16, "%ld..%ld", lo[p][i], hi[p][i]);
        }
        const char *concl = "?";
        if (moves[0]) concl = "COUNTER (moves idle)";
        else if (moves[1] && !moves[2] && !moves[3]) concl = "*** STEERING ***";
        else if (moves[2] && !moves[1] && !moves[3]) concl = "*** THROTTLE ***";
        else if (moves[3] && !moves[1] && !moves[2]) concl = "*** BRAKE ***";
        else concl = "shared/unclear";
        printf("%-6d %-14s %-14s %-14s %-14s  %s\n", i,
               cell[0], cell[1], cell[2], cell[3], concl);
    }
    return 0;
}
