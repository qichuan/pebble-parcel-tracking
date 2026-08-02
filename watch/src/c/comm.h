#pragma once

#include <pebble.h>

// Called with the reply type ("parcels" / "events"), its packed payload, and a
// flag: 1 while the phone is still fetching fresher data, 0 when this is final.
typedef void (*CommDataHandler)(const char *type, const char *payload, int pending);
typedef void (*CommErrorHandler)(const char *message);

void comm_init(void);

// The active window registers itself here so replies land in the right place.
void comm_set_handlers(CommDataHandler on_data, CommErrorHandler on_error);

// Parcel summaries. `force` skips the phone-side cache TTL.
void comm_request_list(bool force);

// Timeline events for the parcel at `index` in the last "parcels" payload.
void comm_request_detail(int index);
