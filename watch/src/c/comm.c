#include <pebble.h>
#include "comm.h"

#define INBOX_SIZE 4096
#define OUTBOX_SIZE 128
#define MAX_SEND_ATTEMPTS 3
#define RETRY_DELAY_MS 500

// A send can succeed while the phone-side JS is still booting, in which case the
// message is simply dropped. Re-ask if nothing comes back. The phone always
// acknowledges straight away (from cache, or with an empty "still fetching"
// payload), so this window doesn't need to cover network latency.
#define RESPONSE_TIMEOUT_MS 5000
#define MAX_RESPONSE_ATTEMPTS 3

static CommDataHandler s_on_data;
static CommErrorHandler s_on_error;

static char s_pending_request[16];
static int s_pending_index;
static bool s_pending_has_index;
static int s_attempts;
static AppTimer *s_response_timer;
static int s_response_attempts;

static void prv_send(void *context);
static void prv_arm_response_timer(void);
static void prv_cancel_response_timer(void);

static void prv_fail(const char *message) {
  if (s_on_error) {
    s_on_error(message);
  }
}

static void prv_schedule_retry(void) {
  if (s_attempts < MAX_SEND_ATTEMPTS) {
    app_timer_register(RETRY_DELAY_MS, prv_send, NULL);
  } else {
    prv_cancel_response_timer(); // the request never left; don't re-ask later
    prv_fail("Can't reach phone");
  }
}

static void prv_send(void *context) {
  s_attempts++;
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    prv_schedule_retry();
    return;
  }
  dict_write_cstring(iter, MESSAGE_KEY_REQUEST, s_pending_request);
  if (s_pending_has_index) {
    dict_write_int32(iter, MESSAGE_KEY_INDEX, s_pending_index);
  }
  if (app_message_outbox_send() != APP_MSG_OK) {
    prv_schedule_retry();
  }
}

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason,
                              void *context) {
  prv_schedule_retry();
}

static void prv_response_timeout(void *context) {
  s_response_timer = NULL;
  if (s_response_attempts >= MAX_RESPONSE_ATTEMPTS) {
    prv_fail("No reply from phone");
    return;
  }
  s_response_attempts++;
  s_attempts = 0;
  prv_send(NULL);
  prv_arm_response_timer();
}

static void prv_arm_response_timer(void) {
  if (s_response_timer) {
    app_timer_cancel(s_response_timer);
  }
  s_response_timer = app_timer_register(RESPONSE_TIMEOUT_MS, prv_response_timeout,
                                        NULL);
}

static void prv_cancel_response_timer(void) {
  if (s_response_timer) {
    app_timer_cancel(s_response_timer);
    s_response_timer = NULL;
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
  prv_cancel_response_timer();
  if (strcmp(type, "error") == 0) {
    prv_fail(prv_cstring(iter, MESSAGE_KEY_ERROR, "Unknown error"));
    return;
  }
  if (s_on_data) {
    // INDEX doubles as the "more is coming" flag on inbound messages: the phone
    // answers from cache first, then again once the network fetch lands.
    Tuple *pending = dict_find(iter, MESSAGE_KEY_INDEX);
    s_on_data(type, prv_cstring(iter, MESSAGE_KEY_PAYLOAD, ""),
              pending ? (int)pending->value->int32 : 0);
  }
}

void comm_set_handlers(CommDataHandler on_data, CommErrorHandler on_error) {
  s_on_data = on_data;
  s_on_error = on_error;
}

static void prv_request(const char *request, bool has_index, int index) {
  strncpy(s_pending_request, request, sizeof(s_pending_request) - 1);
  s_pending_request[sizeof(s_pending_request) - 1] = '\0';
  s_pending_has_index = has_index;
  s_pending_index = index;
  s_attempts = 0;
  s_response_attempts = 0;
  prv_send(NULL);
  prv_arm_response_timer();
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
  app_message_open(INBOX_SIZE, OUTBOX_SIZE);
}
