# Hardwired pair selection - design notes

> **Historical.** These are the notes from the change that moved pair selection
> onto jumpers; the work is done and shipped. Kept for the reasoning, not as a
> reference. **[docs/WIRING.md](WIRING.md) is the authoritative pin map** and
> wins wherever the two disagree - in particular the select pins are read
> _inverted_ (so an unjumpered board is pair 1, not pair 4), and the text-mode
> pin is now polled live rather than sampled once at boot.

Goal: stop choosing the radio pair by tapping the key at boot, and instead set it
with jumpers/switches on the board. Two select pins pick one of four pairs; a
third pin (the existing pin 6) picks text vs CW display. The only thing still
chosen in software is which half of the pair this board is (A or B), stored in
EEPROM.

## New naming

Drop the flat 1..8 unit numbering. A board is identified by its **pair** (1-4)
plus its **side** (A or B):

| Old unit | New name | Pair | Side |
| -------- | -------- | ---- | ---- |
| 1        | 1A       | 1    | A    |
| 2        | 1B       | 1    | B    |
| 3        | 2A       | 2    | A    |
| 4        | 2B       | 2    | B    |
| 5        | 3A       | 3    | A    |
| 6        | 3B       | 3    | B    |
| (new) 7  | 4A       | 4    | A    |
| (new) 8  | 4B       | 4    | B    |

Two boards talk only if they share a pair and are on opposite sides (1A <-> 1B).

## Pair select pins (a, b)

Two digital pins choose the pair, on **A0 (a) and A1 (b)**, used as plain digital
inputs. Wired `INPUT_PULLUP` + jumper to GND (no extra parts): a pin reads HIGH
unwired and LOW when jumpered. Truth table:

| pin a | pin b | pair           |
| ----- | ----- | -------------- |
| low   | low   | 1 (channel 76) |
| low   | high  | 2 (channel 40) |
| high  | low   | 3 (channel 8)  |
| high  | high  | 4 (channel 24) |

Because `digitalRead` gives LOW=0/HIGH=1, this is just
`pairIndex = (digitalRead(a) << 1) | digitalRead(b)` - no inversion needed, pin a
is the high bit. Note the default (nothing jumpered) is HIGH/HIGH = **pair 4**;
jumper both pins to GND for pair 1.

`PAIR_CHANNELS` and `ADDRESS` both grow to 4 pairs (8 pipe addresses). The pins
are read **once at boot** (jumpers don't change while running) and select the
channel + pipe pair, replacing the key-tap pair cycling.

## Side select (A / B)

Still soft-chosen by tapping the key during the boot banner, but now it only
toggles between two states (A and B) instead of cycling 1..N. Stored in EEPROM
as a single bool (0 = A, 1 = B). One confirmation beep for A, two for B.

A is the low-pitch side (600 Hz local / 900 Hz remote); B is the reverse - same
as the current odd/even behaviour.

## Mode pin (text vs CW) - pin 6

Pin 6 (`MODE_SWITCH_PIN`) already exists but is currently disabled
(`MODE_SWITCH_ENABLED 0`). Enable it so the pin selects raw dots/dashes vs
decoded text. Read **once at boot** like the pair jumpers (no live toggling) -
LOW (jumpered to GND) = decoded text, HIGH (unwired) = raw dots/dashes.

## Free pins on the Nano

Already used: D2, D3, D4, D5, D6, D7, D8, D9, D11/D12/D13 (SPI), A4/A5 (I2C),
D0/D1 (serial debug). Free and usable as digital inputs: **D10, A0, A1, A2, A3**.
(A6/A7 are analog-input only and cannot be digital inputs.)

## Decisions (resolved)

1. **Logic level / default.** `INPUT_PULLUP` + jumper to GND, no extra parts.
   Default (nothing wired) = HIGH/HIGH = pair 4; jumper to GND for the rest.
2. **Select pins.** a = A0, b = A1.
3. **Pair 4's channel.** 24 (2424 MHz). `PAIR_CHANNELS = {76, 40, 8, 24}`.
4. **Mode pin behaviour.** Read once at boot (no live toggle).
5. **Header label format.** "1A CW" / "1A CW TEXT" (same width as the old
   "U1 CW", so the link glyph column is unchanged).
6. **EEPROM migration.** Address 0 now stores side only (0 = A, 1 = B). Any other
   value (e.g. an old unit number) is treated as A on first boot - no migration.

## Implementation checklist (done - commit 352b001 on branch hardwired-pair-select)

- [x] Add `PAIR_A_PIN A0`, `PAIR_B_PIN A1` to the pin map; `pinMode(..., INPUT_PULLUP)`.
- [x] Grow `ADDRESS` to 8 pipes and `PAIR_CHANNELS` to `{76, 40, 8, 24}`.
- [x] Read `pairIndex = (digitalRead(PAIR_A_PIN) << 1) | digitalRead(PAIR_B_PIN)` at boot.
- [x] Replace key-tap pair cycling with key-tap A/B toggle; store side bool in EEPROM.
- [x] Derive sidetone from side (A = 600/900, B = 900/600).
- [x] Enable mode pin, sampled once at boot.
- [x] Update banner + header to the "1A"/"1B" naming.
- [x] Bump version (now v2.2).

Verified with `arduino-cli compile --fqbn arduino:avr:nano`: 49% flash, 62% RAM.
