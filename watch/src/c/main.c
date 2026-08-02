// Parcel list — the app's root window. Rows come from the phone as a packed
// "parcels" payload; everything here is presentation only.
#include <pebble.h>
#include "comm.h"
#include "status_colour.h"
#include "timeline_window.h"

#define US_CHAR '\x1f'
#define PARCEL_MAX 12
#define HEADER_H 18
#define BAR_W 4 // status colour bar down the left edge of each row

typedef enum { STATE_LOADING, STATE_LOADED, STATE_ERROR } State;

typedef struct {
  char nickname[40];
  char milestone[24]; // Ship24 status code, drives the row colour
  char label[36];     // human-readable status, or why the fetch failed
  char ago[16];       // "2h ago" / "" when never fetched
} ParcelRow;

static Window *s_window;
static MenuLayer *s_menu_layer;
static ParcelRow s_rows[PARCEL_MAX];
static int s_count;
static State s_state;
static bool s_updating;
static char s_status[64];

// --- parsing -------------------------------------------------------------

static void prv_copy_field(char *dst, const char *src, size_t size) {
  strncpy(dst, src, size - 1);
  dst[size - 1] = '\0';
}

// Parses "nickname<US>milestone<US>label<US>ago\n..." one line at a time, so the
// whole payload is never copied into a second buffer (aplite has 24 KB of heap).
static void prv_parse_parcels(const char *payload) {
  s_count = 0;
  const char *p = payload;
  char line[160];

  while (*p && s_count < PARCEL_MAX) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = '\0';
    p = nl ? nl + 1 : p + strlen(p);
    if (line[0] == '\0') continue;

    ParcelRow *row = &s_rows[s_count];
    char *u1 = strchr(line, US_CHAR);
    char *u2 = u1 ? strchr(u1 + 1, US_CHAR) : NULL;
    char *u3 = u2 ? strchr(u2 + 1, US_CHAR) : NULL;
    if (u1) *u1 = '\0';
    if (u2) *u2 = '\0';
    if (u3) *u3 = '\0';
    prv_copy_field(row->nickname, line, sizeof(row->nickname));
    prv_copy_field(row->milestone, u1 ? u1 + 1 : "", sizeof(row->milestone));
    prv_copy_field(row->label, u2 ? u2 + 1 : "", sizeof(row->label));
    prv_copy_field(row->ago, u3 ? u3 + 1 : "", sizeof(row->ago));
    s_count++;
  }
}

// --- rendering -----------------------------------------------------------

static void prv_reload(void) {
  if (s_menu_layer) {
    menu_layer_reload_data(s_menu_layer);
  }
}

static uint16_t prv_get_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  if (s_state == STATE_LOADED) {
    return s_count > 0 ? s_count : 1;
  }
  return 1;
}

static int16_t prv_header_height(MenuLayer *ml, uint16_t section, void *ctx) {
  return HEADER_H;
}

static void prv_draw_header(GContext *ctx, const Layer *cell, uint16_t section,
                            void *context) {
  char title[32];
  if (s_updating) {
    strncpy(title, "Updating…", sizeof(title));
  } else if (s_state == STATE_LOADED && s_count > 0) {
    snprintf(title, sizeof(title), "%d parcel%s", s_count, s_count == 1 ? "" : "s");
  } else {
    strncpy(title, "Parcels", sizeof(title));
  }
  title[sizeof(title) - 1] = '\0';
#if defined(PBL_ROUND)
  // The stock header is left-aligned, which the circular mask clips away.
  GRect bounds = layer_get_bounds(cell);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(bounds.origin.x, bounds.origin.y - 3, bounds.size.w,
                           bounds.size.h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
#else
  menu_cell_basic_header_draw(ctx, cell, title);
#endif
}

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx,
                         void *context) {
  if (s_state == STATE_LOADING) {
    menu_cell_basic_draw(ctx, cell, "Loading…", NULL, NULL);
    return;
  }
  if (s_state == STATE_ERROR) {
    menu_cell_basic_draw(ctx, cell, s_status, "Select to retry", NULL);
    return;
  }
  if (s_count == 0) {
    menu_cell_basic_draw(ctx, cell, "No parcels",
                         "Add them in the phone app settings", NULL);
    return;
  }

  ParcelRow *row = &s_rows[idx->row];
  char sub[64];
  if (row->ago[0]) {
    snprintf(sub, sizeof(sub), "%s · %s", row->label, row->ago);
  } else {
    prv_copy_field(sub, row->label, sizeof(sub));
  }

#if defined(PBL_COLOR) && defined(PBL_RECT)
  // A colour bar in the left gutter reads the status at a glance without
  // stealing room from the text. Round screens have no straight edge to use.
  GRect bounds = layer_get_bounds(cell);
  graphics_context_set_fill_color(ctx, status_colour(row->milestone));
  graphics_fill_rect(ctx, GRect(0, 0, BAR_W, bounds.size.h), 0, GCornerNone);
#endif

  menu_cell_basic_draw(ctx, cell, row->nickname, sub, NULL);
}

// --- interaction ---------------------------------------------------------

static void prv_on_data(const char *type, const char *payload, int pending);
static void prv_on_error(const char *message);

static void prv_start_request(bool force) {
  comm_set_handlers(prv_on_data, prv_on_error);
  s_updating = true;
  comm_request_list(force);
  prv_reload();
}

static void prv_select_click(MenuLayer *ml, MenuIndex *idx, void *context) {
  if (s_state == STATE_ERROR) {
    s_state = STATE_LOADING;
    prv_start_request(false);
    return;
  }
  if (s_state != STATE_LOADED || s_count == 0) {
    return;
  }
  ParcelRow *row = &s_rows[idx->row];
  timeline_window_push(idx->row, row->nickname, row->label, row->milestone);
}

static void prv_select_long_click(MenuLayer *ml, MenuIndex *idx, void *context) {
  if (s_updating) {
    return;
  }
  vibes_short_pulse();
  prv_start_request(true);
}

// --- comm handlers -------------------------------------------------------

static void prv_on_data(const char *type, const char *payload, int pending) {
  if (strcmp(type, "parcels") != 0) {
    return;
  }
  prv_parse_parcels(payload);
  s_updating = pending != 0;
  // Nothing yet but more is coming: an interim ack, not an empty parcel list.
  s_state = (s_count == 0 && s_updating) ? STATE_LOADING : STATE_LOADED;
  prv_reload();
}

static void prv_on_error(const char *message) {
  s_updating = false;
  // A failed background refresh shouldn't wipe a list we already have.
  if (s_state == STATE_LOADED && s_count > 0) {
    prv_reload();
    return;
  }
  s_state = STATE_ERROR;
  prv_copy_field(s_status, message, sizeof(s_status));
  prv_reload();
}

// --- window --------------------------------------------------------------

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_rows = prv_get_num_rows,
    .get_header_height = prv_header_height,
    .draw_header = prv_draw_header,
    .draw_row = prv_draw_row,
    .select_click = prv_select_click,
    .select_long_click = prv_select_long_click,
  });
#if defined(PBL_ROUND)
  menu_layer_set_center_focused(s_menu_layer, true);
#endif
#if defined(PBL_COLOR)
  menu_layer_set_highlight_colors(s_menu_layer, GColorOrange, GColorBlack);
#endif
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_menu_layer));
}

static void prv_window_unload(Window *window) {
  menu_layer_destroy(s_menu_layer);
  s_menu_layer = NULL;
}

// The timeline window takes over the comm handlers while it is open; take them
// back (and pick up any status change) when the list is shown again.
static void prv_window_appear(Window *window) {
  comm_set_handlers(prv_on_data, prv_on_error);
  prv_reload();
}

static void prv_init(void) {
  comm_init();
  s_state = STATE_LOADING;

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
    .appear = prv_window_appear,
  });
  window_stack_push(s_window, true);

  prv_start_request(false);
}

static void prv_deinit(void) {
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
