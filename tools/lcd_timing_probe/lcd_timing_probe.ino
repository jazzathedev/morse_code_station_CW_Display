/**
 * 20x4 I2C LCD timing probe - for the old panel that shows gibberish with our
 * driver but works with the HD44780_LCD_PCF8574 library.
 *
 * Theory under test: the wiring and pin map are fine (gibberish, not blank
 * blocks, means the controller IS hearing us) but one of our init timings
 * doesn't suit this older, slower controller. Each STEP below initialises the
 * panel a different way using the SAME stock-backpack bit map our station
 * firmware uses, then draws the same test pattern. You watch the LCD, note
 * which STEP reads correctly, and report back - the winner's numbers get baked
 * into LiquidCrystalI2C_Dual so no new library is needed.
 *
 *   STEP 0 BASELINE - our exact current init (expect gibberish here)
 *   STEP 1 LONGWAIT - same, but 1500ms power-on wait (LiquidCrystal_I2C style)
 *   STEP 2 GAVIN    - HD44780_LCD_PCF8574's exact init framing + 5ms gaps
 *   STEP 3 DOUBLE   - our init run twice (re-sync from a stale 4-bit state)
 *   STEP 4 SLOWEXEC - our init with 250us settle + 5ms clear (slow oscillator)
 *   STEP 5 SLOWBUS  - our init at 50kHz I2C (marginal backpack/bus)
 *   STEP 6 PATIENT  - long wait + double init + slow exec, everything at once
 *
 * How to run:
 *   1. Wire the problem 20x4 + backpack as usual (SDA->A4, SCL->A5, 5V, GND).
 *   2. Flash this sketch, open Serial Monitor at 9600.
 *   3. For the cleanest result: unplug USB, wait 5s (lets the LCD fully power
 *      down), replug, then press any key in Serial Monitor to start.
 *   4. Each step shows for 8 seconds. The monitor prints exactly what the LCD
 *      SHOULD show - compare character for character.
 *   5. Keys: 0-6 = jump to a step and hold it, c = resume cycling.
 *
 * Report back: the FIRST step number that reads perfectly, plus one word for
 * each broken step (blocks / gibberish / blank / fades / flickers).
 */

#include <Wire.h>

// --- Stock PCF8574 backpack bit map (same as the station firmware) -----------
#define BP_RS 0x01 // P0 - register select
#define BP_RW 0x02 // P1 - R/W, always driven low (= write)
#define BP_EN 0x04 // P2 - enable strobe
#define BP_BL 0x08 // P3 - backlight

// --- HD44780 instructions -----------------------------------------------------
#define LCD_CLEARDISPLAY 0x01
#define LCD_ENTRYMODESET 0x04
#define LCD_DISPLAYCONTROL 0x08
#define LCD_FUNCTIONSET 0x20
#define LCD_SETDDRAMADDR 0x80
#define LCD_ENTRYLEFT 0x02
#define LCD_DISPLAYON 0x04
#define LCD_2LINE 0x08

// Standard 20x4 DDRAM row starts (same in all three libraries compared).
static const uint8_t ROW_ADDR[4] = {0x00, 0x40, 0x14, 0x54};

#define NUM_STEPS 7
#define DWELL_MS 8000

uint8_t lcdAddr = 0x27;
int8_t holdStep = -1; // >= 0 while the user is holding one step

// --- Init under test ----------------------------------------------------------
struct InitCfg
{
  const char *name;
  uint16_t powerWaitMs;   // park-expander -> first nibble
  uint16_t d1, d2, d3, d4; // gaps in the init dance (us, except GAVIN: ms*1000)
  uint16_t nibbleSettleUs; // post-enable-pulse settle
  uint16_t clearMs;
  uint8_t doubleInit;
  uint8_t gavinStyle;     // full-byte 0x02 x3 framing, single-burst I2C
  uint32_t i2cHz;
};

static const InitCfg PRESETS[NUM_STEPS] = {
    {"BASELINE", 50, 4500, 4500, 150, 150, 50, 2, 0, 0, 100000},
    {"LONGWAIT", 1500, 4500, 4500, 150, 150, 50, 2, 0, 0, 100000},
    {"GAVIN", 15, 5000, 5000, 5000, 0, 0, 5, 0, 1, 100000},
    {"DOUBLE", 50, 4500, 4500, 150, 150, 50, 2, 1, 0, 100000},
    {"SLOWEXEC", 50, 4500, 4500, 150, 150, 250, 5, 0, 0, 100000},
    {"SLOWBUS", 50, 4500, 4500, 150, 150, 50, 2, 0, 0, 50000},
    {"PATIENT", 1500, 9000, 9000, 300, 300, 250, 5, 1, 0, 100000},
};

// --- Low-level bus access -----------------------------------------------------

void expanderWrite(uint8_t data)
{
  Wire.beginTransmission(lcdAddr);
  Wire.write(data | BP_BL);
  Wire.endTransmission();
}

void pulseEnable(uint8_t data, uint16_t settleUs)
{
  expanderWrite(data | BP_EN);
  delayMicroseconds(1);
  expanderWrite(data & ~BP_EN);
  delayMicroseconds(settleUs);
}

// One nibble (already in the high 4 bits), one I2C transaction per edge.
void write4bits(uint8_t nibble, uint16_t settleUs)
{
  expanderWrite(nibble);
  pulseEnable(nibble, settleUs);
}

// Full byte, high nibble first (classic split-transaction style).
void sendByteClassic(uint8_t value, uint8_t rsBit, uint16_t settleUs)
{
  write4bits((value & 0xF0) | rsBit, settleUs);
  write4bits(((value << 4) & 0xF0) | rsBit, settleUs);
}

// Full byte in ONE I2C burst, no inter-nibble delays (Gavin Lyons style).
void sendByteBurst(uint8_t value, uint8_t rsBit)
{
  uint8_t hi = (value & 0xF0) | rsBit | BP_BL;
  uint8_t lo = ((value << 4) & 0xF0) | rsBit | BP_BL;
  uint8_t buf[4] = {(uint8_t)(hi | BP_EN), hi, (uint8_t)(lo | BP_EN), lo};
  Wire.beginTransmission(lcdAddr);
  Wire.write(buf, 4);
  Wire.endTransmission();
}

static uint16_t gSettle = 50;
static uint8_t gBurst = 0;

void cmd(uint8_t v)
{
  if (gBurst)
    sendByteBurst(v, 0);
  else
    sendByteClassic(v, 0, gSettle);
}

void data(uint8_t v)
{
  if (gBurst)
    sendByteBurst(v, BP_RS);
  else
    sendByteClassic(v, BP_RS, gSettle);
}

// --- Init sequences -----------------------------------------------------------

// Our current dance: three 0x30 nibbles then 0x20, all single-nibble.
void initDanceClassic(const InitCfg &c)
{
  write4bits(0x30, c.nibbleSettleUs);
  delayMicroseconds(c.d1);
  write4bits(0x30, c.nibbleSettleUs);
  delayMicroseconds(c.d2);
  write4bits(0x30, c.nibbleSettleUs);
  delayMicroseconds(c.d3);
  write4bits(0x20, c.nibbleSettleUs);
  delayMicroseconds(c.d4);
}

// Gavin Lyons dance: full-byte 0x02 three times with 5ms gaps.
void initDanceGavin(const InitCfg &c)
{
  sendByteBurst(0x02, 0);
  delay(c.d1 / 1000);
  sendByteBurst(0x02, 0);
  delay(c.d2 / 1000);
  sendByteBurst(0x02, 0);
  delay(c.d3 / 1000);
  sendByteBurst(0x20, 0);
}

void runInit(uint8_t step)
{
  const InitCfg &c = PRESETS[step];
  Wire.setClock(c.i2cHz);
  gSettle = c.nibbleSettleUs;
  gBurst = c.gavinStyle;

  for (uint8_t pass = 0; pass <= c.doubleInit; pass++)
  {
    if (pass > 0)
      delay(100);
    expanderWrite(0); // park everything low, backlight rides along
    delay(c.powerWaitMs);

    if (c.gavinStyle)
      initDanceGavin(c);
    else
      initDanceClassic(c);

    cmd(LCD_FUNCTIONSET | LCD_2LINE); // 0x28, 4-bit, 2 lines, 5x8
    cmd(LCD_DISPLAYCONTROL | LCD_DISPLAYON); // 0x0C
    cmd(LCD_CLEARDISPLAY);
    delay(c.clearMs);
    cmd(LCD_ENTRYMODESET | LCD_ENTRYLEFT); // 0x06
    if (c.gavinStyle)
      delay(5);
  }
}

// --- Test pattern (identical for every step) ----------------------------------

void setCursor20x4(uint8_t col, uint8_t row)
{
  cmd(LCD_SETDDRAMADDR | (ROW_ADDR[row & 3] + col));
}

void printStr(const char *s)
{
  while (*s)
    data((uint8_t)*s++);
}

void drawPattern(uint8_t step)
{
  char row0[21];
  snprintf(row0, sizeof(row0), "S%u %-8s READABLE", step, PRESETS[step].name);
  setCursor20x4(0, 0);
  printStr(row0);
  setCursor20x4(0, 1);
  printStr("ABCDEFGHIJ0123456789");
  setCursor20x4(0, 2);
  printStr("abcdefghij9876543210");
  setCursor20x4(0, 3);
  printStr("0123456789*#+-/.,?!:");
}

// --- Serial helpers -----------------------------------------------------------

void waitPoll(uint16_t ms)
{
  unsigned long t0 = millis();
  while (millis() - t0 < ms)
  {
    if (Serial.available())
      return;
    delay(10);
  }
}

void pollKeys()
{
  while (Serial.available())
  {
    char k = (char)Serial.read();
    if (k >= '0' && k < '0' + NUM_STEPS)
    {
      holdStep = k - '0';
      Serial.print(F(">>> HOLDING step "));
      Serial.print(holdStep);
      Serial.print(F(" "));
      Serial.println(PRESETS[holdStep].name);
    }
    else if (k == 'c' || k == 'C')
    {
      holdStep = -1;
      Serial.println(F(">>> resuming cycle"));
    }
  }
}

void announce(uint8_t step)
{
  Serial.println();
  Serial.print(F("=== STEP "));
  Serial.print(step);
  Serial.print(F(" "));
  Serial.print(PRESETS[step].name);
  Serial.println(F(" - look at the LCD now ==="));
  Serial.println(F("It SHOULD show exactly:"));
  char row0[21];
  snprintf(row0, sizeof(row0), "S%u %-8s READABLE", step, PRESETS[step].name);
  Serial.println(row0);
  Serial.println(F("ABCDEFGHIJ0123456789"));
  Serial.println(F("abcdefghij9876543210"));
  Serial.println(F("0123456789*#+-/.,?!:"));
}

void scanBus()
{
  Serial.println(F("Scanning I2C bus..."));
  uint8_t found[8];
  uint8_t n = 0;
  for (uint8_t a = 0x08; a < 0x78; a++)
  {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0)
    {
      Serial.print(F("  ACK at 0x"));
      Serial.println(a, HEX);
      if (n < 8)
        found[n++] = a;
    }
  }
  if (n == 0)
  {
    Serial.println(F("  nothing found! check wiring. Defaulting to 0x27."));
    lcdAddr = 0x27;
    return;
  }
  lcdAddr = found[0];
  for (uint8_t i = 0; i < n; i++)
  {
    if (found[i] == 0x27)
      lcdAddr = 0x27;
  }
  for (uint8_t i = 0; i < n; i++)
  {
    if (found[i] == 0x3F && lcdAddr != 0x27)
      lcdAddr = 0x3F;
  }
  Serial.print(F("Using 0x"));
  Serial.println(lcdAddr, HEX);
}

// --- Main ---------------------------------------------------------------------

void setup()
{
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(9600);
  Wire.begin();
  Wire.setWireTimeout(25000, true);

  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000)
    ; // wait for the monitor, max 3s
  Serial.println(F("20x4 LCD timing probe"));
  scanBus();
  Serial.println(F("Power-cycle the LCD now if you can (unplug USB, wait 5s,"));
  Serial.println(F("replug), then press any key to start. Keys 0-6 hold a"));
  Serial.println(F("step, c resumes cycling."));

  while (!Serial.available())
  {
    digitalWrite(LED_BUILTIN, (millis() / 250) & 1);
    delay(50);
  }
  while (Serial.available())
    Serial.read(); // flush
  digitalWrite(LED_BUILTIN, LOW);
  Serial.println(F("Starting. Each step shows for 8 seconds."));
}

void loop()
{
  static uint8_t step = 0;
  static uint8_t cycles = 0;

  if (holdStep >= 0)
  {
    runInit((uint8_t)holdStep);
    drawPattern((uint8_t)holdStep);
    announce((uint8_t)holdStep);
    Serial.println(F("(held - press c to resume)"));
    unsigned long t0 = millis();
    while (holdStep >= 0)
    {
      pollKeys();
      if (millis() - t0 > 2000) // redraw in case of a glitch
      {
        drawPattern((uint8_t)holdStep);
        t0 = millis();
      }
      delay(50);
    }
    return;
  }

  runInit(step);
  drawPattern(step);
  announce(step);
  waitPoll(DWELL_MS);
  pollKeys();

  step++;
  if (step >= NUM_STEPS)
  {
    step = 0;
    cycles++;
    Serial.println();
    Serial.print(F("--- cycle "));
    Serial.print(cycles);
    Serial.println(F(" done, repeating from STEP 0 ---"));
    Serial.println(F("Report: FIRST fully-readable step number + one word"));
    Serial.println(F("per broken step (blocks/gibberish/blank/flicker)."));
  }
}
