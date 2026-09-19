/**
 * Auto-scan finder for the 40x4 parallel LCD's pinout when all that's known
 * is which 8 Arduino pins are wired to the display, not which LCD signal
 * (RS, RW, E1, E2, D4-D7) each one carries.
 *
 * A blind full search over all 8! = 40320 orderings is impractical to sit
 * through even automated, so this runs two smaller phases instead:
 *
 *   PHASE_CONTROL cycles all 8*7*6*5 = 1680 ways to assign 4 of the 8 pins
 *   to RS/RW/E1/E2 (the other 4 go to D4-D7 in a fixed, arbitrary order).
 *   Correct RS/RW/E1/E2 makes the WHOLE screen fill with one repeated
 *   glyph, even if that glyph is wrong (D4-D7 order is still a guess) -
 *   a uniform screen means "electrically alive", noise/blank means "still
 *   wrong". ~1680 combos at DWELL_MS each.
 *
 *   PHASE_DATA fixes RS/RW/E1/E2 to whatever PHASE_CONTROL found and
 *   brute-forces the remaining 4 pins as D4-D7 (4! = 24 combos) until the
 *   glyph shown is actually a correct 'A', not just uniform.
 *
 * There's no spare button on this board to freeze a combo, so this uses the
 * Serial Monitor instead: leave it open, watch the LCD, and the moment the
 * screen looks right, type anything into the Serial Monitor send box and
 * hit Send. The sketch stops advancing and reprints the winning combo.
 * Reset the board to resume scanning from the start.
 *
 * Wiring: cw_station's LCD_FAST_40X4 backend, ported to pins 2,3,4,5,6,7,10,11
 * (role assignment unconfirmed - that's what this sketch is for).
 */

#include "LiquidCrystalFast.h"

#define PHASE_CONTROL 0
#define PHASE_DATA 1

#define PHASE PHASE_CONTROL

const uint8_t CANDIDATE_PINS[8] = {2, 3, 4, 5, 6, 7, 10, 11};
#define NUM_PINS 8

// Only used in PHASE_DATA. Fill these in with whatever PHASE_CONTROL found
// before switching PHASE above.
const uint8_t CONFIRMED_RS = 2;
const uint8_t CONFIRMED_RW = 3;
const uint8_t CONFIRMED_E1 = 4;
const uint8_t CONFIRMED_E2 = 5;

#define DWELL_MS 600

LiquidCrystalFast *lcd = NULL;

unsigned long comboIndex = 0;
unsigned long lastAdvance = 0;
bool frozen = false;

void fillScreen(char c)
{
  for (uint8_t row = 0; row < 4; row++)
  {
    lcd->setCursor(0, row);
    for (uint8_t col = 0; col < 40; col++)
      lcd->write((uint8_t)c);
  }
}

void printCombo(uint8_t rs, uint8_t rw, uint8_t e1, uint8_t e2,
                uint8_t d4, uint8_t d5, uint8_t d6, uint8_t d7)
{
  Serial.print("#");
  Serial.print(comboIndex);
  Serial.print(" RS=");
  Serial.print(rs);
  Serial.print(" RW=");
  Serial.print(rw);
  Serial.print(" E1=");
  Serial.print(e1);
  Serial.print(" E2=");
  Serial.print(e2);
  Serial.print(" D4=");
  Serial.print(d4);
  Serial.print(" D5=");
  Serial.print(d5);
  Serial.print(" D6=");
  Serial.print(d6);
  Serial.print(" D7=");
  Serial.println(d7);
}

void tryCombo(uint8_t rs, uint8_t rw, uint8_t e1, uint8_t e2,
              uint8_t d4, uint8_t d5, uint8_t d6, uint8_t d7)
{
  printCombo(rs, rw, e1, e2, d4, d5, d6, d7);

  // Passing a real RW pin to the library makes send() busy-poll a data pin
  // in an unbounded loop - fine once RW is confirmed, but on a wrong guess
  // that bit never clears and the whole scan hangs. Always construct with
  // RW=255 (fixed-delay write path, matches this project's known-working
  // "RW tied to GND" mode) and just hold our RW candidate low ourselves, so
  // a correct guess still puts the real display in write mode.
  pinMode(rw, OUTPUT);
  digitalWrite(rw, LOW);

  if (lcd)
    delete lcd;
  lcd = new LiquidCrystalFast(rs, 255, e1, e2, d4, d5, d6, d7);
  lcd->begin(40, 4);
  fillScreen('A'); // fixed glyph; PHASE_CONTROL only cares whether it's uniform
  comboIndex++;
}

bool nextPermutation(uint8_t *a, uint8_t n)
{
  int i = n - 2;
  while (i >= 0 && a[i] >= a[i + 1])
    i--;
  if (i < 0)
    return false;
  int j = n - 1;
  while (a[j] <= a[i])
    j--;
  uint8_t t = a[i];
  a[i] = a[j];
  a[j] = t;
  for (int l = i + 1, r = n - 1; l < r; l++, r--)
  {
    t = a[l];
    a[l] = a[r];
    a[r] = t;
  }
  return true;
}

#if PHASE == PHASE_CONTROL

void runNextCombo()
{
  static uint8_t i = 0, j = 0, k = 0, l = 0;

  // advance the 4-of-8 ordered selection (i,j,k,l all distinct indices into
  // CANDIDATE_PINS), skipping repeats - a base-8 4-digit odometer
  while (true)
  {
    l++;
    if (l >= NUM_PINS)
    {
      l = 0;
      k++;
    }
    if (k >= NUM_PINS)
    {
      k = 0;
      j++;
    }
    if (j >= NUM_PINS)
    {
      j = 0;
      i++;
    }
    if (i >= NUM_PINS)
    {
      i = 0;
      Serial.println("=== wrapped: exhausted all 1680 combos, restarting ===");
    }
    if (i != j && i != k && i != l && j != k && j != l && k != l)
      break;
  }

  uint8_t rs = CANDIDATE_PINS[i];
  uint8_t rw = CANDIDATE_PINS[j];
  uint8_t e1 = CANDIDATE_PINS[k];
  uint8_t e2 = CANDIDATE_PINS[l];

  // remaining 4 pins, in ascending order, fixed as D4-D7 for this phase
  uint8_t rest[4];
  uint8_t n = 0;
  for (uint8_t p = 0; p < NUM_PINS; p++)
  {
    if (p != i && p != j && p != k && p != l)
      rest[n++] = CANDIDATE_PINS[p];
  }

  tryCombo(rs, rw, e1, e2, rest[0], rest[1], rest[2], rest[3]);
}

#else // PHASE_DATA

void runNextCombo()
{
  static uint8_t perm[4] = {0, 1, 2, 3};
  static bool first = true;

  uint8_t rest[4];
  uint8_t n = 0;
  for (uint8_t p = 0; p < NUM_PINS; p++)
  {
    uint8_t pin = CANDIDATE_PINS[p];
    if (pin != CONFIRMED_RS && pin != CONFIRMED_RW && pin != CONFIRMED_E1 && pin != CONFIRMED_E2)
      rest[n++] = pin;
  }

  if (!first)
  {
    if (!nextPermutation(perm, 4))
    {
      Serial.println("=== wrapped: exhausted all 24 D4-D7 orders, restarting ===");
      perm[0] = 0;
      perm[1] = 1;
      perm[2] = 2;
      perm[3] = 3;
    }
  }
  first = false;

  tryCombo(CONFIRMED_RS, CONFIRMED_RW, CONFIRMED_E1, CONFIRMED_E2,
           rest[perm[0]], rest[perm[1]], rest[perm[2]], rest[perm[3]]);
}

#endif

void setup()
{
  Serial.begin(9600);
  Serial.println("LCD pin finder starting.");
#if PHASE == PHASE_CONTROL
  Serial.println("PHASE_CONTROL: searching for RS/RW/E1/E2 (1680 combos).");
  Serial.println("Watch for the WHOLE screen filling with ONE repeated character - any character, just uniform, not noise or blank. D4-D7 order is still a guess so the glyph itself may be wrong.");
#else
  Serial.println("PHASE_DATA: RS/RW/E1/E2 fixed, searching D4-D7 order (24 combos).");
  Serial.println("Watch for the screen filling with actual capital 'A' characters.");
#endif
  Serial.println("No spare button on this board - type anything into Serial Monitor and hit Send the moment it looks right; the sketch freezes and reprints the winning combo. Reset the board to restart the scan.");
  lastAdvance = millis();
}

void loop()
{
  if (Serial.available())
  {
    while (Serial.available())
      Serial.read();
    frozen = true;
    Serial.println(">>> FROZEN on the combo above. Reset the board to resume scanning. <<<");
  }

  if (frozen)
    return;

  if (millis() - lastAdvance >= DWELL_MS)
  {
    runNextCombo();
    lastAdvance = millis();
  }
}
