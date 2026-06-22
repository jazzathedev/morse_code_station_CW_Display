/**
 * Bare LCD test - no radio, no morse, nothing else. Uses the exact same library,
 * address and constructor as the station sketches, so it isolates the display.
 *
 * Flash it and watch the LCD:
 *   - If you see the four labelled rows and a counter ticking on the bottom row,
 *     the LCD + wiring + library are all good, and any black-boxes problem in the
 *     station sketch is elsewhere.
 *   - If you still get black boxes here, the HD44780 isn't being initialised even
 *     though the I2C backpack answers at 0x27. That's almost always a hardware
 *     joint between the backpack and the LCD's 16-pin header (E or a data line),
 *     or a backpack with a non-standard pin mapping. Reflow those header pins and
 *     re-seat the backpack first.
 *
 * Wiring: SCL->A5, SDA->A4, 5V, GND (same as the station).
 */

#include <Wire.h>
#include <LCDI2C_Generic.h>

#define LCD_I2C_ADDR 0x27
#define LCD_COLS 20
#define LCD_ROWS 4

LCDI2C_Generic lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);

void setup() {
  Serial.begin(9600);
  Serial.println("LCD test starting");

  lcd.init();
  lcd.backlight();

  lcd.setCursor(0, 0);
  lcd.print("LCD TEST 20x4 0x27");
  lcd.setCursor(0, 1);
  lcd.print("ABCDEFGHIJKLMNOPQRST");
  lcd.setCursor(0, 2);
  lcd.print("0123456789 .,?!-/=+@");
  lcd.setCursor(0, 3);
  lcd.print("count: ");
}

void loop() {
  static unsigned int count = 0;

  lcd.setCursor(7, 3);
  lcd.print(count);
  lcd.print("    "); // wipe trailing digits as the number shrinks/grows
  Serial.print("tick ");
  Serial.println(count);

  count++;
  delay(500);
}
