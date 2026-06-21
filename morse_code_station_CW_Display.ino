/**
 * A Morse code station for the Arduino. Uses any board from the Nano to the Uno.
 * 
 * The display is a 128*64 OLED display and it uses the 
 * AdaFruit OLED driver libraries: 
 * - Adafruit GFX
 * - Adafruit SSD1306
 * 
 * You can search for these libraries and install them from within the Arduino
 * IDE. From the IDE menu choose Sketch->Include Library->Library Manager
 * enter the names of the libraries above and click install for each of them.
 * 
 * Author: Mario Gianota July 2020
 * Revised by: Owen Graham, KE0SBX, September 2023
 */
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
#define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)

#define SPEAKER_PIN 3 //PWM-enabled pin for tone generation
#define LED_PIN 13
#define CODE_BUTTON 2

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

bool codeButtonArmed;
bool codeButtonPressed;
unsigned long codeTime;
unsigned long startTime;
unsigned long lastButtonPressTime;
bool letterDecoded;
bool newWord;

unsigned long lastActivityTime;
const unsigned long inactivityTimeout = 10000; // 10 seconds in milliseconds

// Time range of a dot in milliseconds
const unsigned int dotTimeMillisMin = 20;
const unsigned int dotTimeMillisMax = 150;

// Array to store the times of the code button presses (up to 7 dots or dashes, 0-6)
unsigned long buttonPressTimes[10];
int bptIndex;

int row;
int col;

void setup() {
  
  pinMode(CODE_BUTTON, INPUT);
  pinMode(SPEAKER_PIN, OUTPUT); //Our speaker is still an output
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(SPEAKER_PIN, LOW); //Initialize the speaker to LOW
  Serial.begin(9600);
  // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // Address 0x3D for 128x64
    Serial.println("SSD1306 allocation failed");
    for(;;); // Don't proceed, loop forever
  }
  display.clearDisplay();
  display.setTextSize(1);      // Normal 1:1 pixel scale
  display.setTextColor(SSD1306_WHITE); // Draw white text
  display.setCursor(0, 0);     // Start at top-left corner
  display.cp437(true);         // Use full 256 char 'Code Page 437' font
  display.write("Morse Code Station");
  display.drawLine(0, 10, display.width(), 10, SSD1306_WHITE);

  display.setCursor(0, 12);
  display.display();
  delay(100);
}

void loop() {
  scanButtons();

  unsigned long elapsedTime = millis() - lastActivityTime;
  if (elapsedTime > inactivityTimeout) {
    // Clear the display and draw the header after 30 seconds of inactivity
    display.clearDisplay();
    drawHeader();
    display.display();
  } else {
    if (millis() - lastButtonPressTime > 1600 && newWord == true) {
      Serial.println("New word");
      display.write(' ');
      display.display();
      newWord = false;
    } else if (millis() - lastButtonPressTime > 600 && letterDecoded == false) {
      decodeButtonPresses();
      letterDecoded = true;
    }
  }
}

void drawHeader() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.cp437(true);
  display.write("Morse Code Station");
  display.drawLine(0, 10, display.width(), 10, SSD1306_WHITE);
  display.setCursor(0, 12);
}

void codeButtonDown() {
  tone(SPEAKER_PIN, 800); // You can adjust the "sidetone" frequency (e.g., 400 - 1000 Hz) as needed
  // It is set to 800Hz currently, but can be changed to match your preferred sidetone frequency
  digitalWrite(LED_PIN, HIGH);
  codeTime = millis() - startTime;
}

void codeButtonReleased() {
  noTone(SPEAKER_PIN); // Turn off the speaker
  digitalWrite(LED_PIN, LOW);

  // Most button bounces take less than 25 millis. If the code time
  // is greater than 25 millis then it was probably a legit button press
  if(codeTime > 25) {
    
    Serial.print("Code time: ");
    Serial.print(codeTime);
    Serial.println(" milliseconds");

    // Save codeTime
    buttonPressTimes[bptIndex] = codeTime;
    bptIndex++;
    if( bptIndex == 10 ) {
      
      for(int i=0; i<10; i++) {
        buttonPressTimes[bptIndex] = 0;
      }
    }
  }
}

void scanButtons() {
  if (!codeButtonArmed && digitalRead(CODE_BUTTON) == HIGH) {
    codeButtonArmed = true;
    // start timer
    startTime = millis();
    lastButtonPressTime = startTime;
    codeTime = 0;
    letterDecoded = false;
    newWord = true;
  }

  if (digitalRead(CODE_BUTTON) == HIGH) {
    codeButtonPressed = true;
    codeButtonDown();
    lastActivityTime = millis(); // Update the last activity time
  }

  if (codeButtonPressed && digitalRead(CODE_BUTTON) == LOW) {
    codeButtonPressed = false;
    codeButtonReleased();
    codeButtonArmed = false;
  }
}

void decodeButtonPresses() {
  Serial.print("DECODE LETTER: ");
  for(int i=0; i<bptIndex; i++) {
    if( isDot(buttonPressTimes[i]) ) 
      Serial.print(" DOT ");
    else if( isDash(buttonPressTimes[i]) )
      Serial.print(" DASH");
  }
  Serial.print("   ");
  String c = decodeMsg();
  Serial.print(c);
  Serial.println();
  display.write(c.c_str());
  display.display();
  bptIndex = 0;
  for(int i=0; i<10; i++) {
    buttonPressTimes[i] = 0;
  }
}

bool isDot(unsigned long t) {
  if(t >= dotTimeMillisMin && t <= dotTimeMillisMax)
    return true;
  return false;
}
bool isDash(unsigned long t) {
  if(t > dotTimeMillisMax)
    return true;
  return false;
}

void drawChar(String c) {
  display.write(c.c_str());
  display.display();
}

//Library of characters - Includes alphabet, numerical characters, and CW prosigns
String decodeMsg() {
  //Initial character or unknown character
  String c = "_";

  //Alphabetic Characters
  if( isDot(buttonPressTimes[0]) && isDash(buttonPressTimes[1]) && buttonPressTimes[2] == 0 )
    c = 'A';
  else if( isDash(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && isDot(buttonPressTimes[3]) && buttonPressTimes[4]== 0 )
    c = 'B';
  else if( isDash(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDash(buttonPressTimes[2]) && isDot(buttonPressTimes[3]) && buttonPressTimes[4]== 0 )
    c = 'C';
  else if( isDash(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && buttonPressTimes[3]== 0 )
    c = 'D';
  else if( isDot(buttonPressTimes[0]) && buttonPressTimes[1] == 0 )
    c = 'E';
  else if( isDot(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDash(buttonPressTimes[2]) && isDot(buttonPressTimes[3]) && buttonPressTimes[4]== 0 )
    c = 'F';
  else if( isDash(buttonPressTimes[0]) && isDash(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && buttonPressTimes[3]== 0 )
    c = 'G';
  else if( isDot(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && isDot(buttonPressTimes[3]) && buttonPressTimes[4]== 0 )
    c = 'H';
  else if( isDot(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && buttonPressTimes[2]== 0 )
    c = 'I';
  else if( isDot(buttonPressTimes[0]) && isDash(buttonPressTimes[1]) && isDash(buttonPressTimes[2]) && isDash(buttonPressTimes[3]) && buttonPressTimes[4]== 0 )
    c = 'J';
  else if( isDash(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDash(buttonPressTimes[2]) && buttonPressTimes[3]== 0 )
    c = 'K';
  else if( isDot(buttonPressTimes[0]) && isDash(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && isDot(buttonPressTimes[3]) && buttonPressTimes[4]== 0 )
    c = 'L';
  else if( isDash(buttonPressTimes[0]) && isDash(buttonPressTimes[1]) && buttonPressTimes[2]== 0 )
    c = 'M';
  else if( isDash(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && buttonPressTimes[2]== 0 )
    c = 'N';
  else if( isDash(buttonPressTimes[0]) && isDash(buttonPressTimes[1]) && isDash(buttonPressTimes[2]) && buttonPressTimes[3]== 0 )
    c = 'O';
  else if( isDot(buttonPressTimes[0]) && isDash(buttonPressTimes[1]) && isDash(buttonPressTimes[2]) && isDot(buttonPressTimes[3]) && buttonPressTimes[4]== 0 )
    c = 'P';
  else if( isDash(buttonPressTimes[0]) && isDash(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && isDash(buttonPressTimes[3]) && buttonPressTimes[4]== 0 )
    c = 'Q';
  else if( isDot(buttonPressTimes[0]) && isDash(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && buttonPressTimes[3]== 0 )
    c = 'R';
  else if( isDot(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && buttonPressTimes[3]== 0 )
    c = 'S';
  else if( isDash(buttonPressTimes[0]) && buttonPressTimes[1]== 0 )
    c = 'T';
  else if( isDot(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDash(buttonPressTimes[2]) && buttonPressTimes[3]== 0 )
    c = 'U';
  else if( isDot(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && isDash(buttonPressTimes[3]) && buttonPressTimes[4]== 0 )
    c = 'V';
  else if( isDot(buttonPressTimes[0]) && isDash(buttonPressTimes[1]) && isDash(buttonPressTimes[2]) && buttonPressTimes[3]== 0 )
    c = 'W';
  else if( isDash(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && isDash(buttonPressTimes[3]) && buttonPressTimes[4]== 0 )
    c = 'X';
  else if( isDash(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDash(buttonPressTimes[2]) && isDash(buttonPressTimes[3]) && buttonPressTimes[4]== 0 )
    c = 'Y';
  else if( isDash(buttonPressTimes[0]) && isDash(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && isDot(buttonPressTimes[3]) && buttonPressTimes[4]== 0 )
    c = 'Z';

  //Numerical characters
  else if( isDot(buttonPressTimes[0]) && isDash(buttonPressTimes[1]) && isDash(buttonPressTimes[2]) && isDash(buttonPressTimes[3]) && isDash(buttonPressTimes[4]) && buttonPressTimes[5] == 0)
    c = '1';
  else if( isDot(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDash(buttonPressTimes[2]) && isDash(buttonPressTimes[3]) && isDash(buttonPressTimes[4]) && buttonPressTimes[5] == 0)
    c = '2';
  else if( isDot(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && isDash(buttonPressTimes[3]) && isDash(buttonPressTimes[4]) && buttonPressTimes[5] == 0)
    c = '3';
  else if( isDot(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && isDot(buttonPressTimes[3]) && isDash(buttonPressTimes[4]) && buttonPressTimes[5] == 0)
    c = '4';
  else if( isDot(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && isDot(buttonPressTimes[3]) && isDot(buttonPressTimes[4]) && buttonPressTimes[5] == 0)
    c = '5';
  else if( isDash(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && isDot(buttonPressTimes[3]) && isDot(buttonPressTimes[4]) && buttonPressTimes[5] == 0)
    c = '6';
  else if( isDash(buttonPressTimes[0]) && isDash(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && isDot(buttonPressTimes[3]) && isDot(buttonPressTimes[4]) && buttonPressTimes[5] == 0)
    c = '7';
  else if( isDash(buttonPressTimes[0]) && isDash(buttonPressTimes[1]) && isDash(buttonPressTimes[2]) && isDot(buttonPressTimes[3]) && isDot(buttonPressTimes[4]) && buttonPressTimes[5] == 0)
    c = '8';
  else if( isDash(buttonPressTimes[0]) && isDash(buttonPressTimes[1]) && isDash(buttonPressTimes[2]) && isDash(buttonPressTimes[3]) && isDot(buttonPressTimes[4]) && buttonPressTimes[5] == 0)
    c = '9';
  else if( isDash(buttonPressTimes[0]) && isDash(buttonPressTimes[1]) && isDash(buttonPressTimes[2]) && isDash(buttonPressTimes[3]) && isDash(buttonPressTimes[4]) && buttonPressTimes[5] == 0)
    c = '0';

  //Prosign Characters - Used in Amateur CW conversations
  else if( isDot(buttonPressTimes[0]) && isDash(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && isDash(buttonPressTimes[3]) && isDot(buttonPressTimes[4]) && buttonPressTimes[5] == 0)
    c = "<AR>"; //End of message prosign
  else if( isDot(buttonPressTimes[0]) && isDash(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && isDot(buttonPressTimes[3]) && isDot(buttonPressTimes[4]) && buttonPressTimes[5] == 0)
    c = "<AS>"; //Stand by prosign
  else if( isDash(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && isDot(buttonPressTimes[3]) && isDash(buttonPressTimes[4]) && isDot(buttonPressTimes[5]) && isDash(buttonPressTimes[6]) && buttonPressTimes[7] == 0)
    c = "<BK>"; //Break message, invite receiving station to TX
  else if( isDash(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && isDot(buttonPressTimes[3]) && isDash(buttonPressTimes[4]) && buttonPressTimes[5] == 0)
    c = "<BT>"; //Pause, break for text
  else if( isDash(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDash(buttonPressTimes[2]) && isDot(buttonPressTimes[3]) && isDash(buttonPressTimes[4]) && buttonPressTimes[5] == 0)
    c = "<KA>"; //Beginning of message
  else if( isDash(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDash(buttonPressTimes[2]) && isDash(buttonPressTimes[3]) && isDot(buttonPressTimes[4]) && buttonPressTimes[5] == 0)
    c = "<KN>"; //End of transmission
  else if( isDash(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDash(buttonPressTimes[2]) && isDot(buttonPressTimes[3]) && isDot(buttonPressTimes[4]) && isDash(buttonPressTimes[5]) && isDot(buttonPressTimes[6]) && isDot(buttonPressTimes[7]) && buttonPressTimes[8] == 0)
    c = "<CL>"; //Frequency clear prosign
  else if( isDot(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && isDash(buttonPressTimes[3]) && isDot(buttonPressTimes[4]) && isDash(buttonPressTimes[5]) && buttonPressTimes[6] == 0)
    c = "<SK>"; //Silent Key, end of contact
  else if( isDot(buttonPressTimes[0]) && isDot(buttonPressTimes[1]) && isDot(buttonPressTimes[2]) && isDash(buttonPressTimes[3]) && isDot(buttonPressTimes[4]) && buttonPressTimes[5] == 0)
    c = "<VE>"; //Copy, 10-4, verified, etc.

  //Add some more Morse characters if you feel like it!
  return c;
}