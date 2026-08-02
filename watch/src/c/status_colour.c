#include "status_colour.h"

GColor status_colour(const char *milestone) {
#if defined(PBL_COLOR)
  if (!milestone) {
    return GColorDarkGray;
  }
  if (strcmp(milestone, "delivered") == 0) {
    return GColorIslamicGreen;
  }
  if (strcmp(milestone, "exception") == 0 ||
      strcmp(milestone, "failed_attempt") == 0) {
    return GColorRed;
  }
  if (strcmp(milestone, "out_for_delivery") == 0) {
    return GColorOrange;
  }
  if (strcmp(milestone, "available_for_pickup") == 0) {
    return GColorChromeYellow;
  }
  if (strcmp(milestone, "in_transit") == 0) {
    return GColorCobaltBlue;
  }
  return GColorDarkGray; // pending, info_received, unknown
#else
  return GColorBlack;
#endif
}
