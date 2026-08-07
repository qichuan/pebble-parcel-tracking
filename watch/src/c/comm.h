#pragma once

#include <pebble.h>

// Called with the reply type ("parcels" / "events"), its packed payload, and a
// flag: 1 while the phone is still fetching fresher data, 0 when this is final.
typedef void (*CommDataHandler)(const char *type, const char *payload, int pending);
typedef void (*CommErrorHandler)(const char *message);

// Bluetooth came or went. The list uses this to say so at once instead of
// waiting for a request to time out first.
typedef void (*CommConnectionHandler)(bool connected);

void comm_init(void);

// The active window registers itself here so replies land in the right place.
void comm_set_handlers(CommDataHandler on_data, CommErrorHandler on_error);
void comm_set_connection_handler(CommConnectionHandler handler);

// Parcel summaries. `force` skips the phone-side cache TTL.
void comm_request_list(bool force);

// Timeline events for the parcel at `index` in the last "parcels" payload.
void comm_request_detail(int index);

// Abandon whatever is outstanding. A window calls this on its way out so a
// reply it no longer wants can't surface as an error on the window behind it.
void comm_cancel(void);

// Whether the phone is reachable right now.
bool comm_is_connected(void);
