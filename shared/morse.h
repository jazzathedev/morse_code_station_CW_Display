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

// Longest morse pattern we decode is 7 symbols ('$' = ...-..-), so patterns need
// 7 chars plus a NUL terminator.
#define MAX_PATTERN 7

// Decode a sequence of key-down durations into an ASCII character.
// `pressTimes` holds up to `count` durations; a 0 (or otherwise unclassifiable)
// entry ends the symbol. Returns MORSE_UNKNOWN if the pattern is not recognised.
char decodeMorse(const unsigned long *pressTimes, int count);

// Decode a ".-" style pattern string into an ASCII character. Used by the
// wireless station, which buffers patterns so it can re-render them as either
// raw dots/dashes or text. Returns MORSE_UNKNOWN if not recognised.
char decodeMorsePattern(const char *pattern);

// Classify a single key-down duration as a symbol character ('.' for a dot,
// '-' for a dash) or '\0' if it is neither. Lets callers build a pattern string
// without duplicating the isDot()/isDash() thresholds.
char symbolFor(unsigned long t);

#endif // MORSE_H
