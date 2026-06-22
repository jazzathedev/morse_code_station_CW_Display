#include "morse.h"

#include <avr/pgmspace.h>
#include <string.h>

// Time range of a dot in milliseconds.
const unsigned int dotTimeMillisMin = 40;
const unsigned int dotTimeMillisMax = 170;

bool isDot(unsigned long t) {
  return t >= dotTimeMillisMin && t <= dotTimeMillisMax;
}

bool isDash(unsigned long t) {
  return t > dotTimeMillisMax;
}

// Pattern -> character lookup. '.' = dot, '-' = dash. Stored in flash (PROGMEM)
// rather than RAM; the longest pattern is 7 symbols ('$' = ...-..-), so a fixed
// 8-byte field holds it plus the NUL terminator.
struct MorseCode {
  char pattern[MAX_PATTERN + 1];
  char letter;
};

static const MorseCode MORSE_TABLE[] PROGMEM = {
    {".-", 'A'},    {"-...", 'B'},  {"-.-.", 'C'},  {"-..", 'D'},
    {".", 'E'},     {"..-.", 'F'},  {"--.", 'G'},   {"....", 'H'},
    {"..", 'I'},    {".---", 'J'},  {"-.-", 'K'},   {".-..", 'L'},
    {"--", 'M'},    {"-.", 'N'},    {"---", 'O'},   {".--.", 'P'},
    {"--.-", 'Q'},  {".-.", 'R'},   {"...", 'S'},   {"-", 'T'},
    {"..-", 'U'},   {"...-", 'V'},  {".--", 'W'},   {"-..-", 'X'},
    {"-.--", 'Y'},  {"--..", 'Z'},  {".----", '1'}, {"..---", '2'},
    {"...--", '3'}, {"....-", '4'}, {".....", '5'}, {"-....", '6'},
    {"--...", '7'}, {"---..", '8'}, {"----.", '9'}, {"-----", '0'},
    // Punctuation (ITU)
    {".-.-.-", '.'},  {"--..--", ','},  {"..--..", '?'},  {".----.", '\''},
    {"-.-.--", '!'},  {"-..-.", '/'},   {"-.--.", '('},   {"-.--.-", ')'},
    {".-...", '&'},   {"---...", ':'},   {"-.-.-.", ';'},  {"-...-", '='},
    {".-.-.", '+'},   {"-....-", '-'},   {"..--.-", '_'},  {".-..-.", '"'},
    {"...-..-", '$'}, {".--.-.", '@'},
};

char symbolFor(unsigned long t) {
  if (isDot(t))
    return '.';
  if (isDash(t))
    return '-';
  return '\0';
}

char decodeMorsePattern(const char *pattern) {
  for (unsigned int i = 0; i < sizeof(MORSE_TABLE) / sizeof(MORSE_TABLE[0]);
       i++) {
    MorseCode entry; // copy this row out of flash before comparing
    memcpy_P(&entry, &MORSE_TABLE[i], sizeof(entry));
    if (strcmp(pattern, entry.pattern) == 0) {
      return entry.letter;
    }
  }
  return MORSE_UNKNOWN;
}

char decodeMorse(const unsigned long *pressTimes, int count) {
  // Build a "-.-." style pattern from the press durations. The first entry
  // that is neither a dot nor a dash (typically a 0) ends the symbol.
  char pattern[16];
  int n = 0;
  for (int i = 0; i < count && n < (int)sizeof(pattern) - 1; i++) {
    char sym = symbolFor(pressTimes[i]);
    if (sym == '\0')
      break;
    pattern[n++] = sym;
  }
  pattern[n] = '\0';

  return decodeMorsePattern(pattern);
}
