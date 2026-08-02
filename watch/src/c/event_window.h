#pragma once

#include <pebble.h>

// Full text of one tracking event — descriptions routinely overflow a menu row.
void event_window_push(const char *when, const char *description,
                       const char *location, GColor accent);
