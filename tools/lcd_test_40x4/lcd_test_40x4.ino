/**
 * Bare 40x4 parallel LCD test - isolates which part of the display path is
 * actually broken, since the known-working reference sketch
 * (morse_code_station_40-4_LCD.ino, VK6TU Nov 2024) only ever writes to
 * rows 0-1 (the E1 chip). It never proves rows 2-3 (E2) or an explicit
 * lcd.clear() are okay, and our station sketch's welcomeBanner() does both
 * of those things as the very first thing put on screen.
 *
 * Pick ONE phase with TEST_PHASE below, flash, and see what shows up.
 * Nothing after setup() touches the LCD again, so what you see is exactly
 * what that phase produced - no cycling to confuse the picture.
 *
 *   PHASE_ROWS_01   rows 0-1 only (E1 chip), no clear() call at all - same
 *                   shape as the reference sketch that reportedly works.
 *   PHASE_ROWS_23   rows 2-3 only (E2 chip), no clear() call at all -
 *                   isolates the second controller/enable line on its own.
 *   PHASE_ALL_CLEAR clear() then all four rows - same shape as
 *                   welcomeBanner() in the station sketch.
 *
 * Test PHASE_ROWS_01 and PHASE_ROWS_23 first. If 01 is clean but 23 is
 * garbage, it's an E2/rows2-3 wiring fault (check that connection first).
 * If both are clean but PHASE_ALL_CLEAR is garbage, the bug is calling
 * clear() with both chips already initialised. If everything is garbage
 * here too, it's back to a wiring/library-level problem rather than
 * anything in the station sketch's logic.
 *
 * Wiring: same as display_cw_station's 40x4 backend -
 *   D5 RS, D6 R/W, D4 E1, D8 E2, D12 D4, D11 D5, D10 D6, D9 D7
 */

#include "LiquidCrystalFast.h"

#define PHASE_ROWS_01 0
#define PHASE_ROWS_23 1
#define PHASE_ALL_CLEAR 2

#define TEST_PHASE PHASE_ALL_CLEAR

// (RS, RW, E1, E2, D4, D5, D6, D7)
LiquidCrystalFast lcd(5, 6, 4, 8, 12, 11, 10, 9);

void setup() {
  Serial.begin(9600);
  Serial.print("40x4 LCD phase test starting, TEST_PHASE=");
  Serial.println(TEST_PHASE);

#if TEST_PHASE == PHASE_ROWS_01
  lcd.begin(40, 4);
  lcd.setCursor(0, 0);
  lcd.print("PHASE_ROWS_01 ROW0: E1 chip test");
  lcd.setCursor(0, 1);
  lcd.print("PHASE_ROWS_01 ROW1: ABCDEFGHIJKL");
#elif TEST_PHASE == PHASE_ROWS_23
  lcd.begin(40, 4);
  lcd.setCursor(0, 2);
  lcd.print("PHASE_ROWS_23 ROW2: E2 chip test");
  lcd.setCursor(0, 3);
  lcd.print("PHASE_ROWS_23 ROW3: MNOPQRSTUVWX");
#elif TEST_PHASE == PHASE_ALL_CLEAR
  lcd.begin(40, 4);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("PHASE_ALL_CLEAR ROW0");
  lcd.setCursor(0, 1);
  lcd.print("PHASE_ALL_CLEAR ROW1");
  lcd.setCursor(0, 2);
  lcd.print("PHASE_ALL_CLEAR ROW2");
  lcd.setCursor(0, 3);
  lcd.print("PHASE_ALL_CLEAR ROW3");
#endif
}

void loop() {
  // Nothing here on purpose - what setup() drew is the whole test.
}
