# Wiring

One firmware (`cw_station/`) runs every board. A board is a display station or
half of a wireless pair depending on how A2 is jumpered.

## Rule for every input

All buttons, switches, jumpers and the straight key wire between their pin and
GND. Nothing goes to +5V, and no input needs a resistor - the internal pull-ups
are on (`INPUT_PULLUP`).

| Pin state      | Reads  | Means       |
| -------------- | ------ | ----------- |
| open / unwired | `HIGH` | idle, off   |
| closed to GND  | `LOW`  | pressed, on |

---

## Pin map - I2C builds

### Inputs (one leg to the pin, one leg to GND)

| Pin | Component             | `LOW` means                           |
| --- | --------------------- | ------------------------------------- |
| D3  | Straight key          | key down                              |
| D6  | TEXT MODE switch      | decoded text (open = raw dots/dashes) |
| D7  | CLEAR button          | soft restart                          |
| A2  | STATION MODE switch   | wireless unit (open = display unit)   |
| A0  | PAIR select, high bit | wireless only                         |
| A1  | PAIR select, low bit  | wireless only                         |

A0-A2 are analogue pins used as plain digital inputs.

### Outputs

| Pin | Component                 | Wiring                  |
| --- | ------------------------- | ----------------------- |
| D2  | Piezo buzzer              | other leg to GND        |
| D4  | Yellow LED - local keying | 220R-1k resistor to GND |
| D5  | Red LED - remote keying   | 220R-1k resistor to GND |

LED long leg (anode) to the pin, short leg to the resistor.

### LCD (I2C backpack)

| Arduino | Backpack |
| ------- | -------- |
| 5V      | VCC      |
| GND     | GND      |
| A4      | SDA      |
| A5      | SCL      |

### Radio NRF24L01 (wireless only)

| Arduino           | Module |
| ----------------- | ------ |
| **3.3V - not 5V** | VCC    |
| GND               | GND    |
| D8                | CSN    |
| D9                | CE     |
| D11               | MOSI   |
| D12               | MISO   |
| D13               | SCK    |

5V on VCC destroys the module. The data pins are 5V tolerant. A 10uF capacitor
across the module's VCC and GND fixes most link trouble.

### D10

Leave empty. It is the SPI slave-select pin, and pulling it low drops the SPI
hardware out of master mode, which stops the radio. The parallel build has no
radio and does use D10.

---

## Switch positions

**STATION MODE (A2)** - open = DISPLAY (local key, no radio), GND = WIRELESS.
Read at boot, so changing it needs a reset.

**TEXT MODE (D6)** - open = raw dots/dashes, GND = decoded text. Polled live;
flipping it re-renders what is already on screen.

**PAIR select (A0, A1)** - wireless only, read at boot. Nothing fitted = pair 1.

| A0   | A1   | Pair | Channel |
| ---- | ---- | ---- | ------- |
| open | open | 1    | 76      |
| open | GND  | 2    | 40      |
| GND  | open | 3    | 8       |
| GND  | GND  | 4    | 24      |

Side A/B is not a jumper - tap the key during the boot banner to flip it, stored
in EEPROM. Two boards link when they are on the same pair and opposite sides.

---

## Rewiring existing display boards

The old display firmware read the key active-high, so those boards have the key
wired to +5V with a pull-down resistor on D3.

- Move the key's second leg from +5V to GND.
- Remove the pull-down resistor on D3.
- CLEAR button moves D5 -> D7. D5 becomes the red LED.

Skipping the key rewire leaves the unit reading key-down permanently: solid
tone, LED stuck on, nothing decodes.

Wireless boards need no changes.

---

## 40x4 over I2C

A 40x4 panel is two HD44780 controllers - rows 0-1 on E1, rows 2-3 on E2. A
PCF8574 backpack has no spare output for E2, so the panel's R/W is tied to GND
(write only) and the backpack's R/W output drives E2 instead.

| Backpack pin | Panel pin          |                  |
| ------------ | ------------------ | ---------------- |
| 1 GND        | 1 VSS              |                  |
| 2 +5V        | 2 VDD              |                  |
| 3 V0         | 3 V0               | contrast trimpot |
| 4 RS         | 4 RS               |                  |
| 5 R/W        | **15 E2**          | the re-route     |
| 6 E          | 6 E1               |                  |
| 7-10 D0-D3   | not connected      | 4-bit mode       |
| 11-14 D4-D7  | 11-14 D4-D7        |                  |
| 15 LED+      | backlight positive |                  |
| 16 LED-      | backlight negative |                  |

Plus one wire the backpack does not provide: **panel pin 5 (R/W) -> GND**.

Panel pinouts vary between manufacturers - check the datasheet. RS, R/W, E1,
D4-D7 and E2 are the ones that matter.

---

## Bring-up

1. `tools/i2c_scanner` - note the address (usually 0x27 or 0x3F), set
   `LCD_I2C_ADDR`.
2. `tools/lcd_test_i2c_40x4` - all four rows must be readable.
   - top two only = E2 not connected
   - bottom two only = E1 and E2 swapped
   - solid blocks = R/W not at GND, or contrast needs adjusting
   - blank with backlight = contrast
3. Set `LCD_BACKEND` and flash `cw_station`.

| Display       | `LCD_BACKEND`   |
| ------------- | --------------- |
| 40x4 on I2C   | `LCD_I2C_40X4`  |
| 20x4 on I2C   | `LCD_I2C_20X4`  |
| 40x4 parallel | `LCD_FAST_40X4` |

On boot: banner for 3s naming the mode (`Display CW` / `Wireless CW`), then the
live screen. Wireless units show `Unit 1A` and accept a key tap to flip side,
confirmed by one beep for A, two for B. `RADIO FAULT` with both LEDs flashing
means the module is missing or miswired; it retries until fixed.

---

## Parallel 40x4 build (`LCD_FAST_40X4`)

Nine pins for the panel leaves no room for a radio, so this build is display
mode only.

| Pin    | Connects to           |
| ------ | --------------------- |
| D2     | Buzzer                |
| D3     | Straight key          |
| D4     | LCD E1                |
| D5     | LCD RS                |
| D6     | LCD R/W               |
| D7     | Yellow LED            |
| D8     | LCD E2                |
| D9-D12 | LCD D7, D6, D5, D4    |
| A0     | CLEAR button          |
| A1     | TEXT MODE switch      |
| A2     | Red LED (unused here) |

**Known issue:** this backend shows solid blocks instead of text, unresolved.
Not the R/W pin - a known-working reference sketch uses the same RW=D6 wiring,
but it only ever writes rows 0-1 (E1) and never calls `clear()`. Isolate with
`tools/lcd_test_40x4`.

---

## Board

ATmega328 Nano. The firmware uses 39% of flash and 34% of RAM. It fits an
ATmega168 at 85%/69%, which is too thin to rely on.
