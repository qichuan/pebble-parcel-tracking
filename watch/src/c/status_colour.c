#include "status_colour.h"

GColor status_colour(const char *milestone) {
#if defined(PBL_COLOR)
  if (!milestone) {
    return COLOUR_DIM;
  }
  if (strcmp(milestone, "delivered") == 0) {
    return GColorJaegerGreen; // #00AA55
  }
  if (strcmp(milestone, "exception") == 0 ||
      strcmp(milestone, "failed_attempt") == 0) {
    // Not one of the design's six swatches, which have no "something went
    // wrong" colour — but red is the one signal worth spending a hue on, and
    // #FF0000 obeys the same 00/55/AA/FF channel rule as the rest.
    return GColorRed;
  }
  if (strcmp(milestone, "out_for_delivery") == 0 ||
      strcmp(milestone, "available_for_pickup") == 0) {
    return COLOUR_ACCENT; // #FFAA00 — the "today" colour
  }
  if (strcmp(milestone, "in_transit") == 0) {
    return GColorCobaltBlue; // #0055AA
  }
  return COLOUR_DIM; // pending, info_received, unknown
#else
  return GColorBlack;
#endif
}

bool status_colour_is_dark(const char *milestone) {
#if defined(PBL_COLOR)
  // Only the accent is light enough to take black type; everything else in the
  // status palette is a dark fill.
  if (!milestone) return true;
  return !(strcmp(milestone, "out_for_delivery") == 0 ||
           strcmp(milestone, "available_for_pickup") == 0);
#else
  return true; // the b/w fill is always solid black
#endif
}
