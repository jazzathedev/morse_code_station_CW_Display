/**
 * A Morse code station for the Arduino, driving a 20x4 I2C or 40x4 parallel
 * character LCD (pick one with LCD_BACKEND below).
 *
 * A straight key wired to CODE_BUTTON is timed: short presses are dots, long
 * presses are dashes. Gaps between presses separate letters and words. Decoded
 * text scrolls up the LCD, and a live dot/dash indicator shows the current
 * symbol being keyed in the top-right corner.
 *
 * Based on Mario Gianota's OLED version (July 2020):
 *   https://hackaday.io/project/175129-arduino-morse-code-station
 *   https://github.com/grahowe/Morse-Code-Station
 *
 * Credits: original by Mario Gianota; LED, row-display, pause/clear and timing
 * work by VK6TU and VK6XM; current changes by Jazza.
 */

#include <SPI.h>
#include <Wire.h>

#include "morse.h" // dot/dash classification + decodeMorse() lookup

// --- Display backend -------------------------------------------------------
// Set LCD_BACKEND to match the display in front of you. Everything below -
// geometry, row buffers, cursor positions and the pin map - follows from it.
//
//   LCD_I2C_20X4  20x4 module on an I2C backpack (SDA A4, SCL A5).
//   LCD_FAST_40X4 40x4 parallel module on the Green Morse code station board.
//
// A 40x4 panel is really two HD44780 controllers behind one glass: rows 0-1
// belong to the first, rows 2-3 to the second, and they share every line except
// enable, which is split into E1 and E2. LiquidCrystalFast takes both enables
// and picks the right chip inside setCursor(), so the rest of the sketch treats
// it as one 40x4 display. It is not in the Arduino library index, so a copy
// ships next to this sketch (LiquidCrystalFast.cpp/.h, LGPL 2.1) and no separate
// install is needed.
#define LCD_I2C_20X4 0
#define LCD_FAST_40X4 1

#define LCD_BACKEND LCD_FAST_40X4

#if LCD_BACKEND == LCD_FAST_40X4

#include "LiquidCrystalFast.h"

#define LCD_COLS 40
#define LCD_ROWS 4

// (RS, RW, E1, E2, D4, D5, D6, D7)
// RW is 255 (unused) on purpose: the busy-flag read-back this library does
// when a real RW pin is given is timing-sensitive, and on this board it was
// corrupting every command/character write after the initial 4-bit handshake
// (LCD came up but only ever showed solid block characters). Tie the LCD's
// RW pin to GND instead of wiring it to the Arduino - that forces the
// library onto its fixed-delay write path, which is what actually works.
LiquidCrystalFast lcd(5, 255, 4, 8, 12, 11, 10, 9);

// The LCD claims D4, D5, D7-D12 (D6 is free - RW goes to GND, not the Nano),
// so the LED and clear button move off the pins the 20x4 build uses. Buzzer
// and key are unchanged.
#define BUZZER_PIN 2
#define CODE_BUTTON 3
#define LED_PIN 7
#define CLEAR_BUTTON A0

#else

#include <LCDI2C_Generic.h>

#define LCD_I2C_ADDR 0x27
#define LCD_COLS 20
#define LCD_ROWS 4

LCDI2C_Generic lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);

#define BUZZER_PIN 2
#define CODE_BUTTON 3
#define LED_PIN 4
#define CLEAR_BUTTON 5

#endif

#define BOTTOM_ROW (LCD_ROWS - 1) // decoded text is printed on the last row

// Row 0 is the header; the rows below it scroll the decoded text.
#define TEXT_ROWS (LCD_ROWS - 1)

#define VER 1
#define SUBVER 7

// Stringize VER/SUBVER so the banner can show "v1.6" without hardcoding it.
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define VERSION_STR "v" STR(VER) "." STR(SUBVER)

// Inter-symbol gap timing. Once the key has been idle this long we end the
// current letter (LETTER_GAP_MS); idle longer still ends the word and emits a
// space (WORD_GAP_MS).
#define LETTER_GAP_MS 600
#define WORD_GAP_MS 1600

// Presses shorter than this are treated as switch bounce, not a real dot/dash.
#define BOUNCE_MS 25

// Sidetone pitch (Hz) and duration (ms) sounded for each keyed symbol.
#define TONE_HZ 440
#define TONE_MS 60

// Max symbols (dots/dashes) captured per character. Must cover the longest code
// we decode: 7 for '$' (...-..-). Most letters/digits are <= 5, punctuation 6-7.
#define MAX_BUTTON_PRESS_TIMES 8

// How many of those symbols the live dot/dash indicator shows. Bounded by the
// columns available to its right (dotDashActivityX .. last column), so the 40
// column display has room for a whole letter while the 20 column one does not.
#if LCD_BACKEND == LCD_FAST_40X4
#define DOTDASH_DISPLAY_CELLS MAX_BUTTON_PRESS_TIMES
#else
#define DOTDASH_DISPLAY_CELLS 6
#endif

#define BANNER_DISPLAY_TIME 3000 // 3 seconds

// Row-scrolling buffers: one per text row, holding what that row is showing so
// the lot can be shifted up a line as characters fill the bottom row. rows[r]
// is displayed on LCD row r + 1 (row 0 being the header).
char rows[TEXT_ROWS][LCD_COLS + 1];

int displayPos = 0; // current column on the bottom row

// Top-right screen position for the live dot/dash activity indicator.
const unsigned int dotDashActivityX = LCD_COLS - DOTDASH_DISPLAY_CELLS;
const unsigned int dotDashActivityY = 0;

// Key state machine:
//   codeButtonArmed   - a key-down has started and the timer is running
//   codeButtonPressed - the key is currently held down
//   letterDecoded     - the symbols buffered so far have been turned into a char
//   newWord           - a symbol is pending, so a long gap should insert a space
bool codeButtonArmed;
bool codeButtonPressed;
unsigned long codeTime;            // duration of the current/last key-down (ms)
unsigned long startTime;           // millis() when the current key-down began
unsigned long lastButtonPressTime; // millis() of last key activity, for gap timing
bool letterDecoded;
bool newWord;
bool initialChar = false; // suppresses the very first decoded char after reset

// Durations of each key-down in the letter currently being built. bptIndex is
// the next free slot; decodeMorse() reads buttonPressTimes[0..bptIndex).
unsigned long buttonPressTimes[MAX_BUTTON_PRESS_TIMES];
int bptIndex;

// dot/dash timing thresholds (dotTimeMillisMin/Max) live in morse.cpp

void showDotDashActivity() {

  lcd.setCursor((dotDashActivityX), dotDashActivityY);
  for (int i = 0; i < DOTDASH_DISPLAY_CELLS; i++) {
    if (isDot(buttonPressTimes[i])) {
      // The ASCII full stop sits on the baseline; swap in the ROM's centered
      // dot (0xA5) so dots line up vertically with dashes.
      lcd.write((uint8_t)0xA5);
    } else if (isDash((buttonPressTimes[i]))) {
      lcd.print("-");
    } else {
      // empty slot (not keyed yet) - draw a blank
      lcd.print(" ");
    }
  }
}

void welcomeBanner(int waitDelay) {

  char banner1[] = " -- SCOUTS WA --";
  char banner2[] = " Radio & Tech Team";
  char banner3[] = VERSION_STR " cw";
  char banner4[] = "By VK6TU/VK6XM";
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(banner1);
  lcd.setCursor(0, 1);
  lcd.print(banner2);
  lcd.setCursor(0, 2);
  lcd.print(banner3);
  lcd.setCursor(0, 3);
  lcd.print(banner4);
  delay(waitDelay);
  lcd.clear();
}

// Fill a row buffer with spaces, terminated so writeRow() covers every column.
void blankRow(char *row) {
  memset(row, ' ', LCD_COLS);
  row[LCD_COLS] = '\0';
}

void resetSystem() {
  welcomeBanner(BANNER_DISPLAY_TIME);

  lcd.setCursor(0, 0);
  lcd.print("Morse Code:");
  lcd.setCursor(0, 1);

  for (int r = 0; r < TEXT_ROWS; r++)
    blankRow(rows[r]);
  newWord = false;
  letterDecoded = true;
  displayPos = 0; // reset column where printing will start
  initialChar = false;
  codeButtonPressed = false;
  codeButtonArmed = false;
  resetButtonPressTimes();
  digitalWrite(LED_PIN, LOW);
  codeTime = 0;
}

void setup() {

  Serial.begin(9600); // serial debug output

  pinMode(CODE_BUTTON, INPUT);
  pinMode(CLEAR_BUTTON, INPUT_PULLUP); // active-low button, no external resistor
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

#if LCD_BACKEND == LCD_FAST_40X4
  lcd.begin(LCD_COLS, LCD_ROWS); // brings up both controllers
#else
  lcd.init();
  lcd.backlight();
#endif

  resetSystem();
}

// Write a row buffer to the LCD one byte per column, bypassing the library's
// UTF-8 decoding so each stored byte maps to exactly one display cell.
void writeRow(const char *s) {
  while (*s)
    lcd.write((uint8_t)*s++);
}

// Shift every text row up one, blank the freed bottom row, and redraw them all.
void scrollRows() {
  for (int r = 0; r < TEXT_ROWS - 1; r++)
    strcpy(rows[r], rows[r + 1]);
  blankRow(rows[TEXT_ROWS - 1]);

  for (int r = 0; r < TEXT_ROWS; r++) {
    lcd.setCursor(0, r + 1);
    writeRow(rows[r]);
  }
}

// Print one character to the bottom row. When the row fills, scroll the text
// rows up and carry on at the start of the (now blank) bottom row.
void displayChar(char ch) {

  if (initialChar) { // swallow the spurious first char after a reset
    initialChar = false;
    return;
  }

  lcd.setCursor(displayPos, BOTTOM_ROW);
  lcd.write((uint8_t)ch); // byte-for-byte write, no UTF-8 decoding (one cell)

  rows[TEXT_ROWS - 1][displayPos] = ch;
  displayPos++;

  if (displayPos >= LCD_COLS) { // bottom row full: scroll everything up
    Serial.println("Row full, scrolling display.");

    displayPos = 0;
    scrollRows();
    lcd.setCursor(0, BOTTOM_ROW); // cursor back to start of bottom row
  }
}

void resetButtonPressTimes() {
  for (int i = 0; i < MAX_BUTTON_PRESS_TIMES; i++) {
    buttonPressTimes[i] = 0;
  }
}

/* ***********************************************
MAIN LOOP HERE
************************************************ */
void loop() {

  scanButtons();

  // A gap past WORD_GAP_MS ends a word: insert a space. A shorter gap (past
  // LETTER_GAP_MS) just ends the current letter: decode what we have so far.
  if (millis() - lastButtonPressTime > WORD_GAP_MS && newWord) {
    Serial.println("New word");
    displayChar(' ');
    newWord = false;
  } else if (millis() - lastButtonPressTime > LETTER_GAP_MS && !letterDecoded) {
    decodeButtonPresses();
    letterDecoded = true;
    codeButtonArmed = false;
  }
}
/* ***********************************************
END: MAIN LOOP
************************************************ */

void codeButtonReleased() {
  digitalWrite(LED_PIN, LOW);

  // Ignore presses shorter than BOUNCE_MS - those are almost always switch
  // bounce rather than a real dot/dash.
  if (codeTime > BOUNCE_MS) {
    buttonPressTimes[bptIndex] = codeTime;
    bptIndex++;

    showDotDashActivity(); // update the live dot/dash indicator

    // Buffer full: wrap back to the start so the next press doesn't write OOB.
    if (bptIndex >= MAX_BUTTON_PRESS_TIMES) {
      resetButtonPressTimes();
      bptIndex = 0;
    }
  }
}

void scanButtons() {

  // Clear button: soft-restart (clears screen and state) without power cycling.
  if (digitalRead(CLEAR_BUTTON) == LOW) {
    resetSystem();
  }

  if (!codeButtonArmed && digitalRead(CODE_BUTTON) == HIGH) {
    // First edge of a new key-down: start the timer.
    codeButtonArmed = true;
    startTime = millis();
    lastButtonPressTime = startTime;
    codeTime = 0;
    letterDecoded = false;
    newWord = true;
  } else if (digitalRead(CODE_BUTTON) == HIGH) {
    // Still held down: keep the tone/LED on and accumulate the press duration.
    codeButtonPressed = true;
    tone(BUZZER_PIN, TONE_HZ, TONE_MS);
    digitalWrite(LED_PIN, HIGH);
    codeTime = millis() - startTime;
  }
  if (codeButtonPressed && digitalRead(CODE_BUTTON) == LOW) {
    codeButtonPressed = false;
    codeButtonReleased();
    codeButtonArmed = false;
  }
  delay(10); // crude debounce / poll interval; also the timing resolution of a dot
}

void decodeButtonPresses() {

  // Dump the collected dots/dashes to serial for debugging.
  Serial.print("DECODE LETTER: ");
  for (int i = 0; i < bptIndex; i++) {
    if (isDot(buttonPressTimes[i]))
      Serial.print(" DOT ");
    else if (isDash(buttonPressTimes[i]))
      Serial.print(" DASH");
  }
  Serial.print("   ");

  char c = decodeMorse(buttonPressTimes, bptIndex);
  displayChar(c);
  Serial.print(c);
  Serial.println();

  // Done with this letter: clear the buffer ready for the next one.
  bptIndex = 0;
  resetButtonPressTimes();
}
