# Fog machine BLE protocol

What the Ulanzi Filmog Ace (FM01) fog machine speaks over BLE, as established
by captures and attended tests (October 2026). `include/fogger_protocol.h`
implements it and `test/test_protocol/` tests it; keep the three in step.

## Link

- The machine advertises as `CH9141BLE2U` (a CH9141 BLE-to-UART bridge) with a
  public address. It is the peripheral; the controller connects as central.
- No pairing or bonding. MTU 131 was negotiated in testing.
- Service `0xFFF0`:
  - `0xFFF1`, notify: the status stream (enable its CCCD).
  - `0xFFF2`, write (Write Request): commands.
- The original remote reconnects automatically and takes the link, so switch it
  off while the controller is in use.

## Commands

Written to `0xFFF2`. No checksum, terminator or initialisation sequence.

| Action | Bytes | Notes |
|---|---|---|
| Fog start | `55 aa 00` | Starts fog, pump and fan. Physically verified. |
| Fog stop | `55 aa 02` | Stops all output. Physically verified. |
| Density | `55 aa 04 <u32 big-endian>` | 8000..40000. The remote's min / middle / max are 8000 / 25000 / 40000. |
| Fan | `55 aa 05 <fan> 64` | `fan` 35..100. The remote's min / middle / max are 35 / 61 / 100. The second byte is always 100 (`0x64`). |

Always stop with an explicit `55 aa 02`. Nothing shows the machine stops when
the link drops, so don't rely on a disconnect.

Present in the machine's firmware but **not** controls to use:

- `01`: an untested mode / timing candidate.
- `03 <0|1>`: changes status byte 3 and the LED colour; no fog was observed.
  It is not a stop.
- `06`, `08`: timing / control fields; untested.
- `07 <u16>`: overwrites the fault-report field; not a control.
- `09`: resets a liveness counter (a keepalive candidate). Short runs didn't
  need it.

## Status frames

Notifications on `0xFFF1` carry 20-byte frames, `aa 55 14`, 14 data bytes,
then `ff ff ff`. A notification can hold part of a frame or several frames, so
reassemble from the byte stream and resync on `aa 55 14`
(`fogger::StreamReassembler`).

| Offset | Field |
|---|---|
| 0..2 | `aa 55 14` header |
| 3 | The command-03 field. **Not** the fog state. |
| 4..7 | Density, u32 big-endian |
| 8 | Fan |
| 9 | Second fan parameter (100) |
| 10..11 | Fault report, u16 big-endian; 0 = no fault |
| 12, 13 | Battery A, B: the machine's estimated percent (which is left / right is unconfirmed) |
| 14 | Charger connected (0 / 1) |
| 15 | Charge complete (0 / 1); can stay 1 after the charger is unplugged |
| 16 | **Output active**: 1 while fogging. This is the real fog indicator. |
| 17..19 | `ff ff ff` trailer |

## Not yet verified

- Long runs: whether the machine needs a keepalive (`09`).
- What the machine does on its own when the link drops mid-fog. The controller
  sends a stop as soon as it reconnects.
- The actual output at each density / fan level.
- The original remote's own link (roles and payloads were never captured).
