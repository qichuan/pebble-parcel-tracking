#pragma once

#include <pebble.h>

// Shows one parcel as a single scrolling page: status headline, the three facts
// worth a scroll, then the full carrier history newest-first. `index` is the
// parcel's position in the phone's list, `total` how many there are (the header
// shows "2/4"), and `nickname` labels the loading state until the phone answers.
void timeline_window_push(int index, int total, const char *nickname);
