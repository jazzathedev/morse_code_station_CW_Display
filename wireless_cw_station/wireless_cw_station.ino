/**
 * A wireless Morse code station for the Arduino Nano, driving a 20x4 (or 16x2)
 * character LCD over I2C and an NRF24L01 radio.
 *
 * Two units form a link. The local straight key on KEY_PIN is transmitted to the
 * remote unit over the radio (TX), and the remote unit's keying is received (RX)
 * and shown on this LCD. By default the screen shows the raw keyed dots and
 * dashes of the remote sender; flipping the MODE switch re-renders the same
 * buffered keying as decoded text for kids to read.
 *
 * Radio link logic is based on the cw-send-receive-arduino NRF24 sketch; the
 * decode/display work is shared with the wired display station (morse.*).
 *
 * Credits: original Morse station by Mario Gianota; LED, row-display, timing and
 * decode work by VK6TU and VK6XM; wireless integration by Jazza.
 *
 * Licensed under GPLv3.
 */

#include <SPI.h>
#include "RF24.h"
#include <Wire.h>
#include <EEPROM.h>
#include <string.h>

#include "morse.h" // dot/dash classification + decodeMorse*() lookups

// LCD geometry and I2C address. Must match the physical display; the content
// rendering below derives from these. Wiring: SCL->A5, SDA->A4, 5V, GND.
#define LCD_I2C_ADDR 0x27
#define LCD_COLS 20
#define LCD_ROWS 4

// Row 0 is the header (unit/mode label + live dot/dash indicator). The remaining
// rows scroll the received keying.
#define HEADER_ROW 0
#define CONTENT_ROWS (LCD_ROWS - 1)

#include <LCDI2C_Generic.h>
LCDI2C_Generic lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);

#define VER 2
#define SUBVER 0

// Stringize VER/SUBVER so the banner can show "v2.0" without hardcoding it.
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define VERSION_STR "v" STR(VER) "." STR(SUBVER)

// --- Pin map (wireless Nano board) -----------------------------------------
// Buttons/switches are active-low and use the internal pull-ups.
#define BUZZER_PIN 2
#define KEY_PIN 3         // local straight key (to transmit)
#define CONFIRM_LED_PIN 4 // yellow: lit while transmitting the local key
#define STATUS_LED_PIN 5  // red: lit while receiving the remote key
#define MODE_SWITCH_PIN 6 // toggles raw dots/dashes vs decoded text
#define CLEAR_BUTTON 7    // soft-restart (clears screen and state)
#define RADIO_CE_PIN 9
#define RADIO_CSN_PIN 8
// SPI for the radio is fixed on the Nano: D11 MOSI, D12 MISO, D13 SCK.

RF24 radio(RADIO_CE_PIN, RADIO_CSN_PIN);

// The unit number (which board this is) is chosen at boot by tapping the key and
// stored in EEPROM - see selectUnitNumber(). It selects the radio pipes and
// sidetone pitches so the two units pair up. The two units must be different
// numbers (1 and 2) to talk.
#define DEFAULT_UNIT 1     // used when EEPROM holds nothing valid
#define MAX_UNITS 2        // tap-cycle wraps 1 -> 2 -> ... -> 1
#define UNIT_EEPROM_ADDR 0 // byte offset where the unit number is stored
// The chooser shares the start-up banner's window (BANNER_DISPLAY_TIME) - tap
// during the banner to cycle the unit, no extra boot delay.

uint8_t unitNumber = DEFAULT_UNIT; // this board's number, set in selectUnitNumber()

const byte ADDRESS[][6] = {"pipe1", "pipe2"}; // two-way comms addresses
#define RADIO_CHANNEL 76                      // 0-83 legal in AU, maps to 2400+N MHz

// Sidetone pitch (Hz), derived from unitNumber after selection. You hear your
// own key at your unit's pitch and the remote key at theirs, so the two are easy
// to tell apart.
int toneLocalHz;
int toneRemoteHz;

// Inter-symbol gap timing (shared with the wired station). Idle past
// LETTER_GAP_MS ends the current letter; idle past WORD_GAP_MS ends the word.
#define LETTER_GAP_MS 600
#define WORD_GAP_MS 1600

// Local-key debounce. Kept below a dot's duration so fast keying still
// registers (dotTimeMillisMin is 40ms in morse.cpp).
#define DEBOUNCE_THRESHOLD_MS 15

// After receiving, hold off transmitting this long to enforce turn-taking and
// avoid two units keying over each other (half-duplex lockout).
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

// Display mode select.
//   MODE_SWITCH_ENABLED 1 - the D6 switch toggles raw dots/dashes vs text live.
//   MODE_SWITCH_ENABLED 0 - ignore the switch and stay fixed on START_MODE_DECODED
//                           (handy when the MODE switch isn't wired up yet).
// START_MODE_DECODED: 0 = raw dots/dashes, 1 = decoded text. It is the boot mode
// when the switch is enabled, and the only mode when it is disabled. So to force
// letter display, set MODE_SWITCH_ENABLED 0 and START_MODE_DECODED 1.
#define MODE_SWITCH_ENABLED 0
#define START_MODE_DECODED 0

// Live dot/dash indicator: top-right of the header row, showing the symbols of
// the remote letter currently being keyed.
#define DOTDASH_DISPLAY_CELLS 6
const unsigned int dotDashActivityX = 14;

// MAX_PATTERN (the longest morse pattern, 7 symbols) comes from morse.h.

// Received-keying history. Each entry is a decoded letter's morse pattern (e.g.
// ".-"), or an empty string as a word-gap marker. The screen is re-rendered from
// this buffer, so the MODE switch can redraw it as dots/dashes or as text.
// MAX_HISTORY = LCD_COLS * CONTENT_ROWS fills the content rows in text mode.
#define MAX_HISTORY (LCD_COLS * CONTENT_ROWS)
char history[MAX_HISTORY][MAX_PATTERN + 1];
int historyCount = 0;

// The remote letter being assembled, one symbol per received key-up.
char currentPattern[MAX_PATTERN + 1];
int patternLen = 0;

// Decode state machine for the received keying:
//   letterPending - symbols are buffered but not yet committed to a letter
//   wordPending   - a letter has been keyed, so a long gap should add a space
bool letterPending = false;
bool wordPending = false;
bool modeDecoded = false; // false = raw dots/dashes, true = decoded text

// Remote key edge tracking.
bool rxKeyDown = false;
unsigned long rxKeyDownStart = 0; // millis() of the current remote key-down
unsigned long lastRxActivity = 0; // millis() of the last remote key edge, for gaps

// Local key (TX) state, mirrored from the cw-send-receive radio sketch.
bool buttonState = false; // last received remote state (LOW = key down)
unsigned long timeOfLastReceive = 0;
unsigned long lastDebounce = 0;
bool rawButtonState = HIGH; // active-low key: HIGH = released
bool debouncedButtonState = HIGH;

// --- Radio ------------------------------------------------------------------

// Flash both LEDs (and show RADIO FAULT) for PANIC_BLINK_MS, then return so the
// caller can retry. Not a permanent lockup - a radio that comes good recovers.
void panic()
{
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("RADIO FAULT");

  unsigned long start = millis();
  while (millis() - start < PANIC_BLINK_MS)
  {
    digitalWrite(CONFIRM_LED_PIN, HIGH);
    digitalWrite(STATUS_LED_PIN, HIGH);
    delay(100);
    digitalWrite(CONFIRM_LED_PIN, LOW);
    digitalWrite(STATUS_LED_PIN, LOW);
    delay(100);
  }
}

void setupRadio()
{
  // Keep retrying instead of bricking: panic() flashes for its timeout, then we
  // loop and try again, so the unit recovers once the module is reachable.
  while (!radio.begin())
  {
    Serial.println("Radio init FAILED");
    panic();
  }
  Serial.println("Radio init ok");

  // Board 1 writes to pipe1 and reads pipe2; board 2 is the mirror image.
  if (unitNumber == 1)
  {
    radio.openWritingPipe(ADDRESS[0]);
    radio.openReadingPipe(1, ADDRESS[1]);
  }
  else
  {
    radio.openWritingPipe(ADDRESS[1]);
    radio.openReadingPipe(1, ADDRESS[0]);
  }

  radio.setPALevel(RF24_PA_LOW);
  radio.setDataRate(RF24_250KBPS);
  radio.setChannel(RADIO_CHANNEL);
  radio.setAutoAck(true);
  radio.setRetries(5, 15); // 1500us between retries, up to 15 attempts
  radio.startListening();
}

// --- Display ----------------------------------------------------------------

// Header row: unit number + current mode on the left, live dot/dash indicator
// for the in-progress remote letter on the right.
void showHeader()
{
  lcd.setCursor(0, HEADER_ROW);
  lcd.print("U");
  lcd.print(unitNumber);
  lcd.print(modeDecoded ? " CW TEXT" : " CW");
  lcd.print("       "); // pad up to the live indicator at column 14
}

// Show the symbols of the remote letter currently being keyed, top-right.
void showRxActivity()
{
  lcd.setCursor(dotDashActivityX, HEADER_ROW);
  for (int i = 0; i < DOTDASH_DISPLAY_CELLS; i++)
  {
    char sym = i < patternLen ? currentPattern[i] : ' ';
    // The pattern stores '.'/'-' for decoding, but the ASCII full stop sits on
    // the baseline. Swap in the ROM's centered dot (0xA5) so dots and dashes
    // line up vertically. The dash is already centered, so it passes through.
    lcd.write((uint8_t)(sym == '.' ? 0xA5 : sym));
  }
}

void clearRxActivity()
{
  patternLen = 0;
  currentPattern[0] = '\0';
  showRxActivity();
}

// Width in display cells of history entry i in the current mode.
int entryWidth(int i)
{
  bool gap = (history[i][0] == '\0');
  if (modeDecoded)
  {
    return 1; // a single letter, or a single space for a word gap
  }
  return gap ? 2 : (int)strlen(history[i]) + 1; // pattern + separator, or "/ "
}

// Re-render the content rows from the history buffer for the current mode,
// showing the most recent entries that fit.
void renderHistory()
{
  const int cells = LCD_COLS * CONTENT_ROWS;

  // Walk back from the newest entry until the tail no longer fits.
  int start = historyCount;
  int total = 0;
  for (int i = historyCount - 1; i >= 0; i--)
  {
    int w = entryWidth(i);
    if (total + w > cells)
    {
      break;
    }
    total += w;
    start = i;
  }

  // Lay the visible entries out left-to-right, top-to-bottom into a flat buffer.
  char buf[LCD_COLS * CONTENT_ROWS];
  memset(buf, ' ', sizeof(buf));
  int pos = 0;
  for (int i = start; i < historyCount && pos < cells; i++)
  {
    bool gap = (history[i][0] == '\0');
    if (modeDecoded)
    {
      buf[pos++] = gap ? ' ' : decodeMorsePattern(history[i]);
    }
    else if (gap)
    {
      buf[pos++] = '/';
      if (pos < cells)
      {
        buf[pos++] = ' ';
      }
    }
    else
    {
      for (const char *p = history[i]; *p && pos < cells; p++)
      {
        // Centered dot glyph (0xA5) for displayed dots; see showRxActivity().
        buf[pos++] = (*p == '.') ? (char)0xA5 : *p;
      }
      if (pos < cells)
      {
        buf[pos++] = ' ';
      }
    }
  }

  // Byte-for-byte write so each stored byte maps to exactly one display cell.
  for (int r = 0; r < CONTENT_ROWS; r++)
  {
    lcd.setCursor(0, HEADER_ROW + 1 + r);
    for (int c = 0; c < LCD_COLS; c++)
    {
      lcd.write((uint8_t)buf[r * LCD_COLS + c]);
    }
  }
}

// Append a history entry, dropping the oldest if the buffer is full.
void pushHistory(const char *entry)
{
  if (historyCount >= MAX_HISTORY)
  {
    for (int i = 1; i < MAX_HISTORY; i++)
    {
      strcpy(history[i - 1], history[i]);
    }
    historyCount = MAX_HISTORY - 1;
  }
  strncpy(history[historyCount], entry, MAX_PATTERN);
  history[historyCount][MAX_PATTERN] = '\0';
  historyCount++;
}

void welcomeBanner(int waitDelay)
{
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(" -- SCOUTS WA --");
  lcd.setCursor(0, 1);
  lcd.print(" Wireless CW");
  lcd.setCursor(0, 2);
  lcd.print(VERSION_STR "  Unit ");
  lcd.print(unitNumber);
  lcd.setCursor(0, 3);
  lcd.print("By VK6TU/VK6XM");
  delay(waitDelay);
  lcd.clear();
}

// Boot banner that doubles as the unit chooser - shows the unit number and the
// "tap key" hint. Redrawn each time the number changes.
void drawBootBanner()
{
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(" -- SCOUTS WA --");
  lcd.setCursor(0, 1);
  lcd.print(" Wireless CW " VERSION_STR);
  lcd.setCursor(0, 2);
  lcd.print(" Unit ");
  lcd.print(unitNumber);
  lcd.setCursor(0, 3);
  lcd.print("Tap key to change");
}

// Boot-time unit chooser. Loads the saved unit from EEPROM, then for the banner
// window each key tap cycles the number (1 -> 2 -> ... -> 1). The result is saved
// back to EEPROM so it sticks across power cycles, and confirmed with a beep per
// unit so it's readable even with no display. Runs before the radio listens, so
// taps don't transmit. Shares BANNER_DISPLAY_TIME - no extra boot delay.
void selectUnitNumber()
{
  unitNumber = EEPROM.read(UNIT_EEPROM_ADDR);
  if (unitNumber < 1 || unitNumber > MAX_UNITS)
  {
    unitNumber = DEFAULT_UNIT; // fresh/invalid EEPROM (e.g. 0xFF)
  }
  uint8_t startUnit = unitNumber;

  drawBootBanner();

  // Wait for the banner window to elapse with no tap. Each tap cycles the number
  // and restarts the window, so it confirms 3s after your last press.
  bool prevKey = HIGH; // active-low key, idle HIGH
  unsigned long lastActivity = millis();
  while (millis() - lastActivity < BANNER_DISPLAY_TIME)
  {
    bool k = digitalRead(KEY_PIN);
    if (prevKey == HIGH && k == LOW) // falling edge = a tap
    {
      unitNumber = (unitNumber % MAX_UNITS) + 1; // cycle 1..MAX_UNITS
      tone(BUZZER_PIN, 880, 60);                 // short tap blip
      drawBootBanner();
      lastActivity = millis(); // restart the window from this tap
      delay(50);               // debounce the tap
    }
    prevKey = k;
  }

  if (unitNumber != startUnit)
  {
    EEPROM.update(UNIT_EEPROM_ADDR, unitNumber); // update() only writes on change
  }

  // Confirm the landing number: one beep/blink per unit.
  for (uint8_t i = 0; i < unitNumber; i++)
  {
    digitalWrite(CONFIRM_LED_PIN, HIGH);
    tone(BUZZER_PIN, 660, 120);
    delay(180);
    digitalWrite(CONFIRM_LED_PIN, LOW);
    delay(120);
  }
}

// Reset operating state and draw the live screen, without the banner. Used at
// boot, where the unit chooser has already shown the banner for its window.
void initStation()
{
  historyCount = 0;
  patternLen = 0;
  currentPattern[0] = '\0';
  letterPending = false;
  wordPending = false;
  rxKeyDown = false;
  lastRxActivity = millis();

  digitalWrite(CONFIRM_LED_PIN, LOW);
  digitalWrite(STATUS_LED_PIN, LOW);
  noTone(BUZZER_PIN);

  lcd.clear();
  showHeader();
  showRxActivity();
  renderHistory();
}

// Soft restart (clear button): show the banner, then reset state.
void resetStation()
{
  welcomeBanner(BANNER_DISPLAY_TIME);
  initStation();
}

void setup()
{
  Serial.begin(9600); // serial debug output

  pinMode(KEY_PIN, INPUT_PULLUP);
  pinMode(MODE_SWITCH_PIN, INPUT_PULLUP);
  pinMode(CLEAR_BUTTON, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(CONFIRM_LED_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);

  // Bring up I2C with a timeout so a missing, half-plugged or flaky display
  // can't wedge the bus and freeze the radio. The unit runs fine headless -
  // writes to an absent LCD are just NACKed and ignored.
  Wire.begin();
  Wire.setWireTimeout(25000, true); // 25ms, reset the bus on timeout

  lcd.init();
  lcd.backlight();

  // Choose this unit's number (tap the key during the banner), then derive the
  // sidetone pitches from it before the radio pipes are set up.
  selectUnitNumber();
  toneLocalHz = (unitNumber == 1) ? 600 : 900;
  toneRemoteHz = (unitNumber == 1) ? 900 : 600;

  setupRadio();

#if MODE_SWITCH_ENABLED
  modeDecoded = (digitalRead(MODE_SWITCH_PIN) == LOW);
#else
  modeDecoded = START_MODE_DECODED;
#endif
  initStation(); // chooser already showed the banner; just draw the live screen
}

/* ***********************************************
MAIN LOOP HERE
************************************************ */
void loop()
{
  scanControls();          // mode switch + clear button
  transmitLocalKey();      // send the local key over the radio
  receiveRemoteKey();      // receive the remote key and feed the decoder
  releaseStuckRemoteKey(); // recover if a key-up packet was lost
  updateDecodeTiming();    // gaps end the current letter/word
}
/* ***********************************************
END: MAIN LOOP
************************************************ */

// Clear button soft-restarts; mode switch re-renders the existing keying.
void scanControls()
{
  if (digitalRead(CLEAR_BUTTON) == LOW)
  {
    resetStation();
    return;
  }

#if MODE_SWITCH_ENABLED
  bool wantDecoded = (digitalRead(MODE_SWITCH_PIN) == LOW);
  if (wantDecoded != modeDecoded)
  {
    modeDecoded = wantDecoded;
    showHeader();
    renderHistory();
  }
#endif
}

// Transmit the local key state to the remote unit when it changes. Mirrors the
// cw-send-receive radio sketch: debounce, half-duplex lockout, TX confirmation.
void transmitLocalKey()
{
  bool currentReading = digitalRead(KEY_PIN);
  unsigned long currentTime = millis();

  if (currentReading != rawButtonState)
  {
    lastDebounce = currentTime;
    rawButtonState = currentReading;
  }

  // Don't transmit just after receiving, so the two units take turns.
  if (currentTime - timeOfLastReceive < RX_TX_LOCKOUT_MS)
  {
    return;
  }

  if ((currentTime - lastDebounce) > DEBOUNCE_THRESHOLD_MS &&
      currentReading != debouncedButtonState)
  {
    debouncedButtonState = currentReading;

    radio.stopListening(); // leave RX only for the actual transmit
    bool txOk = radio.write(&debouncedButtonState, sizeof(debouncedButtonState));
    radio.startListening();
    // A failed write just means the peer didn't ack this packet (out of range,
    // powered off, or RF noise). That's normal operation, not a fault - log it
    // and carry on; the local sidetone/LED below still track the key.
    Serial.println(txOk ? "TX ok" : "TX FAILED");

    if (debouncedButtonState == LOW)
    { // active-low key: LOW = pressed
      digitalWrite(CONFIRM_LED_PIN, HIGH);
      tone(BUZZER_PIN, toneLocalHz);
    }
    else
    {
      digitalWrite(CONFIRM_LED_PIN, LOW);
      noTone(BUZZER_PIN);
    }
  }

  delay(5);
}

// Receive the remote key state and drive the LED, sidetone and decoder.
void receiveRemoteKey()
{
  if (!radio.available())
  {
    return;
  }

  radio.read(&buttonState, sizeof(buttonState));
  timeOfLastReceive = millis();
  Serial.print("Received: ");
  Serial.println(buttonState);

  bool down = (buttonState == LOW); // active-low key: LOW = remote key down
  if (down && !rxKeyDown)
  {
    onRemoteKeyDown();
  }
  else if (!down && rxKeyDown)
  {
    onRemoteKeyUp();
  }
  rxKeyDown = down;
}

void onRemoteKeyDown()
{
  rxKeyDownStart = millis();
  lastRxActivity = rxKeyDownStart;
  digitalWrite(STATUS_LED_PIN, HIGH);
  tone(BUZZER_PIN, toneRemoteHz);
}

void onRemoteKeyUp()
{
  unsigned long held = millis() - rxKeyDownStart;
  lastRxActivity = millis();
  digitalWrite(STATUS_LED_PIN, LOW);
  noTone(BUZZER_PIN);

  // Classify the held duration; anything too short to be a dot is ignored as
  // bounce. A real symbol extends the current letter and updates the indicator.
  char sym = symbolFor(held);
  if (sym != '\0' && patternLen < MAX_PATTERN)
  {
    currentPattern[patternLen++] = sym;
    currentPattern[patternLen] = '\0';
    letterPending = true;
    wordPending = true;
    showRxActivity();
  }
}

// If a key-up packet was lost the remote key stays "down" forever, jamming the
// buzzer/LED on. Once it has been held longer than any real element, drop it.
// The runaway duration isn't a valid dot/dash, so we discard it rather than
// record a bogus symbol.
void releaseStuckRemoteKey()
{
  if (rxKeyDown && (millis() - rxKeyDownStart > STUCK_KEY_MS))
  {
    rxKeyDown = false;
    lastRxActivity = millis();
    digitalWrite(STATUS_LED_PIN, LOW);
    noTone(BUZZER_PIN);
  }
}

// A gap past WORD_GAP_MS ends a word (insert a space). A shorter gap past
// LETTER_GAP_MS ends the current letter (commit its pattern).
void updateDecodeTiming()
{
  unsigned long idle = millis() - lastRxActivity;

  if (idle > WORD_GAP_MS && wordPending)
  {
    pushHistory(""); // word-gap marker
    wordPending = false;
    renderHistory();
  }
  else if (idle > LETTER_GAP_MS && letterPending)
  {
    pushHistory(currentPattern);
    letterPending = false;
    clearRxActivity();
    renderHistory();
  }
}
