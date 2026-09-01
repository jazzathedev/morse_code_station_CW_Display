#include "LiquidCrystalI2C_Dual.h"

#include <Wire.h>

// --- PCF8574 backpack bit map ----------------------------------------------
// The near-universal "LCD1602 I2C" backpack wires the expander like this. The
// only departure here is P1: on a stock board it drives R/W, and on the 40x4
// build it is re-routed to the panel's E2 pin instead (R/W goes to GND). On a
// single-controller display P1 is simply never raised, which the panel reads as
// R/W = LOW = write, so the same code drives an unmodified backpack.
#define BP_RS 0x01 // P0 - register select (LOW: command, HIGH: data)
#define BP_E2 0x02 // P1 - enable for controller 2 (R/W on a stock backpack)
#define BP_E1 0x04 // P2 - enable for controller 1
#define BP_BL 0x08 // P3 - backlight transistor
                   // P4-P7 carry D4-D7 in the high nibble

// --- HD44780 instruction set ------------------------------------------------
#define LCD_CLEARDISPLAY 0x01
#define LCD_RETURNHOME 0x02
#define LCD_ENTRYMODESET 0x04
#define LCD_DISPLAYCONTROL 0x08
#define LCD_FUNCTIONSET 0x20
#define LCD_SETCGRAMADDR 0x40
#define LCD_SETDDRAMADDR 0x80

#define LCD_ENTRYLEFT 0x02 // cursor moves right, display does not shift
#define LCD_DISPLAYON 0x04 // display on, cursor off, blink off
#define LCD_2LINE 0x08     // each controller drives two lines
#define LCD_4BITMODE 0x00  // data arrives as two nibbles

// Longest instruction (clear/home) takes 1.52ms on a 270kHz part; the rest
// settle in 37us. With R/W grounded we cannot poll the busy flag, so both are
// waited out with a margin.
#define EXEC_SHORT_US 50
#define EXEC_LONG_MS 2

LiquidCrystalI2C_Dual::LiquidCrystalI2C_Dual(uint8_t addr, uint8_t cols,
                                             uint8_t rows, uint8_t chips)
    : _addr(addr), _cols(cols), _rows(rows), _chips(chips == 2 ? 2 : 1),
      _chip(0), _backlightBit(BP_BL), _displaycontrol(LCD_DISPLAYON) {}

uint8_t LiquidCrystalI2C_Dual::enableMask(uint8_t chip) const
{
  return (chip == 1 && _chips == 2) ? BP_E2 : BP_E1;
}

// --- Low-level expander access ----------------------------------------------

// Push one byte to the expander's output latch. The backlight bit rides along
// with every write, so it stays as set regardless of what else is going on.
void LiquidCrystalI2C_Dual::expanderWrite(uint8_t data)
{
  Wire.beginTransmission(_addr);
  Wire.write(data | _backlightBit);
  Wire.endTransmission();
}

// Strobe one controller's enable line, latching whatever is on the data bus.
// The HD44780 needs the enable high for 450ns and the data stable 10ns after it
// falls; a 100kHz I2C byte takes ~90us, so both are met by the bus itself.
void LiquidCrystalI2C_Dual::pulseEnable(uint8_t data, uint8_t en)
{
  expanderWrite(data | en);
  delayMicroseconds(1);
  expanderWrite(data & ~en);
  delayMicroseconds(EXEC_SHORT_US);
}

// Present one nibble (already positioned in the high four bits) and strobe it.
void LiquidCrystalI2C_Dual::write4bits(uint8_t nibble, uint8_t en)
{
  expanderWrite(nibble);
  pulseEnable(nibble, en);
}

// Send a full byte as two nibbles, high first. rsBit picks the register: 0 for
// an instruction, BP_RS for character data.
void LiquidCrystalI2C_Dual::send(uint8_t value, uint8_t rsBit, uint8_t en)
{
  write4bits((value & 0xF0) | rsBit, en);
  write4bits(((value << 4) & 0xF0) | rsBit, en);
}

void LiquidCrystalI2C_Dual::commandTo(uint8_t value, uint8_t en)
{
  send(value, 0, en);
}

void LiquidCrystalI2C_Dual::command(uint8_t value)
{
  commandTo(value, enableMask(_chip));
}

// --- Bring-up ----------------------------------------------------------------

// The datasheet's "initialisation by instruction" dance, which puts a controller
// into a known 4-bit state whether it powered up cleanly or not.
void LiquidCrystalI2C_Dual::initChip(uint8_t en)
{
  // Three 8-bit function-set nibbles force the controller into 8-bit mode from
  // any starting state, then a fourth switches it to 4-bit.
  write4bits(0x30, en);
  delayMicroseconds(4500);
  write4bits(0x30, en);
  delayMicroseconds(4500);
  write4bits(0x30, en);
  delayMicroseconds(150);
  write4bits(0x20, en);
  delayMicroseconds(150);

  commandTo(LCD_FUNCTIONSET | LCD_4BITMODE | LCD_2LINE, en);
  commandTo(LCD_DISPLAYCONTROL | _displaycontrol, en);
  commandTo(LCD_CLEARDISPLAY, en);
  delay(EXEC_LONG_MS);
  commandTo(LCD_ENTRYMODESET | LCD_ENTRYLEFT, en);
}

void LiquidCrystalI2C_Dual::begin()
{
  Wire.begin(); // harmless if the sketch has already brought the bus up

  // Park every output low (bar the backlight) so the enable lines start idle,
  // then give the panel the 40ms it wants after power-on before talking to it.
  expanderWrite(0);
  delay(50);

  for (uint8_t chip = 0; chip < _chips; chip++)
  {
    initChip(enableMask(chip));
  }

  _chip = 0;
  clear();
}

// --- Public API --------------------------------------------------------------

void LiquidCrystalI2C_Dual::clear()
{
  // Each controller owns its own DDRAM, so both have to be told.
  for (uint8_t chip = 0; chip < _chips; chip++)
  {
    commandTo(LCD_CLEARDISPLAY, enableMask(chip));
  }
  delay(EXEC_LONG_MS);
  _chip = 0;
}

void LiquidCrystalI2C_Dual::home()
{
  for (uint8_t chip = 0; chip < _chips; chip++)
  {
    commandTo(LCD_RETURNHOME, enableMask(chip));
  }
  delay(EXEC_LONG_MS);
  _chip = 0;
}

// Move the cursor, selecting the controller that owns the requested row. On a
// 40x4 panel each controller is a two-line display in its own right, so rows 2
// and 3 restart at its line 0 and line 1 addresses.
void LiquidCrystalI2C_Dual::setCursor(uint8_t col, uint8_t row)
{
  if (row >= _rows)
  {
    row = _rows - 1;
  }
  if (col >= _cols)
  {
    col = _cols - 1;
  }

  uint8_t offset;
  if (_chips == 2)
  {
    _chip = row >> 1; // rows 0-1 on chip 0, rows 2-3 on chip 1
    offset = (row & 1) ? 0x40 : 0x00;
  }
  else
  {
    _chip = 0;
    // Single controller: lines 0/1 live at 0x00/0x40, and a four-line module
    // wraps lines 2/3 onto the tail of each, one screen width along.
    offset = (row & 1) ? 0x40 : 0x00;
    if (row >= 2)
    {
      offset += _cols;
    }
  }

  command(LCD_SETDDRAMADDR | (offset + col));
}

size_t LiquidCrystalI2C_Dual::write(uint8_t value)
{
  send(value, BP_RS, enableMask(_chip));
  return 1;
}

void LiquidCrystalI2C_Dual::backlight()
{
  _backlightBit = BP_BL;
  expanderWrite(0);
}

void LiquidCrystalI2C_Dual::noBacklight()
{
  _backlightBit = 0;
  expanderWrite(0);
}

void LiquidCrystalI2C_Dual::display()
{
  _displaycontrol |= LCD_DISPLAYON;
  for (uint8_t chip = 0; chip < _chips; chip++)
  {
    commandTo(LCD_DISPLAYCONTROL | _displaycontrol, enableMask(chip));
  }
}

void LiquidCrystalI2C_Dual::noDisplay()
{
  _displaycontrol &= ~LCD_DISPLAYON;
  for (uint8_t chip = 0; chip < _chips; chip++)
  {
    commandTo(LCD_DISPLAYCONTROL | _displaycontrol, enableMask(chip));
  }
}

// Custom glyphs live in each controller's own CGRAM, so a 40x4 panel needs the
// bitmap loaded into both or it will only render on the top half.
void LiquidCrystalI2C_Dual::createChar(uint8_t location, const uint8_t charmap[])
{
  location &= 0x7; // only slots 0-7 exist
  for (uint8_t chip = 0; chip < _chips; chip++)
  {
    uint8_t en = enableMask(chip);
    commandTo(LCD_SETCGRAMADDR | (location << 3), en);
    for (uint8_t i = 0; i < 8; i++)
    {
      send(charmap[i], BP_RS, en);
    }
  }
  // CGRAM writes leave the address counter in character memory; put it back.
  setCursor(0, 0);
}
