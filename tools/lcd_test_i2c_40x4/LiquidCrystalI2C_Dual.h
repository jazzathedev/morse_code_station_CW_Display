/**
 * HD44780 character LCD over a PCF8574 I2C backpack, with optional support for
 * the two-controller 40x4 panel.
 *
 * A 40x4 module is really two HD44780 chips behind one sheet of glass: rows 0-1
 * belong to chip 1, rows 2-3 to chip 2. They share RS, R/W and the data bus, and
 * differ only in the enable strobe - E1 for the first chip, E2 for the second.
 * A stock backpack has no pin left over for a second enable, so this driver
 * expects R/W to be tied to GND at the panel and the backpack's P1 output (which
 * normally carries R/W) to be re-routed to the panel's E2 pin. See
 * ../docs/WIRING.md for the full pin-by-pin mapping.
 *
 * With R/W grounded the busy flag can never be read, so every operation is paced
 * by a fixed delay - the same approach the stock LiquidCrystal library takes.
 *
 * On a single-controller display (16x2, 20x4) construct with chips = 1 and the
 * backpack is used exactly as shipped: P1 stays LOW throughout, which is R/W low
 * = "write", so no hardware modification is needed.
 *
 * Written for the Scouts WA Morse code station. Licensed under GPLv3.
 */

#ifndef LIQUIDCRYSTALI2C_DUAL_H
#define LIQUIDCRYSTALI2C_DUAL_H

#include <Arduino.h>
#include <Print.h>

class LiquidCrystalI2C_Dual : public Print
{
public:
  // chips = 1 for a normal single-controller module, 2 for a 40x4 panel whose
  // E2 line is driven from the backpack's P1 output.
  LiquidCrystalI2C_Dual(uint8_t addr, uint8_t cols, uint8_t rows,
                        uint8_t chips = 1);

  void begin();
  void init() { begin(); } // alias, matching the LiquidCrystal_I2C-style API

  void clear();
  void home();
  void setCursor(uint8_t col, uint8_t row);

  void backlight();
  void noBacklight();
  void display();
  void noDisplay();

  void createChar(uint8_t location, const uint8_t charmap[]);
  void command(uint8_t value);

  virtual size_t write(uint8_t value);
  using Print::write;

private:
  // Enable-strobe mask for the given controller (0 or 1).
  uint8_t enableMask(uint8_t chip) const;

  void initChip(uint8_t en);
  void commandTo(uint8_t value, uint8_t en);
  void send(uint8_t value, uint8_t rsBit, uint8_t en);
  void write4bits(uint8_t nibble, uint8_t en);
  void pulseEnable(uint8_t data, uint8_t en);
  void expanderWrite(uint8_t data);

  uint8_t _addr;
  uint8_t _cols;
  uint8_t _rows;
  uint8_t _chips;          // 1 or 2 HD44780 controllers
  uint8_t _chip;           // controller the cursor currently sits on
  uint8_t _backlightBit;   // P3 held high while the backlight is on
  uint8_t _displaycontrol; // display/cursor/blink flags, mirrored for both chips
};

#endif // LIQUIDCRYSTALI2C_DUAL_H
