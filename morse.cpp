#include "morse.h"

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

// Pattern -> character lookup. '.' = dot, '-' = dash.
struct MorseCode {
  const char *pattern;
  char letter;
};

static const MorseCode MORSE_TABLE[] = {
    {".-", 'A'},    {"-...", 'B'},  {"-.-.", 'C'},  {"-..", 'D'},
    {".", 'E'},     {"..-.", 'F'},  {"--.", 'G'},   {"....", 'H'},
    {"..", 'I'},    {".---", 'J'},  {"-.-", 'K'},   {".-..", 'L'},
    {"--", 'M'},    {"-.", 'N'},    {"---", 'O'},   {".--.", 'P'},
    {"--.-", 'Q'},  {".-.", 'R'},   {"...", 'S'},   {"-", 'T'},
    {"..-", 'U'},   {"...-", 'V'},  {".--", 'W'},   {"-..-", 'X'},
    {"-.--", 'Y'},  {"--..", 'Z'},  {".----", '1'}, {"..---", '2'},
    {"...--", '3'}, {"....-", '4'}, {".....", '5'}, {"-....", '6'},
    {"--...", '7'}, {"---..", '8'}, {"----.", '9'}, {"-----", '0'},
};

char decodeMorse(const unsigned long *pressTimes, int count) {
  // Build a "-.-." style pattern from the press durations. The first entry
  // that is neither a dot nor a dash (typically a 0) ends the symbol.
  char pattern[16];
  int n = 0;
  for (int i = 0; i < count && n < (int)sizeof(pattern) - 1; i++) {
    if (isDot(pressTimes[i])) {
      pattern[n++] = '.';
    } else if (isDash(pressTimes[i])) {
      pattern[n++] = '-';
    } else {
      break;
    }
  }
  pattern[n] = '\0';

  for (unsigned int i = 0; i < sizeof(MORSE_TABLE) / sizeof(MORSE_TABLE[0]);
       i++) {
    if (strcmp(pattern, MORSE_TABLE[i].pattern) == 0) {
      return MORSE_TABLE[i].letter;
    }
  }
  return '?';
}
