#pragma once

#include <pebble.h>

// The design's palette, which lives entirely inside Pebble's 64-colour space —
// every channel is 00/55/AA/FF, so nothing dithers unexpectedly. On 1-bit
// platforms these all collapse to black or white and the layout carries the
// distinctions instead.
//
//   PAPER   #FFFFFF   background
//   INK     #000000   type, bars
//   ACCENT  #FFAA00   arriving today, selection
//   DIM     #555555   secondary type, quiet states
//   RULE    #AAAAAA   hairlines and connectors only, never type
#if defined(PBL_COLOR)
  #define COLOUR_PAPER  GColorWhite
  #define COLOUR_INK    GColorBlack
  #define COLOUR_ACCENT GColorChromeYellow  // #FFAA00
  #define COLOUR_DIM    GColorDarkGray      // #555555
  #define COLOUR_RULE   GColorLightGray     // #AAAAAA
#else
  #define COLOUR_PAPER  GColorWhite
  #define COLOUR_INK    GColorBlack
  #define COLOUR_ACCENT GColorBlack
  #define COLOUR_DIM    GColorBlack
  #define COLOUR_RULE   GColorBlack
#endif

// Colour for a Ship24 status milestone code ("in_transit", "delivered", ...).
// Returns a sensible neutral on unknown codes; callers on b/w platforms simply
// don't use it.
GColor status_colour(const char *milestone);

// True when the status colour is dark enough to need white type on top.
bool status_colour_is_dark(const char *milestone);
