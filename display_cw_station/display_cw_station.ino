/**
 * A Morse code station for the Arduino, driving a 20x4 (or 16x2) character LCD.
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

// LCD geometry and I2C address. Must match the physical display; the row
// buffers and cursor positions below all derive from these.
#define LCD_I2C_ADDR 0x27
#define LCD_COLS 20
#define LCD_ROWS 4
#define BOTTOM_ROW (LCD_ROWS - 1) // decoded text is printed on the last row

#include <LCDI2C_Generic.h>
LCDI2C_Generic lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);

// Wiring for a parallel (non-I2C) LCD instead of the I2C one above:
// #include <LiquidCrystal.h>
// LiquidCrystal lcd(2, 255, 3, 4, 5, 6, 7); // (RS, RW, E, D4, D5, D6, D7)

#define VER 1
#define SUBVER 6

// Stringize VER/SUBVER so the banner can show "v1.6" without hardcoding it.
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define VERSION_STR "v" STR(VER) "." STR(SUBVER)

#define BUZZER_PIN 2
#define LED_PIN 4
#define CODE_BUTTON 3
#define CLEAR_BUTTON 5

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
// columns available to its right (dotDashActivityX .. last column).
#define DOTDASH_DISPLAY_CELLS 6

#define BANNER_DISPLAY_TIME 3000 // 3 seconds

// Row-scrolling buffers. Each holds one display row of text so it can be shifted
// up a line at a time as new characters fill the bottom row. BLANKROW must be
// LCD_COLS spaces wide.
#define BLANKROW "                    "
char blankrow[] = BLANKROW;
char row1[] = BLANKROW;
char row2[] = BLANKROW;
char row3[] = BLANKROW;
static_assert(sizeof(BLANKROW) - 1 == LCD_COLS, "BLANKROW must be LCD_COLS spaces");

int displayPos = 0; // current column on the bottom row

// Top-right screen position for the live dot/dash activity indicator.
const unsigned int dotDashActivityX = 14;
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

void resetSystem() {
  welcomeBanner(BANNER_DISPLAY_TIME);

  lcd.setCursor(0, 0);
  lcd.print("Morse Code:");
  lcd.setCursor(0, 1);

  strcpy(row1, blankrow);
  strcpy(row2, blankrow);
  strcpy(row3, blankrow);
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

  lcd.init();
  lcd.backlight();

  resetSystem();
}

// Write a row buffer to the LCD one byte per column, bypassing the library's
// UTF-8 decoding so each stored byte maps to exactly one display cell.
void writeRow(const char *s) {
  while (*s)
    lcd.write((uint8_t)*s++);
}

// Print one character to the bottom row. When the row fills, scroll the text
// rows up (row3 -> row2 -> row1) and blank the bottom row.
void displayChar(char ch) {

  if (initialChar) { // swallow the spurious first char after a reset
    initialChar = false;
    return;
  }

  lcd.setCursor(displayPos, BOTTOM_ROW);
  lcd.write((uint8_t)ch); // byte-for-byte write, no UTF-8 decoding (one cell)

  row3[displayPos] = ch;
  displayPos++;

  if (displayPos >= LCD_COLS) { // bottom row full: scroll everything up
    Serial.println("Row full, scrolling display.");

    displayPos = 0;

    strcpy(row1, row2);
    strcpy(row2, row3);
    strcpy(row3, blankrow);

    lcd.setCursor(0, 1);
    writeRow(row1);

    lcd.setCursor(0, 2);
    writeRow(row2);

    lcd.setCursor(0, BOTTOM_ROW);
    writeRow(row3); // now blank

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
