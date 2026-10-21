# Fog machine BLE protocol

What the Ulanzi Filmog Ace (FM01) fog machine speaks over BLE, as established
by captures of the machine and of its original remote, and by attended tests
(October 2026). `include/fogger_protocol.h` implements it and
`test/test_protocol/` tests it; keep the three in step. The evidence (captures,
dated notes) is in the ULFACE reverse-engineering repo.

## Link

- The machine advertises as `CH9141BLE2U` (a CH9141 BLE-to-UART bridge) with a
  public address. It is the peripheral; the controller connects as central,
  exactly as the original remote does.
- No pairing or encryption, also on the original remote's link. MTU 131.
- Service `0xFFF0`:
  - `0xFFF1`, notify: the status stream (enable its CCCD).
  - `0xFFF2`, write: commands. The remote uses Write Command, Wraith uses Write
    Request; the machine accepts both.
  - `0xFFF3`: the CH9141's own GPIO-sync channel. The remote polls it
    (`05 00 04 04 01 01 06`, reply `85 00 04 04 01 01 06`); fog runs fine
    without it.
- The original remote reconnects automatically and takes the link, so switch
  it off while the controller is in use.

## Commands

Written to `0xFFF2`. No checksum, terminator or initialisation sequence. One
write can carry several commands back to back.

| Action | Bytes | Notes |
|---|---|---|
| Fog start | `55 aa 00` | Starts fog, pump and fan. Physically verified. |
| Fog stop | `55 aa 02` | Stops all output. Physically verified. |
| Density | `55 aa 04 <u32 big-endian>` | 8000..40000. The remote's six levels: 8000, 15000, 25000, 30000, 35000, 40000. |
| Fan | `55 aa 05 <fan> 64` | `fan` 35..100. The remote's six levels: 35, 48, 61, 74, 87, 100. The second byte is always 100 (`0x64`). |
| Power off | `55 aa 02 55 aa 03 01` | Stop, then `03 01`, in one write: what the remote sends when it is switched off. Captured twice (once with `55 aa 08 2c 00` appended); both times the user found the machine off. Not yet tested from Wraith. See below. |

What the original remote sends for its buttons: a held Spray sends start, then
stop on release; a double-click sends start, and the next press sends stop.
Settings take effect within about 0.1 s. There is no separate "continuous"
command.

Always stop with an explicit `55 aa 02`; see "When the link drops" below.

### Power off (command `03`)

Command `03` writes status byte 3. What byte 3 means is not resolved; this is
what has been observed:

- **The remote's power-off.** After its `02` + `03 01` write, status frames
  carried on for about 7 s with byte 3 = 1, then ended, and the user found the
  machine off. In the second capture the sniffer then saw no machine
  advertising until the machine's own button powered it on again (an idle link
  loss advertises for about a minute first).
- **Byte 3 = 1 alone does not mean a switch-off is coming.** It is also set by
  `03 01` on its own (the LED turns red), by the machine's own power control,
  and right after a connection. Under Wraith it went back to 0 by itself a few
  seconds after connecting, with no `03` sent and no switch-off.
- **`03 00` clears it.** The remote sends it right after connecting. When the
  machine's own power control was used with the remote connected, the remote
  answered with `03 00` and the machine appeared to turn itself back on.
- **Not a fog control.** A 1 s `03 01` followed by `03 00` on an idle machine
  only flashed the LED red. Don't use `03` to start fog, nor as a stop.
- **No wake.** Nothing has woken a switched-off machine over BLE so far, so
  treat a power-off as one-way: the machine's own button brings it back.

Wraith sends the 7-byte form as one Write Request (the remote uses Write
Command; the machine accepts both). It never sends `03 00`, so the machine's own
power control should work while Wraith is connected; that is not yet tested.

Seen from the remote, or present in the machine's firmware, but **not**
controls to use:

- `08 <byte> 00`: the remote sends it about every 0.5 s; meaning not decoded.
  Not needed: fog runs of 30 s and 90 s without it behaved normally.
- `01`, `06`: untested mode / timing fields.
- `07 <u16>`: overwrites the machine report (below); not a control, and never
  to be used to clear a report.
- `09`: resets a firmware counter that also has a hardware input; never sent by
  the remote, and not needed in the tests.

## Status frames

Notifications on `0xFFF1` carry 20-byte frames, `aa 55 14`, 14 data bytes,
then `ff ff ff`. A notification can hold part of a frame or several frames, so
reassemble from the byte stream and resync on `aa 55 14`
(`fogger::StreamReassembler`). Frames arrive every 0.1-0.25 s.

| Offset | Field |
|---|---|
| 0..2 | `aa 55 14` header |
| 3 | The command-03 field; meaning unresolved (see "Power off"). **Not** the fog state. |
| 4..7 | Density, u32 big-endian |
| 8 | Fan |
| 9 | Second fan parameter (100) |
| 10..11 | Machine report, u16 big-endian; 0 = nothing to report (below) |
| 12, 13 | Battery A, B: the machine's estimated percent (which is left / right is unconfirmed) |
| 14 | Charger connected (0 / 1) |
| 15 | Charge complete (0 / 1); can stay 1 after the charger is unplugged |
| 16 | **Output active**: 1 while fogging. This is the real fog indicator; output switches within ~0.65 s of a command. |
| 17..19 | `ff ff ff` trailer |

### Machine report (offsets 10..11)

A latched **last-event** word, not live state: the machine copies each batch of
events here and never clears it (only a later event, or command `07`, changes
it). Bits can combine. Decoded from the machine firmware (ULFACE
`artifacts/ble/report-word-analysis-2026-10-21.md`); only `0x0200` has been
matched to a physical event so far.

| Bit | Meaning | Effect on fog | Confidence |
|---|---|---|---|
| `0x0001` | Low battery | Refuses / stops | high |
| `0x0002` | Battery reading invalid (cell missing?) | Refuses | medium |
| `0x0004` | Atomizer short (< ~0.22 Ω) or over-current | Refuses / stops | high |
| `0x0008` | Oil tank missing (open load) | Refuses / stops | medium-high |
| `0x0020` | Over-temperature | Refuses / stops | medium-high |
| `0x0080` | Battery imbalance (> 499 mV between cells) | Refuses | high |
| `0x0100` | Dry burn (coil overheating, oil likely empty) | Stops | medium |
| `0x0200` | **Oil tank connected and recognised.** Seen after reinserting the batteries, while the original remote showed "oil tank connected". | None: informational | high |
| `0x0400` | Oil tank not recognised (load outside ~0.29-0.40 Ω) | Refuses | high (logic) |
| others | No setter in this firmware | — | — |

Notes: removing the tank raises no event (it shows up as `0x0008` at the next
start); events raised while charging are not reported; the machine re-checks
every condition at each start, so a latched fault bit doesn't mean the fault
still holds.

## When the link drops

- **The machine does not stop at once.** When the controller lost power
  mid-fog, the machine kept fogging for about a minute, then stopped (and
  switched off) by itself, in two attended trials. Nothing can send a stop
  then, so treat a controller reset or power loss as about a minute of fog.
- A normal disconnect sends `55 aa 02` first and stops promptly.
- After its batteries are removed and reinserted, the machine comes back idle;
  the controller sends a stop on reconnect and does not restart a program.
- A switched-off machine is not woken by the remote's power-on, and waking it
  over BLE from Wraith has not been shown either. Power it on with its own
  button.

## Not yet verified

- Runs longer than 90 s (no keepalive was needed up to then).
- Power-off sent by Wraith (only the remote's has been captured).
- The machine's own power control while Wraith is connected (Wraith never sends
  the remote's `03 00`, so it should switch the machine off).
- The actual output at each density / fan level.
- The machine report bits other than `0x0200` against physical events (their
  meanings come from static analysis).
