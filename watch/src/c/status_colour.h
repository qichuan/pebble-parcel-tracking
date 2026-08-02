#pragma once

#include <pebble.h>

// Colour for a Ship24 status milestone code ("in_transit", "delivered", ...).
// Returns a sensible neutral on unknown codes; callers on b/w platforms simply
// don't use it.
GColor status_colour(const char *milestone);
