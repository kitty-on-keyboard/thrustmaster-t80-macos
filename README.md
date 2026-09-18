# Thrustmaster T80 on macOS

The T80 enumerates on macOS, reports its buttons, and reports **nothing at all**
for the wheel and pedals. Every controller mapping, every rebinding UI and every
"just add an SDL mapping" answer fails on it, for a reason none of them can fix.

This repo explains why, gives you the byte offsets, and includes a ~130-line
bridge that makes the wheel usable.

---

## Why it looks broken

The T80 publishes a perfectly ordinary gamepad HID descriptor. It declares six
Generic-Desktop analogue axes — `X`, `Y`, `Z`, `Rx`, `Ry`, `Rz` — plus a hat and
buttons, all in one 64-byte input report (report ID 1).

Those declared axes never move. Turn the wheel lock to lock, floor both pedals,
and they sit at their rest values forever:

```
X=128  Y=128  Z=128  Rz=128  Hat=8  Rx=0  Ry=0
```

The buttons in that same report *do* change. So the device is alive, the report
is arriving, and IOHID is parsing it — the analogue fields it advertises are
simply dead. Apple still sets `GameControllerSupportedHIDDevice=No`, so Godot
and SDL's joypad list can be empty while the wheel is on USB. Buttons then
never become gamepad events either, which is why they ride the same UDP packet
as the axes.

The real data is in the **vendor section of the same report**, which no generic
driver reads, because nothing in the descriptor says what it is.

This is why a controller mapping cannot help. A mapping is a translation table:
it renames "raw axis 3" to "left trigger". It cannot manufacture a value for an
axis that never reports one.

## The decode

Input report ID 1, 64 bytes. Little-endian `uint16`:

| bytes | control | rest | full travel | notes |
| --- | --- | --- | --- | --- |
| 43–44 | **steering** | ~32900 | 0 … 65535 | centre is near but not exactly 0x8000 |
| 45–46 | **throttle** | 65535 | → 0 | inverted: 65535 released, 0 floored |
| 47–48 | **brake** | 65535 | → 0 | inverted, same as throttle |
| 5, low nibble | **hat** | 8 | 0=N … 7=NW | same packing as Linux hid-t80 |
| 5, high nibble | **face** | 0 | bits 0–3 | Triangle, Cross, Circle, Square |
| 6 | **shoulders / system** | 0 | bits 0–7 | L1 R1 L2 R2 Select Start L3 R3 |

Steering is 16-bit, so you get considerably finer resolution than the 8-bit axis
the descriptor advertises.

Verified by logging 5938 reports across a 150-second session: steering sweeps
the full range smoothly and returns to centre, both pedals sit pinned at 65535
and drop to 0 under a foot, and no other byte in the report changes except a
noisy low byte on the steering (a potentiometer doing what potentiometers do).

## Quick start

Needs only the Xcode command line tools (`xcode-select --install`).

```sh
git clone https://github.com/kitty-on-keyboard/thrustmaster-t80-macos.git
cd thrustmaster-t80-macos/src
./build.sh
./t80_bridge -v
```

Keep your hands off the wheel for the first second — it averages the first 60
samples to find centre, because the rest position is not exactly 0x8000 and a
fixed centre puts a permanent lean on the steering.

It then sends `steer throttle brake buttons hat` as plain text to
`127.0.0.1:7654` at about 40 Hz:

```
-0.4213 0.0000 0.8817 0 8
```

`steer` is −1 … +1, `throttle` and `brake` are 0 … 1, `buttons` is a 12-bit mask
(bit 0 = Triangle … bit 11 = R3), `hat` is 0–7 or 8 for neutral. Older readers
that only split three floats still work if they accept `size() >= 3`. Text
rather than packed floats so you can watch the stream with `nc -ul 7654`
while debugging.

Consuming it is a few lines in anything. Godot, for example:

```gdscript
var udp := PacketPeerUDP.new()
udp.bind(7654, "127.0.0.1")

func _process(_delta):
    while udp.get_available_packet_count() > 0:
        var p := udp.get_packet().get_string_from_utf8().split(" ", false)
        if p.size() >= 3:
            steer = float(p[0]); throttle = float(p[1]); brake = float(p[2])
        if p.size() >= 5:
            buttons = int(p[3]); hat = int(p[4])
```

If your engine has an input system, replay these as synthetic joypad events
on device `-1` (every device). Pinning device `0` is dropped when Godot's
joypad list is empty, which it is for this wheel: Apple sets
`GameControllerSupportedHIDDevice=No`, so SDL/Godot never enumerate it even
though IOHID can see it. That is also why buttons have to ride this socket —
the HID gamepad path does not reach the engine.

## Decoding your own wheel

The interesting part of this repo is not the T80 numbers, it's the method. If
you have a different wheel that enumerates and does nothing, the same two tools
will find its offsets. Neither is T80-specific — pass any USB vendor ID.

Find what's attached:

```sh
ioreg -c IOHIDDevice -r -d 1 -l | grep -E '"Product"|"VendorID"|"ProductID"'
```

**1. Does anything in the raw report change?** This dumps every input report and
reports which byte offsets ever moved. Press a button first — that's your
control: if a button press moves a byte, the tool works, and a wheel that still
moves nothing is genuinely silent.

```sh
./hid_report_dump 0x044F 30
```

**2. Which byte is which control?** This walks you through four timed phases —
hands off, wheel only, throttle only, brake only — and reports which bytes moved
in which phase. A byte that changes while you are not touching anything is a
packet counter, not an input, which is what the idle phase is for.

```sh
./hid_phase_map 0x044F
```

Then read the moving bytes as `uint16` little-endian and check the shape: a
steering axis sweeps smoothly and returns to centre, a pedal sits at one
extreme and travels to the other.

### What made this hard

Two things, in case they save you the same hours:

- **The declared axes are a decoy.** I spent a long time measuring `X`/`Y`/`Z`
  through the parsed HID element API, concluding the device sent nothing. It was
  sending everything, one layer down. Dump raw report bytes, not parsed elements.
- **Zero events is ambiguous.** A blocked reader and a silent device produce
  identical output. Always include a control you can trigger — a button press —
  so silence means something.

## Caveats

- Tested on **exactly one T80**, on one firmware, on Apple Silicon (macOS 26).
  I have no way to know how far the offsets generalise across T80 revisions, let
  alone to the T150/T248/TMX.
- Buttons and the hat are in the same report (bytes 5–6). They used to be
  left to the HID gamepad path; on macOS that path is dead for this VID/PID
  because the device is not a Game Controller framework device. The bridge
  therefore emits them too.
- No force feedback. The T80 has none.
- Steering deadzone is a fixed ±0.012 to kill low-byte sensor noise. Adjust in
  `t80_bridge.c` if your unit is quieter or noisier.
- The bridge reconnects if you unplug the wheel. It is still a separate
  process, and `IOHIDManagerOpen` needs Input Monitoring if macOS asks.

## Contributing

If you run the two diagnostics on another Thrustmaster and get offsets — the
same ones or different ones — please open an issue with the model, the firmware
if you can get it, and the phase-map output. A table covering more than one
wheel would be worth much more than this one.

## Licence

MIT. See [LICENSE](LICENSE).
