/**
 * 40x4 LCD over I2C - hardware check.
 *
 * Flash this before flashing the real station firmware. It proves out the one
 * thing that is easy to get wrong on a 40x4: that BOTH controllers are being
 * strobed. Rows 0-1 belong to controller 1 (E1) and rows 2-3 to controller 2
 * (E2), so a half-working display is the normal failure and it only shows up
 * if each row says something different.
 *
 * Wiring is in ../../docs/WIRING.md. The short version: one PCF8574
 * backpack, the panel's R/W pin tied to GND, and the backpack's P1 output
 * (header pin 5, normally R/W) re-routed to the panel's E2 pin.
 *
 * Run tools/i2c_scanner first to confirm the address, then set LCD_I2C_ADDR.
 *
 * Expected result, and what each failure means:
 *
 *   ROW 0 ... E1 / chip 1     all four rows readable -> wiring is good
 *   ROW 1 ... E1 / chip 1     rows 0-1 only          -> E2 not connected
 *   ROW 2 ... E2 / chip 2     rows 2-3 only          -> E1 and E2 swapped
 *   ROW 3 ... E2 / chip 2     all solid blocks       -> R/W not at GND, or
 *                                                       contrast needs turning
 *
 * After the labelled rows it runs a column ruler and a full-screen fill, so a
 * dead column or a row that wraps to the wrong place shows up too.
 */

#include <Wire.h>

#include "LiquidCrystalI2C_Dual.h"

#define LCD_I2C_ADDR 0x27 // set to whatever tools/i2c_scanner reported
#define LCD_COLS 40
#define LCD_ROWS 4

// The final argument is what makes this a 40x4: two controllers, so the driver
// strobes E2 for rows 2-3 instead of E1.
LiquidCrystalI2C_Dual lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS, 2);

#define STEP_MS 4000

void banner(const char *what)
{
  Serial.print(F("--- "));
  Serial.println(what);
}

// Every row labelled with its number and which controller owns it. This is the
// test that actually matters.
void testRows()
{
  banner("labelled rows");
  lcd.clear();
  for (uint8_t r = 0; r < LCD_ROWS; r++)
  {
    lcd.setCursor(0, r);
    lcd.print(F("ROW "));
    lcd.print(r);
    lcd.print(r < 2 ? F("  E1 / chip 1  ") : F("  E2 / chip 2  "));
    lcd.print(F("<-- all 4 rows?"));
  }
}

// A ruler across all 40 columns, so a stuck data line or a short shows up as a
// gap or a repeated digit.
void testColumns()
{
  banner("column ruler");
  lcd.clear();
  for (uint8_t r = 0; r < LCD_ROWS; r++)
  {
    lcd.setCursor(0, r);
    for (uint8_t c = 0; c < LCD_COLS; c++)
    {
      // 0123456789 repeating, with every tenth column marked.
      lcd.write((c % 10 == 0) ? '|' : (char)('0' + (c % 10)));
    }
  }
}

// Fill every cell. Any cell that stays blank is a cell the driver never
// addressed; any that stays black is a cell that never got data.
void testFill()
{
  banner("full fill");
  lcd.clear();
  for (uint8_t r = 0; r < LCD_ROWS; r++)
  {
    lcd.setCursor(0, r);
    for (uint8_t c = 0; c < LCD_COLS; c++)
    {
      lcd.write((uint8_t)('A' + ((r * LCD_COLS + c) % 26)));
    }
  }
}

// Prove clear() reaches both controllers. If the bottom two rows keep their
// text here, clear() is only landing on chip 1.
void testClear()
{
  banner("clear both chips (screen should go blank)");
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("cleared - rows 1-3 should be blank"));
}

void setup()
{
  Serial.begin(9600);
  Serial.println(F("40x4 I2C LCD test"));
  Serial.print(F("address 0x"));
  Serial.println(LCD_I2C_ADDR, HEX);

  Wire.begin();
  Wire.setWireTimeout(25000, true); // don't let a flaky display wedge the bus

  lcd.init();
  lcd.backlight();
}

void loop()
{
  testRows();
  delay(STEP_MS);
  testColumns();
  delay(STEP_MS);
  testFill();
  delay(STEP_MS);
  testClear();
  delay(STEP_MS);
}
