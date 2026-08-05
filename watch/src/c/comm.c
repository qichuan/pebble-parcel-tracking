// AppMessage transport.
//
// Three things go wrong on a real watch, and each gets its own answer:
//
//   the outbox refuses the message   -> retry with a growing delay
//   the message goes out unanswered  -> re-ask with a growing delay
//   Bluetooth is simply gone         -> stop burning retries, wait, and replay
//                                       the moment it comes back
//
// The last one matters most. Without it the app spends its whole retry budget
// talking to nothing, reports a failure, and then sits there — the phone can
// reconnect a second later and the watch never notices.
#include <pebble.h>
#include "comm.h"

#define INBOX_SIZE 4096
#define OUTBOX_SIZE 128

#define MAX_SEND_ATTEMPTS 4
static const uint32_t SEND_BACKOFF_MS[MAX_SEND_ATTEMPTS] = { 200, 500, 1200, 2500 };

// The phone always answers straight away by design — from cache, or with an
// empty "still fetching" payload — so this window doesn't cover network
// latency. What it does have to cover is the phone-side JS booting, which on a
// cold launch is slow and varies wildly between handsets. Hence the long tail.
#define MAX_REPLY_ATTEMPTS 5
static const uint32_t REPLY_BACKOFF_MS[MAX_REPLY_ATTEMPTS] =
    { 4000, 5000, 7000, 9000, 12000 };

// The phone said "more is coming" and then went quiet. Ask once more rather
// than leaving the watch on a loading screen indefinitely.
#define PENDING_TIMEOUT_MS 25000

// After giving up entirely, keep trying quietly so a blip heals itself without
// the user having to press anything.
#define IDLE_RETRY_MS 20000

static CommDataHandler s_on_data;
static CommErrorHandler s_on_error;
static CommConnectionHandler s_on_connection;

static char s_request[16];
static int s_index;
static bool s_has_index;

static bool s_active;      // a request is outstanding
static bool s_mailbox_open;
static bool s_connected;
static bool s_rescued;     // already re-asked after an interim reply

static int s_send_attempts;
static int s_reply_attempts;

static AppTimer *s_send_timer;
static AppTimer *s_reply_timer;
static AppTimer *s_idle_timer;

static void prv_send(void *context);
static void prv_reply_timeout(void *context);

static void prv_stop(AppTimer **timer) {
  if (*timer) {
    app_timer_cancel(*timer);
    *timer = NULL;
  }
}

static void prv_arm_idle_retry(void) {
  prv_stop(&s_idle_timer);
  if (!s_request[0]) return;
  s_idle_timer = app_timer_register(IDLE_RETRY_MS, prv_send, NULL);
}

// Give up on this attempt and tell the window, but keep a slow retry running in
// the background — most of these failures are transient.
static void prv_fail(const char *message) {
  s_active = false;
  prv_stop(&s_send_timer);
  prv_stop(&s_reply_timer);
  prv_arm_idle_retry();
  if (s_on_error) {
    s_on_error(message);
  }
}

static void prv_retry_send(void) {
  if (s_send_attempts >= MAX_SEND_ATTEMPTS) {
    prv_fail("Can't reach phone");
    return;
  }
  uint32_t delay = SEND_BACKOFF_MS[s_send_attempts];
  s_send_attempts++;
  prv_stop(&s_send_timer);
  s_send_timer = app_timer_register(delay, prv_send, NULL);
  if (!s_send_timer) {
    prv_fail("Can't reach phone");
  }
}

static void prv_arm_reply_timer(void) {
  prv_stop(&s_reply_timer);
  int step = s_reply_attempts < MAX_REPLY_ATTEMPTS ? s_reply_attempts
                                                   : MAX_REPLY_ATTEMPTS - 1;
  s_reply_timer = app_timer_register(REPLY_BACKOFF_MS[step], prv_reply_timeout,
                                     NULL);
}

static void prv_send(void *context) {
  // Whichever timer woke us has now fired; don't cancel it later.
  s_send_timer = NULL;
  s_idle_timer = NULL;
  if (!s_request[0]) {
    return;
  }
  s_active = true;

  if (!s_mailbox_open) {
    prv_fail("Out of memory");
    return;
  }
  // No point spending retries on a radio that isn't there. Park the request;
  // the connection handler replays it the moment the phone is back.
  if (!s_connected) {
    prv_fail("Phone not connected");
    return;
  }

  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    prv_retry_send();
    return;
  }
  dict_write_cstring(iter, MESSAGE_KEY_REQUEST, s_request);
  if (s_has_index) {
    dict_write_int32(iter, MESSAGE_KEY_INDEX, s_index);
  }
  if (app_message_outbox_send() != APP_MSG_OK) {
    prv_retry_send();
    return;
  }
  prv_arm_reply_timer();
}

static void prv_reply_timeout(void *context) {
  s_reply_timer = NULL;
  if (!s_active) {
    return;
  }
  if (s_reply_attempts + 1 >= MAX_REPLY_ATTEMPTS) {
    prv_fail(s_connected ? "No reply from phone" : "Phone not connected");
    return;
  }
  s_reply_attempts++;
  s_send_attempts = 0;
  prv_send(NULL);
}

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason,
                              void *context) {
  // Only meaningful while we're still waiting on the message this refers to; a
  // late failure for an abandoned request must not restart anything.
  if (s_active) {
    prv_retry_send();
  }
}

static const char *prv_cstring(DictionaryIterator *iter, uint32_t key,
                               const char *fallback) {
  Tuple *tuple = dict_find(iter, key);
  return (tuple && tuple->type == TUPLE_CSTRING) ? tuple->value->cstring : fallback;
}

static void prv_inbox_received(DictionaryIterator *iter, void *context) {
  const char *type = prv_cstring(iter, MESSAGE_KEY_TYPE, NULL);
  if (!type) {
    return;
  }
  prv_stop(&s_send_timer);
  prv_stop(&s_reply_timer);
  prv_stop(&s_idle_timer);
  s_active = false;

  if (strcmp(type, "error") == 0) {
    prv_fail(prv_cstring(iter, MESSAGE_KEY_ERROR, "Unknown error"));
    return;
  }
  if (!s_on_data) {
    return;
  }
  // INDEX doubles as the "more is coming" flag on inbound messages: the phone
  // answers from cache first, then again once the network fetch lands.
  Tuple *pending = dict_find(iter, MESSAGE_KEY_INDEX);
  int more = pending ? (int)pending->value->int32 : 0;
  s_on_data(type, prv_cstring(iter, MESSAGE_KEY_PAYLOAD, ""), more);

  if (more && !s_rescued) {
    // Hold the door open once. If the promised follow-up never lands, ask
    // again rather than leaving the window loading forever.
    s_rescued = true;
    s_active = true;
    s_send_attempts = 0;
    s_reply_attempts = 0;
    prv_stop(&s_reply_timer);
    s_reply_timer = app_timer_register(PENDING_TIMEOUT_MS, prv_reply_timeout, NULL);
  }
}

static void prv_connection_changed(bool connected) {
  s_connected = connected;
  if (s_on_connection) {
    s_on_connection(connected);
  }
  if (!connected) {
    // Stop the clock: re-asking into a dead radio just spends the budget that
    // will be needed once the phone is back.
    prv_stop(&s_send_timer);
    prv_stop(&s_reply_timer);
    return;
  }
  // Back again — replay whatever we were last asking for, straight away.
  if (s_request[0]) {
    s_send_attempts = 0;
    s_reply_attempts = 0;
    prv_stop(&s_idle_timer);
    prv_send(NULL);
  }
}

void comm_set_handlers(CommDataHandler on_data, CommErrorHandler on_error) {
  s_on_data = on_data;
  s_on_error = on_error;
}

void comm_set_connection_handler(CommConnectionHandler handler) {
  s_on_connection = handler;
}

bool comm_is_connected(void) {
  return s_connected;
}

void comm_cancel(void) {
  s_active = false;
  s_request[0] = '\0';
  prv_stop(&s_send_timer);
  prv_stop(&s_reply_timer);
  prv_stop(&s_idle_timer);
}

static void prv_request(const char *request, bool has_index, int index) {
  strncpy(s_request, request, sizeof(s_request) - 1);
  s_request[sizeof(s_request) - 1] = '\0';
  s_has_index = has_index;
  s_index = index;
  s_send_attempts = 0;
  s_reply_attempts = 0;
  s_rescued = false;
  prv_stop(&s_idle_timer);
  prv_send(NULL);
}

void comm_request_list(bool force) {
  prv_request(force ? "refresh" : "list", false, 0);
}

void comm_request_detail(int index) {
  prv_request("detail", true, index);
}

void comm_init(void) {
  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_outbox_failed(prv_outbox_failed);

  // A failed open leaves every send failing for a reason the user can't act on,
  // so find out now rather than reporting it as "no reply from phone" later.
  s_mailbox_open = app_message_open(INBOX_SIZE, OUTBOX_SIZE) == APP_MSG_OK;
  if (!s_mailbox_open) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "app_message_open failed");
  }

  s_connected = connection_service_peek_pebble_app_connection();
  connection_service_subscribe((ConnectionHandlers) {
    .pebble_app_connection_handler = prv_connection_changed,
  });
}
