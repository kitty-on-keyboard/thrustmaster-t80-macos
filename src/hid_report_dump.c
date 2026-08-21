// Dump the RAW input report and report which byte offsets ever change.
//
// The parsed Generic-Desktop axes are flat, but report 1 also carries 54 bytes
// of vendor data. If the wheel's real position lives in there, the standard
// axes would sit at their rest values forever while the bytes underneath moved
// -- which is precisely the symptom. A button press is the control: if pressing
// a button changes a byte here, the instrument works and any flat wheel result
// is real.
#include <IOKit/hid/IOHIDManager.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXLEN 128
static uint8_t base[MAXLEN];
static int have_base = 0;
static int changed[MAXLEN];
static long lo[MAXLEN], hi[MAXLEN];
static int reports = 0;

static void on_report(void *ctx, IOReturn r, void *sender, IOHIDReportType type,
                      uint32_t id, uint8_t *rep, CFIndex len) {
    reports++;
    if (len > MAXLEN) len = MAXLEN;
    if (!have_base) {
        memcpy(base, rep, len); have_base = 1;
        for (int i = 0; i < len; i++) { lo[i] = hi[i] = rep[i]; }
        printf("first report: id=%u len=%ld bytes:", id, (long)len);
        for (int i = 0; i < len && i < 24; i++) printf(" %02x", rep[i]);
        printf("\n"); fflush(stdout);
        return;
    }
    for (int i = 0; i < len; i++) {
        if (rep[i] < lo[i]) lo[i] = rep[i];
        if (rep[i] > hi[i]) hi[i] = rep[i];
        if (rep[i] != base[i]) changed[i] = 1;
    }
}

int main(int argc, char **argv) {
    double secs = argc > 2 ? atof(argv[2]) : 30.0;
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
    printf("watching raw reports for %.0fs.\n"
           "  1) PRESS A BUTTON a few times  (proves this tool works)\n"
           "  2) TURN THE WHEEL lock to lock\n"
           "  3) FLOOR BOTH PEDALS\n\n", secs);
    fflush(stdout);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, secs, false);
    printf("\n%d report(s) received\n", reports);
    if (!reports) { printf("NO REPORTS AT ALL -- tool is blocked, result is meaningless.\n"); return 2; }
    int n = 0;
    printf("\nbyte offsets that changed:\n");
    for (int i = 0; i < MAXLEN; i++)
        if (changed[i]) { printf("  byte %-3d  range %ld..%ld\n", i, lo[i], hi[i]); n++; }
    if (!n) printf("  NONE -- every byte in the report was constant.\n");
    return 0;
}
