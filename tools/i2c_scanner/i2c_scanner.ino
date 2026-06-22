/**
 * Minimal I2C bus scanner. Flash this to a unit, open the Serial Monitor at
 * 9600 baud, and it prints the address of every I2C device it finds (SDA=A4,
 * SCL=A5 on the Nano). Use the reported address as LCD_I2C_ADDR in the station
 * sketch - it is commonly 0x27 or 0x3F.
 *
 * If nothing is found, the LCD isn't reachable on the bus at all: check SDA/SCL
 * wiring (not swapped), the joints, and 5V/GND.
 */

#include <Wire.h>

void setup() {
  Serial.begin(9600);
  while (!Serial) {
    ; // wait for the Serial Monitor on boards that need it
  }
  Wire.begin();
  Serial.println("I2C scanner ready.");
}

void loop() {
  int found = 0;

  Serial.println("Scanning...");
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) { // 0 = device acknowledged
      Serial.print("  device at 0x");
      if (addr < 16) {
        Serial.print("0");
      }
      Serial.println(addr, HEX);
      found++;
    }
  }

  if (found == 0) {
    Serial.println("  none found - check SDA/SCL wiring and joints");
  }
  Serial.println();
  delay(2000);
}
