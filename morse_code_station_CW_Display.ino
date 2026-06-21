/**
 * A Morse code station for the Arduino.
 *
 * The display is a 20 x 4 or 16 x 2 LCD display and it uses the
 * LiquidCrystal.h LCD librarie:
 * or LCDI2C_Multilingual.h for 12c interface
 * Original Author: Mario Gianota July 2020 using a OLED screen.
 * https://hackaday.io/project/175129-arduino-morse-code-station
 * And improved code https://github.com/grahowe/Morse-Code-Station
 *
 * 2024-10-00 LED code changes VK6TU Oct 2024
 * 2024-12-00 Row Display Code - VK6XM Dec 2024
 * 2025-03-26 CW Pause & Clear Display Button - VK6XM_VK6TU
 * 2025-04-28 Fixed timing issue, morse key didn't work after reset. Change
 * BANNER display time, now uses "BANNER_DISPLAY_TIME" constant - VK6XM
 * 2025-05-15 v1.6 Added 0 timeout option to turn off this code. Also, reset LED
 * to OFF when resetting. - VK6XM
 */

#include <SPI.h>
#include <Wire.h>

#include <LCDI2C_Generic.h>
LCDI2C_Generic lcd(0x27, 20, 4); // I2C address: 0x27; Display size: 20x4

#include "morse.h" // dot/dash classification + decodeMorse() lookup

// #include <LiquidCrystal.h>
//  Pin assignments -->  (RS,RW, E, D4, D5, D6, D7)
//  LiquidCrystal lcd(2,255, 3, 4, 5, 6, 7);

#define VER 1
#define SUBVER 6

// Stringize VER/SUBVER so the banner can show "v1.6" without hardcoding it.
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define VERSION_STR "v" STR(VER) "." STR(SUBVER)

#define BUZZER_PIN 2
#define LED_PIN 4
#define CODE_BUTTON 3
#define CLEAR_BUTTON 5 // 26Mar2025 Change

// Max symbols (dots/dashes) captured per character. Must cover the longest code
// we decode: 7 for '$' (...-..-). Most letters/digits are <= 5, punctuation 6-7.
#define MAX_BUTTON_PRESS_TIMES 8

// How many of those symbols the live dot/dash indicator shows. Bounded by the
// columns available to its right (dotDashActivityX .. last column).
#define DOTDASH_DISPLAY_CELLS 6

// define INACTIVITY_TIME 600000 //10 minutes, 600 seconds, 600k milliseconds
// #define INACTIVITY_TIME 60000 //1 minute, 60 seconds, 60k milliseconds
// #define INACTIVITY_TIME 0 //if  0 - function is disabled, won't autoreboot.
#define BANNER_DISPLAY_TIME 3000 // 3 seconds

unsigned long lastActivityTime;
// const unsigned long inactivityTimeout = 10000; // 10 seconds in milliseconds
// //v1.5 change const unsigned long inactivityTimeout = INACTIVITY_TIME; // 600
// seconds in milliseconds  //v1.5 change   //v1.6 DISABLED

/** VK6XM Additional code - row scrolling **/
// VK6XM - RowCopy code
//  initially there is no data, but define arrays to hold data.
#define BLANKROW "                    ";
char blankrow[] = BLANKROW;
char row1[] = BLANKROW;
char row2[] = BLANKROW;
char row3[] = BLANKROW;

int DisplayPos = 0;
int MaxColumns = 20;

// CW Display positions on screen...
const unsigned int dotDashActivityX = 14; //(X pos --> 15)
const unsigned int dotDashActivityY = 0;  //(Y pos (ie Row is 1) --> 0)
/**-----------------------------------**/

bool codeButtonArmed;
bool codeButtonPressed;
unsigned long codeTime;
unsigned long startTime;
unsigned long lastButtonPressTime;
bool letterDecoded;
bool newWord;
// bool initialChar = true;
bool initialChar = false;

// Array to store the times of the code button presses
unsigned long buttonPressTimes[MAX_BUTTON_PRESS_TIMES];
int bptIndex;
bool keyStroke = false;

// dot/dash timing thresholds (dotTimeMillisMin/Max) live in morse.cpp

// Reset Nano Function
void (*resetFunc)(void) = 0;

void showDotDashActivity() {

  lcd.setCursor((dotDashActivityX), dotDashActivityY);
  // lcd.print("CW");
  for (int i = 0; i < DOTDASH_DISPLAY_CELLS; i++) {
    if (isDot(buttonPressTimes[i])) {
      lcd.print(".");
    } else if (isDash((buttonPressTimes[i]))) {
      lcd.print("-");
    } else { // not a dot or dash... not defined yet, make it a space/unfilled
             // yet
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
  welcomeBanner(
      BANNER_DISPLAY_TIME); // show banner for BANNER_DISPLAY_TIME/1000 seconds
                            // - 3 seconds

  lcd.setCursor(0, 0);
  lcd.print("Morse Code:"); // 20 x 4
  lcd.setCursor(0, 1);

  strcpy(row1, blankrow);
  strcpy(row2, blankrow);
  strcpy(row3, blankrow);
  newWord = false;
  letterDecoded = true;
  DisplayPos = 0; // reset column where printing will start
  keyStroke = false;
  initialChar = false;
  codeButtonPressed = false;
  codeButtonArmed = false;
  resetButtonPressTimes();
  digitalWrite(CLEAR_BUTTON, HIGH); // 26Mar2025  - set clear button to HIGH
  digitalWrite(LED_PIN, LOW);
  lastActivityTime = millis();
  codeTime = 0;
}

void setup() {

  // Serial.begin(9600); // uncomment to enable serial debug output

  pinMode(CODE_BUTTON, INPUT);
  pinMode(CLEAR_BUTTON, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  lcd.init(); // initialize the lcd
  lcd.backlight();

  // testLines();
  resetSystem();
}

// Write a row buffer to the LCD verbatim, one byte per column. Bypasses the
// library's UTF-8 decoding so raw ROM codes (e.g. the 0xFF block) render as-is.
void writeRow(const char *s) {
  while (*s)
    lcd.write((uint8_t)*s++);
}

void displayChar(char DisplayChar) {
  // we will display on the last row, and scroll up row 3-> 2, 2->1
  // when we scroll up, we will blank the bottom row.

  if (initialChar) {     // needed to stop initial char showing up
    initialChar = false; // reset flag
    return;              // return - we're not showing this character
  }

  lcd.setCursor(DisplayPos, 3);     // put the cursor in the right spot on Row 4
  lcd.write((uint8_t)DisplayChar);  // display the single character (raw, so the
                                    // 0xFF error block renders correctly)

  row3[DisplayPos] = DisplayChar; // record the character in it's position
  DisplayPos++;                   // move the position across one.

  if (DisplayPos >= MaxColumns) { // if we've reached the end of the line, we
                                  // need to scroll...

    Serial.println("MaxColumns reached, scrolling display.");

    // we have reached the end of the line.
    // we need to move the data up, and blank the bottom line.

    // reset DisplayPos
    DisplayPos = 0;

    // copy rows, reset row3, redisplay
    strcpy(row1, row2);
    strcpy(row2, row3);
    strcpy(row3, blankrow);

    lcd.setCursor(0, 1);
    writeRow(row1); // display row1 (has contents of row2)

    lcd.setCursor(0, 2);
    writeRow(row2); // display row2 (has contents of row3)

    lcd.setCursor(0, 3);
    writeRow(row3); // display a blank line

    lcd.setCursor(0, 3); // put the cursor back at the beginning of row 4
  }
}

void resetButtonPressTimes() {
  // set all times to 0
  for (int i = 0; i < MAX_BUTTON_PRESS_TIMES; i++) {
    buttonPressTimes[i] = 0; // set back to ZERO - ie no time recorded
  }
  // showDotDashActivity ();  //clear the CW screen area
}

void resetTimeOut() {
  // Clear the display and draw the header after NN seconds of inactivity
  // we;re pretending to start again, with a new splashscreen and clear all old
  // variables.
  Serial.println("RESET - Screen Cleared");

  resetSystem();

  // resetSystem();
  lastActivityTime =
      millis();      // don't reset again, until inactvityTimeout comes up again
  keyStroke = false; // reset this
}

/* ***********************************************
MAIN LOOP HERE
************************************************ */
void loop() {

  unsigned long elapsedTime = millis() - lastActivityTime;
  // if ((elapsedTime > inactivityTimeout) && keyStroke) {
  // if (elapsedTime > inactivityTimeout) {   *DISABLED in V1.6*
  //     // nothing has happened for a long time, reset everything.
  //     resetSystem();
  // }   *DISABLED in V1.6*

  scanButtons();

  if (millis() - lastButtonPressTime > 1600 && newWord == true) {
    Serial.println("New word");

    // lcd.print(' ');
    displayChar(' '); // VK6XM Change
    newWord = false;
  } else if (millis() - lastButtonPressTime > 600 && letterDecoded == false) {
    decodeButtonPresses();
    letterDecoded = true;
    codeButtonArmed = false;
    // delay (5); //  allow button to fully be depressed, perior
  }
}
/* ***********************************************
END: MAIN LOOP
************************************************ */

void codeButtonDown() {
  tone(BUZZER_PIN, 440, 60);
  digitalWrite(LED_PIN, HIGH);
  codeTime = millis() - startTime;
  // lastActivityTime = millis();   //timeout function
  // keyStroke = false;  //button was pressed, activity started.
}

void codeButtonReleased() {
  digitalWrite(LED_PIN, LOW);

  // Most button bounces take less than 25 millis. If the code time
  // is greater than 25 millis then it was probably a legit button press
  if (codeTime > 25) {
    lastActivityTime = millis(); // timeout function
    keyStroke = false;           // button was pressed, activity started.

    // Serial.print("Code time: ");
    // Serial.print(codeTime);
    // Serial.println(" milliseconds");

    // Save codeTime
    buttonPressTimes[bptIndex] = codeTime;
    bptIndex++;

    showDotDashActivity(); // iterate through button presses, evaluate and
                           // display as buttons are being used.

    // if the dotDash buffer is full, then everything needs to be reset to ZERO
    // time.
    //  AND the dotDash Display needs to be reset as well.
    if (bptIndex >= MAX_BUTTON_PRESS_TIMES) {
      resetButtonPressTimes();
      bptIndex = 0; // also reset the index, else the next press writes OOB
      // clearDotDashActivity();  //clear up the Top Right display
    }
  }
}

void scanButtons() {

  // Check for RESET Button press:
  // 26Mar2025 Change - read CLEAR Button
  if (digitalRead(CLEAR_BUTTON) == LOW) {
    resetSystem(); // restart the program, clear screen, start again without a
                   // full power off/on
  }

  if (!codeButtonArmed && digitalRead(CODE_BUTTON) == HIGH) {
    codeButtonArmed = true;
    // start timer
    startTime = millis();
    lastButtonPressTime = startTime;
    codeTime = 0;
    letterDecoded = false;
    newWord = true;
  } else if (digitalRead(CODE_BUTTON) == HIGH) {
    codeButtonPressed = true;
    // codeButtonDown();
    tone(BUZZER_PIN, 440, 60);
    digitalWrite(LED_PIN, HIGH);
    codeTime = millis() - startTime;
    keyStroke = true;
  }
  if (codeButtonPressed && digitalRead(CODE_BUTTON) == LOW) {
    codeButtonPressed = false;
    keyStroke = true;
    codeButtonReleased();
    codeButtonArmed = false;
  }
  delay(10);
}

void decodeButtonPresses() {

  /* DEBUG TO SERIAL */
  Serial.print("DECODE LETTER: "); // DEBUG
  for (int i = 0; i < bptIndex; i++) {
    if (isDot(buttonPressTimes[i]))
      Serial.print(" DOT ");
    else if (isDash(buttonPressTimes[i]))
      Serial.print(" DASH");
  }
  Serial.print("   ");
  /* DEBUG TO SERIAL */

  char c = decodeMorse(buttonPressTimes, bptIndex);
  displayChar(
      c); // vk6XM change -put the character on screen, and scroll if required.
  Serial.print(c);  // DEBUG
  Serial.println(); // DEBUG

  bptIndex = 0;            // reset button press index to start
  resetButtonPressTimes(); // clear all the times recorded
  // clearDotDashActivity();  //clear up the Top Right display

  keyStroke = true;
}
