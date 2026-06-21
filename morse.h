#ifndef MORSE_H
#define MORSE_H

#include <Arduino.h>

// Time range of a dot in milliseconds. Anything longer than the max is a dash.
extern const unsigned int dotTimeMillisMin;
extern const unsigned int dotTimeMillisMax;

// Classify a single key-down duration.
bool isDot(unsigned long t);
bool isDash(unsigned long t);

// Returned by decodeMorse() when no pattern matches. 0xFF is the HD44780 solid
// block tile - the LCD's "unknown character" glyph, used in place of U+FFFD
// (the replacement character) which a character LCD has no glyph for. It must be
// sent to the display with lcd.write(), not lcd.print(), to bypass UTF-8 decoding.
#define MORSE_UNKNOWN ((char)0xFF)

// Decode a sequence of key-down durations into an ASCII character.
// `pressTimes` holds up to `count` durations; a 0 (or otherwise unclassifiable)
// entry ends the symbol. Returns MORSE_UNKNOWN if the pattern is not recognised.
char decodeMorse(const unsigned long *pressTimes, int count);

#endif // MORSE_H
