/**
 * Scouts WA Morse code station - one firmware for both boards.
 *
 * A single sketch that runs as either kind of station, picked by a switch read
 * at boot:
 *
 *   DISPLAY  - the local straight key is timed, decoded and shown on the LCD.
 *              No radio; this is the standalone "key it and watch it appear"
 *              unit. (STATION_MODE_PIN open.)
 *   WIRELESS - the local key is transmitted to a paired unit over an NRF24L01,
 *              and the remote unit's keying is what appears on the LCD.
 *              (STATION_MODE_PIN jumpered to GND.)
 *
 * A second switch (TEXT_MODE_PIN) picks how the keying is drawn: raw dots and
 * dashes, or decoded text for kids to read. It is polled live and the screen
 * re-renders from the buffer, so flipping it re-reads what is already up.
 *
 * In wireless mode up to four independent links can run at once: two select
 * jumpers hardwire the board's pair (channel + pipes), and a key tap at boot
 * picks which side (A or B) of that pair this board is. So 1A pairs with 1B,
 * 2A with 2B, and so on.
 *
 * Display hardware is chosen with LCD_BACKEND below: a 20x4 or 40x4 panel on an
 * I2C backpack, or a 40x4 panel wired straight to the Nano's pins. A 40x4 panel
 * is two HD44780 controllers behind one sheet of glass - see ../docs/WIRING.md
 * for how its second enable line is driven in each case.
 *
 * ALL switch and button inputs are active-low with the internal pull-up on:
 * open = idle, closed to GND = pressed/selected. There are no active-high
 * inputs and no external resistors anywhere. See ../docs/WIRING.md.
 *
 * Based on Mario Gianota's OLED version (July 2020):
 *   https://hackaday.io/project/175129-arduino-morse-code-station
 * Radio link logic based on the cw-send-receive-arduino NRF24 sketch.
 *
 * Credits: original by Mario Gianota; LED, row-display, timing and decode work
 * by VK6TU and VK6XM; wireless integration and the merged firmware by Jazza.
 *
 * Licensed under GPLv3.
 */

#include <SPI.h>
#include <Wire.h>
#include <EEPROM.h>
#include <string.h>

#include "morse.h" // dot/dash classification + decodeMorse*() lookups

// --- Display backend -------------------------------------------------------
// Set LCD_BACKEND to match the display fitted. Geometry, the pin map
// and the live-indicator width all follow from it.
//
//   LCD_I2C_20X4  20x4 module on a stock PCF8574 backpack. No modification.
//   LCD_I2C_40X4  40x4 panel on a PCF8574 backpack with R/W grounded and the
//                 backpack's P1 output re-routed to the panel's E2 pin.
//   LCD_FAST_40X4 40x4 panel wired straight to the Nano (parallel). It uses
//                 nine pins, which leaves none for the radio - DISPLAY only.
#define LCD_I2C_20X4 0
#define LCD_I2C_40X4 1
#define LCD_FAST_40X4 2

#define LCD_BACKEND LCD_I2C_40X4

// The parallel backend claims the pins the radio needs, so a board built that
// way can only ever be a display station.
#if LCD_BACKEND == LCD_FAST_40X4
#define RADIO_AVAILABLE 0
#else
#define RADIO_AVAILABLE 1
#endif

#if RADIO_AVAILABLE
#include "RF24.h"
#endif

#if LCD_BACKEND == LCD_FAST_40X4

#include "LiquidCrystalFast.h"

#define LCD_COLS 40
#define LCD_ROWS 4

// (RS, RW, E1, E2, D4, D5, D6, D7)
LiquidCrystalFast lcd(5, 6, 4, 8, 12, 11, 10, 9);

#elif LCD_BACKEND == LCD_I2C_40X4

#include "LiquidCrystalI2C_Dual.h"

#define LCD_I2C_ADDR 0x27 // run tools/i2c_scanner if unsure; often 0x3F
#define LCD_COLS 40
#define LCD_ROWS 4

// Two controllers: rows 0-1 are strobed by E1, rows 2-3 by E2.
LiquidCrystalI2C_Dual lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS, 2);

#else

#include "LiquidCrystalI2C_Dual.h"

#define LCD_I2C_ADDR 0x27
#define LCD_COLS 20
#define LCD_ROWS 4

// One controller, stock backpack: P1 is never raised, which the panel reads as
// R/W = write. Nothing to modify.
LiquidCrystalI2C_Dual lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS, 1);

#endif

// Row 0 is the header (label + link state + live dot/dash indicator). The rows
// below it carry the keying.
#define HEADER_ROW 0
#define CONTENT_ROWS (LCD_ROWS - 1)

// --- Pin map ---------------------------------------------------------------
// Every input below is INPUT_PULLUP and active-low: open (or unwired) reads
// HIGH and means idle/off, closed to GND reads LOW and means pressed/selected.
// Nothing on this board is active-high and nothing needs an external resistor.
#define BUZZER_PIN 2
#define KEY_PIN 3         // straight key                  LOW = key down
#define CONFIRM_LED_PIN 4 // yellow: local keying
#define STATUS_LED_PIN 5  // red: the remote unit's keying (wireless mode only)
#define TEXT_MODE_PIN 6   // display style   LOW = decoded text, HIGH = raw
#define CLEAR_BUTTON 7    // soft restart                  LOW = pressed

#if LCD_BACKEND == LCD_FAST_40X4
// The parallel panel takes D4-D6 and D8-D12, so the controls that clash move to
// the analogue pins (ordinary digital I/O on a Nano).
#undef CONFIRM_LED_PIN
#undef STATUS_LED_PIN
#undef TEXT_MODE_PIN
#undef CLEAR_BUTTON
#define CONFIRM_LED_PIN 7
#define STATUS_LED_PIN A2 // no radio on this build, but the shared code still
                          // wants somewhere harmless to write
#define TEXT_MODE_PIN A1
#define CLEAR_BUTTON A0
#endif

#if RADIO_AVAILABLE
#define RADIO_CE_PIN 9
#define RADIO_CSN_PIN 8
// Pair select: two jumpers to GND pick which of the four links this board joins.
// Unwired = HIGH; the reads are inverted so unwired counts as 0, making an
// unjumpered board pair 1 and both jumpers fitted pair 4. Read once at boot.
#define PAIR_A_PIN A0 // pair select high bit
#define PAIR_B_PIN A1 // pair select low bit
// Station mode, read once at boot: jumper to GND for a wireless unit, leave it
// open for a standalone display unit.
#define STATION_MODE_PIN A2
// SPI for the radio is fixed on the Nano: D11 MOSI, D12 MISO, D13 SCK. D10 is
// deliberately left unused - as the hardware SS pin it has to stay an output
// for the AVR's SPI peripheral to remain a master.

RF24 radio(RADIO_CE_PIN, RADIO_CSN_PIN);
#endif

#define VER 3
#define SUBVER 0

// Stringize VER/SUBVER so the banner can show "v3.0" without hardcoding it.
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define VERSION_STR "v" STR(VER) "." STR(SUBVER)

// --- Operating mode ---------------------------------------------------------
#if RADIO_AVAILABLE
bool wirelessMode = false; // set from STATION_MODE_PIN at boot
#else
const bool wirelessMode = false; // no radio here, so the branches fold away
#endif
bool textMode = false; // false = raw dots/dashes, true = decoded text

// --- Pair / side identity (wireless mode) -----------------------------------
// A board's identity has two parts. The PAIR (which link, 1-4) is hardwired by
// the two select jumpers and read at boot - it picks the channel and pipe pair.
// The SIDE (A or B, the two halves of that link) is chosen by tapping the key at
// boot and stored in EEPROM. So 1A talks to 1B, 2A to 2B, and so on; the two
// boards in a pair must be opposite sides.
#define SIDE_EEPROM_ADDR 0 // byte offset of the stored side (0 = A, 1 = B)

uint8_t pairIndex = 0; // 0-3, set from the select pins in selectSide()
bool sideB = false;    // false = side A, true = side B; persisted in EEPROM

#if RADIO_AVAILABLE
// Two-way comms addresses, one pair per link: pair 1 uses [0]/[1], 2 uses [2]/[3],
// 3 uses [4]/[5], 4 uses [6]/[7]. Within a pair side A writes the first address
// and reads the second; side B is the mirror image.
const byte ADDRESS[][6] = {"pipe1", "pipe2", "pipe3", "pipe4",
                           "pipe5", "pipe6", "pipe7", "pipe8"};
// Each pair sits on its own channel so the links don't talk over each other.
// 0-83 legal in AU, maps to 2400+N MHz; spaced well apart to avoid adjacency.
const uint8_t PAIR_CHANNELS[] = {76, 40, 8, 24}; // pairs 1, 2, 3, 4
#define NUM_PAIRS (sizeof(PAIR_CHANNELS) / sizeof(PAIR_CHANNELS[0]))
#endif

// Display helpers for the pair/side identity, e.g. pair 1 side B -> "1B".
uint8_t pairLabel() { return pairIndex + 1; } // 1-based pair number for display
char sideChar() { return sideB ? 'B' : 'A'; }

// --- Timing -----------------------------------------------------------------
// Inter-symbol gaps. Idle past LETTER_GAP_MS ends the current letter; idle past
// WORD_GAP_MS ends the word and inserts a space.
#define LETTER_GAP_MS 600
#define WORD_GAP_MS 1600

// Local-key debounce. Kept below a dot's duration so fast keying still
// registers (dotTimeMillisMin is 40ms in morse.cpp).
#define DEBOUNCE_THRESHOLD_MS 15

// The text-mode switch is a slow, human-scale control, so it can afford a much
// longer settling time than the key.
#define MODE_DEBOUNCE_MS 60

// After receiving, hold off transmitting this long to enforce turn-taking and
// stop two units keying over each other (half-duplex lockout).
#define RX_TX_LOCKOUT_MS 3000

// Fail-safe for a dropped key-up packet. If the remote key looks held longer
// than any real dot/dash could last, force it back up so the buzzer/LED don't
// stay stuck on. Must be longer than the longest deliberate element.
#define STUCK_KEY_MS 1500

// How long panic() flashes the LEDs (and shows RADIO FAULT) before it gives up
// and returns so the caller can retry. Recoverable - a transient radio glitch
// or a module that wasn't ready at boot won't brick the unit permanently.
#define PANIC_BLINK_MS 3000

#define BANNER_DISPLAY_TIME 3000 // 3 seconds

// --- Sidetone ---------------------------------------------------------------
// In wireless mode the pitch comes from the side: the local key sounds at this
// side's pitch and the remote key at the other side's, so the two are easy to
// tell apart. A display station has only one key, so it uses a fixed pitch.
#define DISPLAY_TONE_HZ 600
int toneLocalHz = DISPLAY_TONE_HZ;
int toneRemoteHz = DISPLAY_TONE_HZ;

// --- Header layout ----------------------------------------------------------
// Live dot/dash indicator: top-right of the header row, showing the symbols of
// the letter currently being keyed. The 40 column panel has room for a whole
// character; the 20 column one does not.
#if LCD_COLS >= 40
#define DOTDASH_DISPLAY_CELLS 8
#else
#define DOTDASH_DISPLAY_CELLS 6
#endif
const uint8_t dotDashActivityX = LCD_COLS - DOTDASH_DISPLAY_CELLS;

// Column just past the header label, recomputed by showHeader(). The label's
// width depends on the mode ("CW", "CW TEXT", "1A CW", "1A CW TEXT"), so the
// link glyph's position is derived rather than hardcoded.
uint8_t labelEndCol = 0;

// Header link indicator (wireless mode). Shown while the peer has been heard
// within LINK_TIMEOUT_MS - a packet was received, or one of ours was acked - and
// blank once the link falls quiet. So the link shows even when nobody is keying,
// the unit sends a silent keepalive ping every LINK_PING_MS while idle; its ack
// confirms the link.
#define LINK_TIMEOUT_MS 5000
#define LINK_PING_MS 1000
// Two arrows pointing at each other (right-arrow then left-arrow) in the LCD's
// ROM, drawn side by side as the "linked" glyph.
#define LINK_GLYPH_RIGHT 0x7E // 01111110
#define LINK_GLYPH_LEFT 0x7F  // 01111111

// The ASCII full stop sits on the baseline, which makes a run of dots and dashes
// look ragged. The HD44780 ROM's centred dot lines up with the dash instead.
#define GLYPH_DOT 0xA5

// Radio payload is a single byte: 0/1 carry the (active-low) key state, and
// PKT_PING marks an idle keepalive that updates the link but is not a key event.
#define PKT_PING 2

// --- Keying history ---------------------------------------------------------
// Each entry is one decoded letter's morse pattern, or a word-gap marker. The
// screen is re-rendered from this buffer, so the text-mode switch can redraw
// what is already up as either dots/dashes or letters.
//
// Patterns are packed into a single byte rather than kept as strings: a leading
// 1 bit marks where the pattern starts, then one bit per symbol, 0 for a dot and
// 1 for a dash, so ".-" becomes 0b101. The longest pattern we decode is 7
// symbols ('$' = ...-..-), so 8 bits is exactly enough, and a 40x4 panel's
// history costs 120 bytes instead of the 960 the string form would need - which
// matters on a Nano with 2K of RAM and a radio stack to fit alongside.
//
// A packed value of 1 is the empty pattern, used as the word-gap marker.
#define HIST_WORD_GAP 1

// MAX_HISTORY fills the content rows in text mode, where each entry is one cell.
#define MAX_HISTORY (LCD_COLS * CONTENT_ROWS)
uint8_t history[MAX_HISTORY];
int historyCount = 0;

// Pack a ".-" style pattern string into the byte form described above.
uint8_t packPattern(const char *pattern) {
  uint8_t v = 1; // leading sentinel bit
  for (const char *p = pattern; *p; p++) {
    v = (v << 1) | (*p == '-' ? 1 : 0);
  }
  return v;
}

// Number of symbols in a packed pattern: every bit below the sentinel.
uint8_t patternLength(uint8_t v) {
  uint8_t n = 0;
  while (v > 1) {
    v >>= 1;
    n++;
  }
  return n;
}

// Expand a packed pattern back into a ".-" string. `out` needs MAX_PATTERN + 1
// bytes.
void unpackPattern(uint8_t v, char *out) {
  uint8_t n = patternLength(v);
  out[n] = '\0';
  for (uint8_t i = n; i > 0; i--) {
    out[i - 1] = (v & 1) ? '-' : '.';
    v >>= 1;
  }
}

// Decode a packed pattern straight to its character.
char decodePacked(uint8_t v) {
  char pattern[MAX_PATTERN + 1];
  unpackPattern(v, pattern);
  return decodeMorsePattern(pattern);
}

// The letter being assembled, one symbol per key-up. Kept as a string because
// that is what the live indicator draws and what decodeMorsePattern() takes.
char currentPattern[MAX_PATTERN + 1];
int patternLen = 0;

// Decode state machine:
//   letterPending - symbols are buffered but not yet committed to a letter
//   wordPending   - a letter has been keyed, so a long gap should add a space
bool letterPending = false;
bool wordPending = false;
unsigned long keyDownStart = 0;    // millis() of the current key-down
unsigned long lastKeyActivity = 0; // millis() of the last key edge, for gaps

// Remote key edge tracking (wireless mode).
bool rxKeyDown = false;

// Link tracking for the header indicator.
unsigned long lastLinkActivity = 0; // millis() of the last received/acked packet
unsigned long lastPing = 0;         // millis() of the last idle keepalive ping
bool linkShown = false;             // current state of the header link glyph

// Local key state.
unsigned long timeOfLastReceive = 0;
unsigned long lastDebounce = 0;
bool rawKeyState = HIGH;       // active-low key: HIGH = released
bool debouncedKeyState = HIGH; // last settled key state

// Text-mode switch debounce.
bool rawTextSwitch = HIGH;
unsigned long lastTextSwitchChange = 0;

// --- Radio ------------------------------------------------------------------
#if RADIO_AVAILABLE

// Flash both LEDs (and show RADIO FAULT) for PANIC_BLINK_MS, then return so the
// caller can retry. Not a permanent lockup - a radio that comes good recovers.
void panic() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("RADIO FAULT"));

  unsigned long start = millis();
  while (millis() - start < PANIC_BLINK_MS) {
    digitalWrite(CONFIRM_LED_PIN, HIGH);
    digitalWrite(STATUS_LED_PIN, HIGH);
    delay(100);
    digitalWrite(CONFIRM_LED_PIN, LOW);
    digitalWrite(STATUS_LED_PIN, LOW);
    delay(100);
  }
}

void setupRadio() {
  // Keep retrying instead of bricking: panic() flashes for its timeout, then we
  // loop and try again, so the unit recovers once the module is reachable.
  while (!radio.begin()) {
    Serial.println(F("Radio init FAILED"));
    panic();
  }
  Serial.println(F("Radio init ok"));

  // Pick this board's pipes from its pair: pair 1 -> ADDRESS[0]/[1], 2 -> [2]/[3],
  // 3 -> [4]/[5], 4 -> [6]/[7]. Side A writes the first address and reads the
  // second; side B is the mirror image.
  uint8_t pairBase = pairIndex * 2;
  if (!sideB) {
    radio.openWritingPipe(ADDRESS[pairBase]);
    radio.openReadingPipe(1, ADDRESS[pairBase + 1]);
  } else {
    radio.openWritingPipe(ADDRESS[pairBase + 1]);
    radio.openReadingPipe(1, ADDRESS[pairBase]);
  }

  radio.setPALevel(RF24_PA_LOW);
  radio.setDataRate(RF24_250KBPS);
  radio.setChannel(PAIR_CHANNELS[pairIndex]); // each pair on its own channel
  radio.setAutoAck(true);
  radio.setRetries(5, 15); // 1500us between retries, up to 15 attempts
  radio.startListening();
}

#endif // RADIO_AVAILABLE

// --- Display ----------------------------------------------------------------

// Header link indicator: the two-arrow glyph right after the label while the
// peer has been heard recently, blank once the link falls quiet. Touches the LCD
// only on a state change, so it is cheap to poll every loop.
void showLinkIndicator() {
  bool linked = lastLinkActivity != 0 &&
                (millis() - lastLinkActivity) < LINK_TIMEOUT_MS;
  if (linked == linkShown) {
    return;
  }
  linkShown = linked;
  lcd.setCursor(labelEndCol + 1, HEADER_ROW); // one space after the label
  lcd.write(linked ? (uint8_t)LINK_GLYPH_RIGHT : ' ');
  lcd.write(linked ? (uint8_t)LINK_GLYPH_LEFT : ' ');
}

// Header row: identity and mode on the left, live dot/dash indicator on the
// right. A wireless unit prefixes its pair/side ("1A CW"); a display unit has no
// pair to name, so it is just "CW".
void showHeader() {
  lcd.setCursor(0, HEADER_ROW);
  uint8_t col = 0;

  if (wirelessMode) {
    lcd.print(pairLabel());
    lcd.write(sideChar());
    lcd.write(' ');
    col += 3;
  }
  lcd.print(F("CW"));
  col += 2;
  if (textMode) {
    lcd.print(F(" TEXT"));
    col += 5;
  }
  labelEndCol = col;

  // Blank the gap between the label and the live dot/dash area, then force the
  // link glyph to redraw - the header may have just been cleared.
  while (col < dotDashActivityX) {
    lcd.write(' ');
    col++;
  }
  linkShown = false;
  if (wirelessMode) {
    showLinkIndicator();
  }
}

// Show the symbols of the letter currently being keyed, top-right.
void showActivity() {
  lcd.setCursor(dotDashActivityX, HEADER_ROW);
  for (uint8_t i = 0; i < DOTDASH_DISPLAY_CELLS; i++) {
    char sym = i < patternLen ? currentPattern[i] : ' ';
    lcd.write((uint8_t)(sym == '.' ? GLYPH_DOT : sym));
  }
}

void clearActivity() {
  patternLen = 0;
  currentPattern[0] = '\0';
  showActivity();
}

// Width in display cells of history entry i in the current mode.
uint8_t entryWidth(int i) {
  if (textMode) {
    return 1; // a single letter, or a single space for a word gap
  }
  if (history[i] <= HIST_WORD_GAP) {
    return 2; // "/ "
  }
  return patternLength(history[i]) + 1; // pattern plus its separating space
}

// Re-render the content rows from the history buffer for the current mode,
// showing the most recent entries that fit.
void renderHistory() {
  const int cells = LCD_COLS * CONTENT_ROWS;

  // Walk back from the newest entry until the tail no longer fits.
  int start = historyCount;
  int total = 0;
  for (int i = historyCount - 1; i >= 0; i--) {
    int w = entryWidth(i);
    if (total + w > cells) {
      break;
    }
    total += w;
    start = i;
  }

  // Lay the visible entries out left-to-right, top-to-bottom into a flat buffer.
  char buf[LCD_COLS * CONTENT_ROWS];
  memset(buf, ' ', sizeof(buf));
  int pos = 0;
  for (int i = start; i < historyCount && pos < cells; i++) {
    uint8_t v = history[i];
    bool gap = (v <= HIST_WORD_GAP);

    if (textMode) {
      buf[pos++] = gap ? ' ' : decodePacked(v);
    } else if (gap) {
      buf[pos++] = '/';
      if (pos < cells) {
        buf[pos++] = ' ';
      }
    } else {
      char pattern[MAX_PATTERN + 1];
      unpackPattern(v, pattern);
      for (const char *p = pattern; *p && pos < cells; p++) {
        buf[pos++] = (*p == '.') ? (char)GLYPH_DOT : *p;
      }
      if (pos < cells) {
        buf[pos++] = ' ';
      }
    }
  }

  // Byte-for-byte write so each stored byte maps to exactly one display cell,
  // bypassing Print's UTF-8 handling.
  for (int r = 0; r < CONTENT_ROWS; r++) {
    lcd.setCursor(0, HEADER_ROW + 1 + r);
    for (int c = 0; c < LCD_COLS; c++) {
      lcd.write((uint8_t)buf[r * LCD_COLS + c]);
    }
  }
}

// Append a history entry, dropping the oldest if the buffer is full.
void pushHistory(uint8_t packed) {
  if (historyCount >= MAX_HISTORY) {
    memmove(&history[0], &history[1], MAX_HISTORY - 1);
    historyCount = MAX_HISTORY - 1;
  }
  history[historyCount++] = packed;
}

// Boot banner. Names the station mode, so the mode switch setting is readable
// without a serial monitor, and the pair/side when there is one.
void drawBanner(bool showTapHint) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F(" -- SCOUTS WA --"));
  lcd.setCursor(0, 1);
  lcd.print(wirelessMode ? F(" Wireless CW ") : F(" Display CW "));
  lcd.print(F(VERSION_STR));
  lcd.setCursor(0, 2);
  if (wirelessMode) {
    lcd.print(F(" Unit "));
    lcd.print(pairLabel());
    lcd.write(sideChar());
  } else {
    lcd.print(F(" Local key"));
  }
  lcd.setCursor(0, 3);
  lcd.print(showTapHint ? F("Tap key: side A/B") : F("By VK6TU/VK6XM"));
}

void welcomeBanner(int waitDelay) {
  drawBanner(false);
  delay(waitDelay);
  lcd.clear();
}

#if RADIO_AVAILABLE
// Boot-time side chooser. Reads the hardwired pair from the select pins, loads
// the saved side from EEPROM, then for the banner window each key tap flips the
// side (A <-> B). The result is saved back to EEPROM so it sticks across power
// cycles, and confirmed with a beep (one for A, two for B) so it is readable
// even with no display. Runs before the radio listens, so taps don't transmit.
// Shares BANNER_DISPLAY_TIME - no extra boot delay.
void selectSide() {
  // Jumpers to GND with INPUT_PULLUP, so unwired = HIGH. Invert each read so
  // unwired counts as 0: nothing connected = pair 1, and each jumper to GND adds
  // to the number (both connected = 11 = pair 4).
  pairIndex = (!digitalRead(PAIR_A_PIN) << 1) | !digitalRead(PAIR_B_PIN);
  if (pairIndex >= NUM_PAIRS) {
    pairIndex = 0; // guard if PAIR_CHANNELS has fewer than four entries
  }

  uint8_t stored = EEPROM.read(SIDE_EEPROM_ADDR);
  sideB = (stored == 1); // anything else (old unit number, 0xFF) defaults to A
  bool startSide = sideB;

  drawBanner(true);

  // Wait for the banner window to elapse with no tap. Each tap flips the side and
  // restarts the window, so it confirms 3s after the last press.
  bool prevKey = HIGH; // active-low key, idle HIGH
  unsigned long lastActivity = millis();
  while (millis() - lastActivity < BANNER_DISPLAY_TIME) {
    bool k = digitalRead(KEY_PIN);
    if (prevKey == HIGH && k == LOW) { // falling edge = a tap
      sideB = !sideB;                  // flip A <-> B
      tone(BUZZER_PIN, 880, 60);       // short tap blip
      drawBanner(true);
      lastActivity = millis(); // restart the window from this tap
      delay(50);               // debounce the tap
    }
    prevKey = k;
  }

  if (sideB != startSide) {
    EEPROM.update(SIDE_EEPROM_ADDR, sideB ? 1 : 0); // only writes on change
  }

  // Confirm the landing side: one beep/blink for A, two for B.
  uint8_t beeps = sideB ? 2 : 1;
  for (uint8_t i = 0; i < beeps; i++) {
    digitalWrite(CONFIRM_LED_PIN, HIGH);
    tone(BUZZER_PIN, 660, 120);
    delay(180);
    digitalWrite(CONFIRM_LED_PIN, LOW);
    delay(120);
  }
}
#endif // RADIO_AVAILABLE

// Reset operating state and draw the live screen, without the banner.
void initStation() {
  historyCount = 0;
  patternLen = 0;
  currentPattern[0] = '\0';
  letterPending = false;
  wordPending = false;
  rxKeyDown = false;
  lastKeyActivity = millis();

  digitalWrite(CONFIRM_LED_PIN, LOW);
  digitalWrite(STATUS_LED_PIN, LOW);
  noTone(BUZZER_PIN);

  lcd.clear();
  showHeader();
  showActivity();
  renderHistory();
}

// Soft restart (clear button): show the banner, then reset state.
void resetStation() {
  welcomeBanner(BANNER_DISPLAY_TIME);
  initStation();
}

void setup() {
  Serial.begin(9600); // serial debug output

  // Every control is an active-low switch to GND on the internal pull-up.
  pinMode(KEY_PIN, INPUT_PULLUP);
  pinMode(TEXT_MODE_PIN, INPUT_PULLUP);
  pinMode(CLEAR_BUTTON, INPUT_PULLUP);
#if RADIO_AVAILABLE
  pinMode(PAIR_A_PIN, INPUT_PULLUP);
  pinMode(PAIR_B_PIN, INPUT_PULLUP);
  pinMode(STATION_MODE_PIN, INPUT_PULLUP);
#endif
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(CONFIRM_LED_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);

#if LCD_BACKEND == LCD_FAST_40X4
  lcd.begin(LCD_COLS, LCD_ROWS); // brings up both controllers
#else
  // Bring up I2C with a timeout so a missing, half-plugged or flaky display
  // can't wedge the bus and freeze the radio. The unit runs fine headless -
  // writes to an absent LCD are just NACKed and ignored.
  Wire.begin();
  Wire.setWireTimeout(25000, true); // 25ms, reset the bus on timeout
  lcd.init();
  lcd.backlight();
#endif

  // Read both mode switches before anything is drawn, so the banner already
  // shows the mode this unit came up in.
#if RADIO_AVAILABLE
  wirelessMode = (digitalRead(STATION_MODE_PIN) == LOW);
#endif
  rawTextSwitch = digitalRead(TEXT_MODE_PIN);
  textMode = (rawTextSwitch == LOW);
  lastTextSwitchChange = millis();

  Serial.println(wirelessMode ? F("Mode: WIRELESS") : F("Mode: DISPLAY"));

#if RADIO_AVAILABLE
  if (wirelessMode) {
    // Read the hardwired pair and choose this board's side (tap the key during
    // the banner), then derive the sidetone pitches from the side before the
    // radio pipes are set up. Side A keys low and hears high; side B is
    // reversed, so the two sides are easy to tell apart.
    selectSide();
    toneLocalHz = sideB ? 900 : 600;
    toneRemoteHz = sideB ? 600 : 900;
    setupRadio();
  } else
#endif
  {
    // A display station has no side to choose, so it just shows the banner.
    welcomeBanner(BANNER_DISPLAY_TIME);
    toneLocalHz = DISPLAY_TONE_HZ;
    toneRemoteHz = DISPLAY_TONE_HZ;
  }

  initStation();
}

/* ***********************************************
MAIN LOOP HERE
************************************************ */
void loop() {
  scanControls();    // clear button and the live text-mode switch
  serviceLocalKey(); // transmit it (wireless) or decode it (display)

#if RADIO_AVAILABLE
  if (wirelessMode) {
    pingLink();              // idle keepalive so the link shows without keying
    receiveRemoteKey();      // receive the remote key and feed the decoder
    releaseStuckRemoteKey(); // recover if a key-up packet was lost
    showLinkIndicator();     // header link glyph tracks peer reachability
  }
#endif

  updateDecodeTiming(); // gaps end the current letter/word
}
/* ***********************************************
END: MAIN LOOP
************************************************ */

// Clear button and text-mode switch. The station-mode switch is deliberately
// boot-only: swapping a unit between wireless and standalone mid-session would
// mean tearing down and re-initialising the radio underneath a live QSO.
void scanControls() {
  if (digitalRead(CLEAR_BUTTON) == LOW) {
    resetStation();
    return;
  }

  // Text mode is live: flipping the switch re-renders the buffer that is
  // already on screen, so what has been keyed can be read either way.
  bool reading = digitalRead(TEXT_MODE_PIN);
  if (reading != rawTextSwitch) {
    rawTextSwitch = reading;
    lastTextSwitchChange = millis();
    return;
  }
  if (millis() - lastTextSwitchChange < MODE_DEBOUNCE_MS) {
    return;
  }

  bool wanted = (reading == LOW);
  if (wanted != textMode) {
    textMode = wanted;
    showHeader(); // the label's width changes with the mode
    renderHistory();
  }
}

// --- Decoder ----------------------------------------------------------------
// Both key sources - the local key in display mode, the radio in wireless mode -
// funnel through these two, so the decode and render path is identical either
// way. The caller owns the LED and sidetone; these only touch decode state.

void decoderKeyDown() {
  keyDownStart = millis();
  lastKeyActivity = keyDownStart;
}

void decoderKeyUp() {
  unsigned long held = millis() - keyDownStart;
  lastKeyActivity = millis();

  // Classify the held duration; anything too short to be a dot is dropped as
  // bounce. A real symbol extends the current letter and updates the indicator.
  char sym = symbolFor(held);
  if (sym != '\0' && patternLen < MAX_PATTERN) {
    currentPattern[patternLen++] = sym;
    currentPattern[patternLen] = '\0';
    letterPending = true;
    wordPending = true;
    showActivity();
  }
}

// A gap past WORD_GAP_MS ends a word (insert a space). A shorter gap past
// LETTER_GAP_MS ends the current letter (commit its pattern).
void updateDecodeTiming() {
  unsigned long idle = millis() - lastKeyActivity;

  if (idle > WORD_GAP_MS && wordPending) {
    pushHistory(HIST_WORD_GAP);
    wordPending = false;
    renderHistory();
  } else if (idle > LETTER_GAP_MS && letterPending) {
    pushHistory(packPattern(currentPattern));
    letterPending = false;
    clearActivity();
    renderHistory();
  }
}

// --- Local key --------------------------------------------------------------

// Debounce the local key and act on each settled edge. In wireless mode the new
// state goes out over the radio and only the sidetone/LED stay local; in display
// mode it drives the decoder directly. Mirrors the cw-send-receive radio sketch
// for the transmit half: debounce, half-duplex lockout, TX confirmation.
void serviceLocalKey() {
  bool reading = digitalRead(KEY_PIN);
  unsigned long now = millis();

  if (reading != rawKeyState) {
    lastDebounce = now;
    rawKeyState = reading;
  }

#if RADIO_AVAILABLE
  // Don't transmit just after receiving, so the two units take turns.
  if (wirelessMode && now - timeOfLastReceive < RX_TX_LOCKOUT_MS) {
    return;
  }
#endif

  if ((now - lastDebounce) > DEBOUNCE_THRESHOLD_MS &&
      reading != debouncedKeyState) {
    debouncedKeyState = reading;
    bool down = (debouncedKeyState == LOW); // active-low key: LOW = pressed

#if RADIO_AVAILABLE
    if (wirelessMode) {
      radio.stopListening(); // leave RX only for the actual transmit
      bool txOk = radio.write(&debouncedKeyState, sizeof(debouncedKeyState));
      radio.startListening();
      // A failed write just means the peer didn't ack this packet (out of range,
      // powered off, or RF noise). That's normal operation, not a fault - log it
      // and carry on; the local sidetone/LED below still track the key.
      Serial.println(txOk ? F("TX ok") : F("TX FAILED"));

      // An ack means the peer is alive and in range - keep the link glyph lit.
      if (txOk) {
        lastLinkActivity = millis();
      }
    } else
#endif
    {
      // Display station: the local key is the only source, so feed the decoder.
      if (down) {
        decoderKeyDown();
      } else {
        decoderKeyUp();
      }
    }

    // Sidetone and the yellow LED follow the local key in both modes.
    if (down) {
      digitalWrite(CONFIRM_LED_PIN, HIGH);
      tone(BUZZER_PIN, toneLocalHz);
    } else {
      digitalWrite(CONFIRM_LED_PIN, LOW);
      noTone(BUZZER_PIN);
    }
  }

  delay(5);
}

// --- Radio traffic ----------------------------------------------------------
#if RADIO_AVAILABLE

// Idle keepalive: when nothing is being keyed, ping the peer every LINK_PING_MS
// so the header link glyph reflects the actual radio link, not just whether we
// happen to be transmitting. The ping carries PKT_PING so the peer counts it as
// link activity without treating it as a key event. Its ack lights our own link.
void pingLink() {
  unsigned long now = millis();
  if (now - lastPing < LINK_PING_MS) {
    return;
  }

  // Stay off the air while either key is down or during the post-RX lockout, so
  // keepalives never talk over real keying.
  if (debouncedKeyState == LOW || rxKeyDown ||
      now - timeOfLastReceive < RX_TX_LOCKOUT_MS) {
    return;
  }

  lastPing = now;
  uint8_t ping = PKT_PING;
  radio.stopListening();
  bool ok = radio.write(&ping, sizeof(ping));
  radio.startListening();
  if (ok) {
    lastLinkActivity = now;
  }
}

// Receive the remote key state and drive the LED, sidetone and decoder.
void receiveRemoteKey() {
  if (!radio.available()) {
    return;
  }

  uint8_t rxByte;
  radio.read(&rxByte, sizeof(rxByte));

  // Any packet - key edge or keepalive - means the link is up.
  lastLinkActivity = millis();

  // A keepalive ping carries no key event and must not start the turn-taking
  // lockout, or a steadily-pinging idle peer would jam our own keying.
  if (rxByte == PKT_PING) {
    return;
  }

  timeOfLastReceive = millis();
  Serial.print(F("Received: "));
  Serial.println(rxByte);

  bool down = (rxByte == LOW); // active-low key: LOW = remote key down
  if (down && !rxKeyDown) {
    digitalWrite(STATUS_LED_PIN, HIGH);
    tone(BUZZER_PIN, toneRemoteHz);
    decoderKeyDown();
  } else if (!down && rxKeyDown) {
    digitalWrite(STATUS_LED_PIN, LOW);
    noTone(BUZZER_PIN);
    decoderKeyUp();
  }
  rxKeyDown = down;
}

// If a key-up packet was lost the remote key stays "down" forever, jamming the
// buzzer/LED on. Once it has been held longer than any real element, drop it.
// The runaway duration isn't a valid dot/dash, so we discard it rather than
// record a bogus symbol.
void releaseStuckRemoteKey() {
  if (rxKeyDown && (millis() - keyDownStart > STUCK_KEY_MS)) {
    rxKeyDown = false;
    lastKeyActivity = millis();
    digitalWrite(STATUS_LED_PIN, LOW);
    noTone(BUZZER_PIN);
  }
}

#endif // RADIO_AVAILABLE
