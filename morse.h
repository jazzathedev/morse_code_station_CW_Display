#ifndef MORSE_H
#define MORSE_H

#include <Arduino.h>

// Time range of a dot in milliseconds. Anything longer than the max is a dash.
extern const unsigned int dotTimeMillisMin;
extern const unsigned int dotTimeMillisMax;

// Classify a single key-down duration.
bool isDot(unsigned long t);
bool isDash(unsigned long t);

// Returned by decodeMorse() when no pattern matches. 0x2A is '*', a plain ASCII
// glyph every character LCD can render, used as a visible "unknown symbol" mark.
#define MORSE_UNKNOWN ((char)0x2A)

// Decode a sequence of key-down durations into an ASCII character.
// `pressTimes` holds up to `count` durations; a 0 (or otherwise unclassifiable)
// entry ends the symbol. Returns MORSE_UNKNOWN if the pattern is not recognised.
char decodeMorse(const unsigned long *pressTimes, int count);

#endif // MORSE_H
