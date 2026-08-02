#include "timeline_window.h"
#include "comm.h"
#include "event_window.h"
#include "status_colour.h"

#define US_CHAR '\x1f'
#define EVENT_MAX 12
#define HEADER_H 34 // two lines: parcel nickname over its current status

typedef enum { STATE_LOADING, STATE_LOADED, STATE_ERROR } State;

typedef struct {
  char when[20];         // "2h ago"
  char description[164];
  char location[44];
} EventRow;

static Window *s_window;
static MenuLayer *s_menu_layer;
static EventRow s_rows[EVENT_MAX];
static int s_count;
static State s_state;
static char s_status[64];

static char s_nickname[40];
static char s_label[36];
static char s_milestone[24];

static void prv_copy_field(char *dst, const char *src, size_t size) {
  strncpy(dst, src, size - 1);
  dst[size - 1] = '\0';
}

// --- parsing -------------------------------------------------------------

// "when<US>description<US>location\n...", newest first (the phone sorts).
static void prv_parse_events(const char *payload) {
  s_count = 0;
  const char *p = payload;
  char line[256];

  while (*p && s_count < EVENT_MAX) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = '\0';
    p = nl ? nl + 1 : p + strlen(p);
    if (line[0] == '\0') continue;

    EventRow *row = &s_rows[s_count];
    char *u1 = strchr(line, US_CHAR);
    char *u2 = u1 ? strchr(u1 + 1, US_CHAR) : NULL;
    if (u1) *u1 = '\0';
    if (u2) *u2 = '\0';
    prv_copy_field(row->when, line, sizeof(row->when));
    prv_copy_field(row->description, u1 ? u1 + 1 : "", sizeof(row->description));
    prv_copy_field(row->location, u2 ? u2 + 1 : "", sizeof(row->location));
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

// Which parcel, and where it is now — pinned above the scrolling event list so
// both stay readable however far back you scroll.
static void prv_draw_header(GContext *ctx, const Layer *cell, uint16_t section,
                            void *context) {
  GRect bounds = layer_get_bounds(cell);
#if defined(PBL_COLOR)
  graphics_context_set_fill_color(ctx, status_colour(s_milestone));
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
#endif
  GRect line = GRect(bounds.origin.x + 4, bounds.origin.y - 4,
                     bounds.size.w - 8, 18);
  graphics_draw_text(ctx, s_nickname, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     line, GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                     NULL);
  line.origin.y += 15;
  graphics_draw_text(ctx, s_label, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     line, GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                     NULL);
}

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx,
                         void *context) {
  if (s_state == STATE_LOADING) {
    menu_cell_basic_draw(ctx, cell, "Loading…", s_nickname, NULL);
    return;
  }
  if (s_state == STATE_ERROR) {
    menu_cell_basic_draw(ctx, cell, s_status, "Press back", NULL);
    return;
  }
  if (s_count == 0) {
    menu_cell_basic_draw(ctx, cell, "No events yet",
                         "The carrier hasn't scanned it", NULL);
    return;
  }

  EventRow *row = &s_rows[idx->row];
  char sub[68];
  if (row->location[0]) {
    snprintf(sub, sizeof(sub), "%s · %s", row->when, row->location);
  } else {
    prv_copy_field(sub, row->when, sizeof(sub));
  }
  menu_cell_basic_draw(ctx, cell, row->description, sub, NULL);
}

static void prv_select_click(MenuLayer *ml, MenuIndex *idx, void *context) {
  if (s_state != STATE_LOADED || s_count == 0) {
    return;
  }
  EventRow *row = &s_rows[idx->row];
  event_window_push(row->when, row->description, row->location,
                    status_colour(s_milestone));
}

// --- comm handlers -------------------------------------------------------

static void prv_on_data(const char *type, const char *payload, int pending) {
  if (strcmp(type, "events") != 0) {
    return;
  }
  prv_parse_events(payload);
  // An empty payload with more coming is the phone's "still fetching" ack.
  s_state = (s_count == 0 && pending != 0) ? STATE_LOADING : STATE_LOADED;
  prv_reload();
}

static void prv_on_error(const char *message) {
  if (s_state == STATE_LOADED && s_count > 0) {
    return; // keep the events we already showed
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
  window_destroy(s_window);
  s_window = NULL;
}

// The event detail window doesn't touch comm, but the parcel list does — take
// the handlers back whenever this window becomes visible again.
static void prv_window_appear(Window *window) {
  comm_set_handlers(prv_on_data, prv_on_error);
}

void timeline_window_push(int index, const char *nickname, const char *label,
                          const char *milestone) {
  prv_copy_field(s_nickname, nickname, sizeof(s_nickname));
  prv_copy_field(s_label, label, sizeof(s_label));
  prv_copy_field(s_milestone, milestone, sizeof(s_milestone));
  s_count = 0;
  s_state = STATE_LOADING;
  comm_set_handlers(prv_on_data, prv_on_error);

  if (!s_window) {
    s_window = window_create();
    window_set_window_handlers(s_window, (WindowHandlers) {
      .load = prv_window_load,
      .unload = prv_window_unload,
      .appear = prv_window_appear,
    });
  }
  window_stack_push(s_window, true);
  comm_request_detail(index);
}
