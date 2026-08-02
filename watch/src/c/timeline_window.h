#pragma once

#include <pebble.h>

// Shows the delivery timeline for the parcel at `index` in the phone's list.
void timeline_window_push(int index, const char *nickname, const char *label,
                          const char *milestone);
