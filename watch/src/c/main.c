// Parcel list — the app's root window, and the screen that has to answer
// "is anything arriving today?" in under a second.
//
// The title bar carries that answer, the rows carry one parcel each in two
// lines, and a 4px stripe in the left gutter marks the ones landing today so
// they read without being read. Rows come from the phone as a packed "parcels"
// payload; everything here is presentation only.
#include <pebble.h>
#include "comm.h"
#include "status_colour.h"
#include "timeline_window.h"

#define US_CHAR '\x1f'
#define PARCEL_MAX 12
#define STRIPE_W 4 // "arriving today" marker down the left edge of a row

typedef enum { STATE_LOADING, STATE_LOADED, STATE_ERROR } State;

typedef struct {
  char nickname[40];
  char milestone[24]; // Ship24 status code, drives the status colour
  char label[36];     // human-readable status, or why the fetch failed
  bool today;         // landing (or landed) today
} ParcelRow;

static Window *s_window;
static MenuLayer *s_menu_layer;
static ParcelRow s_rows[PARCEL_MAX];
static int s_count;
static State s_state;
static bool s_updating;
static bool s_disconnected; // showing cached rows the phone can't confirm
static int s_today_count;
static char s_updated[24]; // "2:40 PM", or empty if never fetched
static char s_status[64];

// Metrics, chosen from the real screen width rather than platform macros so a
// new Pebble with a bigger panel gets the larger treatment for free.
static int s_row_h;
static int s_header_h;
static int s_name_y;
static int s_sub_y;
static int s_pad; // horizontal breathing room at the screen edge
static GFont s_name_font;
static GFont s_sub_font;
static GFont s_head_font;

// --- parsing -------------------------------------------------------------

static void prv_copy_field(char *dst, const char *src, size_t size) {
  strncpy(dst, src, size - 1);
  dst[size - 1] = '\0';
}

// Splits a line on US in place, returning the field at `n` (or "" if absent).
// The caller's buffer is modified — every field is NUL-terminated where its
// separator used to be.
static const char *prv_field(char *line, int n) {
  char *p = line;
  for (int i = 0; i < n; i++) {
    p = strchr(p, US_CHAR);
    if (!p) return "";
    *p = '\0';
    p++;
  }
  char *end = strchr(p, US_CHAR);
  if (end) *end = '\0';
  return p;
}

// "todayCount<US>updated\n" then "nickname<US>milestone<US>label<US>today\n"…
// Read a line at a time so the payload is never copied whole (aplite has 24 KB
// of heap for everything).
static void prv_parse_parcels(const char *payload) {
  s_count = 0;
  s_today_count = 0;
  s_updated[0] = '\0';

  const char *p = payload;
  char line[160];
  bool meta_seen = false;

  while (*p && s_count < PARCEL_MAX) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = '\0';
    p = nl ? nl + 1 : p + strlen(p);

    if (!meta_seen) {
      meta_seen = true;
      char meta[160];
      prv_copy_field(meta, line, sizeof(meta));
      s_today_count = atoi(prv_field(meta, 0));
      prv_copy_field(meta, line, sizeof(meta));
      prv_copy_field(s_updated, prv_field(meta, 1), sizeof(s_updated));
      continue;
    }
    if (line[0] == '\0') continue;

    ParcelRow *row = &s_rows[s_count];
    char scratch[160];
    prv_copy_field(scratch, line, sizeof(scratch));
    prv_copy_field(row->nickname, prv_field(scratch, 0), sizeof(row->nickname));
    prv_copy_field(scratch, line, sizeof(scratch));
    prv_copy_field(row->milestone, prv_field(scratch, 1), sizeof(row->milestone));
    prv_copy_field(scratch, line, sizeof(scratch));
    prv_copy_field(row->label, prv_field(scratch, 2), sizeof(row->label));
    prv_copy_field(scratch, line, sizeof(scratch));
    row->today = prv_field(scratch, 3)[0] == '1';
    s_count++;
  }
}

// --- rendering -----------------------------------------------------------

static void prv_reload(void) {
  if (s_menu_layer) {
    menu_layer_reload_data(s_menu_layer);
  }
}

// A full-screen state has nothing to select and gets the whole menu to itself.
static bool prv_is_full_screen(void) {
  return s_state != STATE_LOADED || s_count == 0;
}

// The update time as a footer: always when we're showing data the phone can't
// confirm, and on tall screens that have the room to spare anyway.
static bool prv_has_footer(void) {
#if defined(PBL_ROUND)
  return false; // no straight bottom edge to hang it on
#else
  if (prv_is_full_screen() || !s_updated[0]) return false;
  return s_disconnected || s_row_h >= 46;
#endif
}

static uint16_t prv_get_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  if (prv_is_full_screen()) return 1;
  return s_count + (prv_has_footer() ? 1 : 0);
}

static int16_t prv_cell_height(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  if (prv_is_full_screen()) {
    GRect bounds = layer_get_bounds(menu_layer_get_layer(s_menu_layer));
    return bounds.size.h - s_header_h;
  }
  if (idx->row >= s_count) return 22; // footer
  // Disconnected rows drop the second line's breathing room: the design tightens
  // them to signal "this is what we last knew", not "this is what's true".
  return s_disconnected ? s_row_h - 4 : s_row_h;
}

static int16_t prv_header_height(MenuLayer *ml, uint16_t section, void *ctx) {
  return s_header_h;
}

// The glance bar. It answers the question the app exists for, so it never shows
// a number the user has to interpret — "1 ARRIVING TODAY", not "1/4".
static void prv_header_text(char *out, size_t size) {
  if (s_disconnected) {
    strncpy(out, "PHONE NOT CONNECTED", size - 1);
  } else if (s_state != STATE_LOADED || s_count == 0) {
    strncpy(out, "PARCELS", size - 1);
  } else if (s_updating && s_today_count == 0) {
    // We have the names but not the statuses yet, so "nothing due today" would
    // be a guess dressed up as an answer. Say what's actually happening until
    // the phone comes back — a count above zero is already true and can stand.
    strncpy(out, "CHECKING…", size - 1);
  } else if (s_today_count == 0) {
    // Deliberately the same sentence as the counted form with the number swapped
    // out, so the bar reads as one pattern rather than two unrelated messages.
    strncpy(out, "NONE ARRIVING TODAY", size - 1);
  } else {
    snprintf(out, size, "%d ARRIVING TODAY", s_today_count);
  }
  out[size - 1] = '\0';
}

static void prv_draw_header(GContext *ctx, const Layer *cell, uint16_t section,
                            void *context) {
  char title[32];
  prv_header_text(title, sizeof(title));
  GRect bounds = layer_get_bounds(cell);

#if defined(PBL_ROUND)
  // An inverted bar would have both ends clipped by the circular mask, so the
  // round build states the same thing in quiet type instead.
  graphics_context_set_text_color(ctx, COLOUR_DIM);
  GRect box = GRect(bounds.origin.x, bounds.origin.y + bounds.size.h - 18,
                    bounds.size.w, 16);
  graphics_draw_text(ctx, title, s_head_font, box, GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
#else
  graphics_context_set_fill_color(ctx, s_disconnected ? COLOUR_DIM : COLOUR_INK);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, COLOUR_PAPER);
  GRect box = GRect(bounds.origin.x + s_pad, bounds.origin.y - 2,
                    bounds.size.w - s_pad * 2, bounds.size.h);
  graphics_draw_text(ctx, title, s_head_font, box, GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
#endif
}

// Centres a bold headline over an optional line of quiet supporting text.
static void prv_draw_centred(GContext *ctx, GRect bounds, const char *headline,
                             const char *detail, int top_offset) {
  GRect box = GRect(bounds.origin.x + 10, 0, bounds.size.w - 20, 200);
  GSize head_size = graphics_text_layout_get_content_size(
      headline, s_name_font, box, GTextOverflowModeWordWrap, GTextAlignmentCenter);
  GSize detail_size = { 0, 0 };
  if (detail && detail[0]) {
    detail_size = graphics_text_layout_get_content_size(
        detail, s_sub_font, box, GTextOverflowModeWordWrap, GTextAlignmentCenter);
  }
  int total = head_size.h + (detail_size.h ? detail_size.h + 6 : 0);
  int y = bounds.origin.y + (bounds.size.h - total) / 2 + top_offset;

  graphics_context_set_text_color(ctx, COLOUR_INK);
  box.origin.y = y;
  box.size.h = head_size.h + 4;
  graphics_draw_text(ctx, headline, s_name_font, box, GTextOverflowModeWordWrap,
                     GTextAlignmentCenter, NULL);
  if (detail_size.h) {
    graphics_context_set_text_color(ctx, COLOUR_DIM);
    box.origin.y = y + head_size.h + 6;
    box.size.h = detail_size.h + 4;
    graphics_draw_text(ctx, detail, s_sub_font, box, GTextOverflowModeWordWrap,
                       GTextAlignmentCenter, NULL);
  }
}

static void prv_draw_empty(GContext *ctx, GRect bounds) {
  // A parcel-shaped outline, drawn rather than shipped as a resource so it
  // stays crisp on every density.
  int w = 26, h = 20;
  GRect box = GRect(bounds.origin.x + (bounds.size.w - w) / 2,
                    bounds.origin.y + bounds.size.h / 2 - 44, w, h);
  graphics_context_set_stroke_color(ctx, COLOUR_RULE);
  graphics_draw_rect(ctx, box);
  graphics_draw_rect(ctx, GRect(box.origin.x + 1, box.origin.y + 1, w - 2, h - 2));

  prv_draw_centred(ctx, bounds, "Nothing on the way",
                   "Add a tracking number on your phone and it shows up here.", 8);
}

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx,
                         void *context) {
  GRect bounds = layer_get_bounds(cell);

  if (s_state == STATE_LOADING) {
    char detail[48];
    if (s_updated[0]) {
      snprintf(detail, sizeof(detail), "Last update %s", s_updated);
    } else {
      strncpy(detail, "This only takes a moment.", sizeof(detail));
      detail[sizeof(detail) - 1] = '\0';
    }
    prv_draw_centred(ctx, bounds, "Checking with your phone…", detail, 0);
    return;
  }
  if (s_state == STATE_ERROR) {
    prv_draw_centred(ctx, bounds, s_status, "Press select to try again", 0);
    return;
  }
  if (s_count == 0) {
    prv_draw_empty(ctx, bounds);
    return;
  }

  // Footer: what we're showing and when it was true.
  if (idx->row >= s_count) {
    char footer[40];
    snprintf(footer, sizeof(footer), s_disconnected ? "Showing %s data"
                                                    : "Updated %s", s_updated);
    graphics_context_set_text_color(ctx, COLOUR_DIM);
    graphics_draw_text(ctx, footer, s_sub_font,
                       GRect(bounds.origin.x + s_pad + STRIPE_W,
                             bounds.origin.y - 2,
                             bounds.size.w - (s_pad + STRIPE_W) - s_pad, 20),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    return;
  }

  ParcelRow *row = &s_rows[idx->row];
  bool selected = menu_cell_layer_is_highlighted(cell);

  // Selection is the only interactive affordance on screen, so it takes the
  // strongest fill the platform has: the accent in colour, an invert in b/w.
  GColor name_colour = COLOUR_INK;
  GColor sub_colour = COLOUR_DIM;
  if (selected) {
#if defined(PBL_COLOR)
    name_colour = COLOUR_INK;
    sub_colour = COLOUR_INK;
#else
    name_colour = GColorWhite;
    sub_colour = GColorWhite;
#endif
  } else if (s_disconnected) {
    // Nothing here is confirmed, so nothing here shouts.
    name_colour = COLOUR_DIM;
  }

#if defined(PBL_RECT)
  // The today marker. Never on the selected row — that row is already the
  // accent colour, and a stripe on top of it would say nothing.
  if (row->today && !selected) {
    graphics_context_set_fill_color(ctx, s_disconnected ? COLOUR_RULE : COLOUR_ACCENT);
    graphics_fill_rect(ctx, GRect(0, 0, STRIPE_W, bounds.size.h), 0, GCornerNone);
  }
  graphics_context_set_stroke_color(ctx, COLOUR_RULE);
  graphics_draw_line(ctx, GPoint(0, bounds.size.h - 1),
                     GPoint(bounds.size.w, bounds.size.h - 1));
  // Text clears the stripe by the same margin it keeps from the screen edge.
  int left = bounds.origin.x + s_pad + STRIPE_W;
  int width = bounds.size.w - (s_pad + STRIPE_W) - s_pad;
  GTextAlignment align = GTextAlignmentLeft;
#else
  // Round: the mask eats a gutter stripe and any left-aligned text, so rows are
  // centred and the today marker is carried by the accent fill alone.
  int left = bounds.origin.x + s_pad;
  int width = bounds.size.w - s_pad * 2;
  GTextAlignment align = GTextAlignmentCenter;
#endif

  int name_y = bounds.origin.y + s_name_y;
  int sub_y = bounds.origin.y + (s_disconnected ? s_sub_y - 3 : s_sub_y);

  // Boxes run to the bottom of the cell rather than a fixed height: both lines
  // are single-line and ellipsized, so extra room costs nothing and a box that
  // is shorter than the font's line height clips the descenders.
  graphics_context_set_text_color(ctx, name_colour);
  graphics_draw_text(ctx, row->nickname, s_name_font,
                     GRect(left, name_y, width, bounds.size.h - s_name_y),
                     GTextOverflowModeTrailingEllipsis, align, NULL);

  graphics_context_set_text_color(ctx, sub_colour);
  graphics_draw_text(ctx, row->label, s_sub_font,
                     GRect(left, sub_y, width, bounds.size.h - s_sub_y),
                     GTextOverflowModeTrailingEllipsis, align, NULL);
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
  if (s_state != STATE_LOADED || s_count == 0 || idx->row >= s_count) {
    return;
  }
  timeline_window_push(idx->row, s_count, s_rows[idx->row].nickname);
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
  s_disconnected = false; // the phone just answered
  // Nothing yet but more is coming: an interim ack, not an empty parcel list.
  s_state = (s_count == 0 && s_updating) ? STATE_LOADING : STATE_LOADED;
  prv_reload();
}

// Bluetooth came or went. Say so at once rather than letting the user find out
// when a request eventually times out — and when it comes back, comm replays
// the outstanding request itself, so there's nothing to do but stop claiming
// the data is unconfirmed.
static void prv_on_connection(bool connected) {
  if (!connected) {
    if (s_state == STATE_LOADED && s_count > 0) {
      s_disconnected = true;
      s_updating = false;
      prv_reload();
    }
    return;
  }
  s_disconnected = false;
  prv_reload();
}

static void prv_on_error(const char *message) {
  s_updating = false;
  // A failed refresh shouldn't wipe a list we already have — it should say so.
  // The rows stay, greyed, under a title that stops claiming anything about
  // today, with the time they were true along the bottom.
  if (s_state == STATE_LOADED && s_count > 0) {
    s_disconnected = true;
    prv_reload();
    return;
  }
  s_state = STATE_ERROR;
  prv_copy_field(s_status, message, sizeof(s_status));
  prv_reload();
}

// --- window --------------------------------------------------------------

static void prv_choose_metrics(GRect bounds) {
#if defined(PBL_ROUND)
  s_header_h = 26;
  s_row_h = 48;
  s_name_y = 2;
  s_sub_y = 24;
  s_pad = 16; // the mask cuts into both ends of every line
  s_name_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  s_sub_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  s_head_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
#else
  if (bounds.size.w >= 180) {
    // Emery and friends: type and rows step up with the panel.
    s_header_h = 26;
    s_row_h = 58;
    s_name_y = 2;
    s_sub_y = 29;
    s_pad = 10;
    s_name_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
    s_sub_font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
    s_head_font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  } else {
    s_header_h = 20;
    s_row_h = 43;
    s_name_y = 0;
    s_sub_y = 21;
    s_pad = 8;
    s_name_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
    s_sub_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
    // The glance bar stays a step down: it labels the list rather than being
    // it, and "NONE ARRIVING TODAY" only fits across 144px at this size.
    s_head_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  }
#endif
}

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  prv_choose_metrics(bounds);

  window_set_background_color(window, COLOUR_PAPER);
  s_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_rows = prv_get_num_rows,
    .get_cell_height = prv_cell_height,
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
  menu_layer_set_normal_colors(s_menu_layer, COLOUR_PAPER, COLOUR_INK);
  menu_layer_set_highlight_colors(s_menu_layer, COLOUR_ACCENT, COLOUR_INK);
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
  comm_set_connection_handler(prv_on_connection);
  // The timeline window cancels its request on the way out, so if we came back
  // with nothing to show there is no longer anything in flight to wait for.
  if (s_state != STATE_LOADED || s_count == 0) {
    prv_start_request(false);
  }
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
