// One parcel, one long scroll.
//
// Status headline first, then the three facts worth a scroll — where it is,
// when it lands, what its number is — then the full history, newest at top,
// grouped by day. There is no third screen: event text word-wraps here instead
// of being ellipsized and opened elsewhere.
//
// The whole page is drawn by one update_proc. prv_layout walks the content once
// to measure it (for the scroll height) and again to draw it, so the two can't
// drift apart.
#include "timeline_window.h"
#include "comm.h"
#include "status_colour.h"

#define US_CHAR '\x1f'
#define EVENT_MAX 12
#define DOT 5

typedef enum { STATE_LOADING, STATE_LOADED, STATE_ERROR } State;

typedef struct {
  char day[16];    // section heading, set only on the first event of a day
  char label[164];
  char time[60];   // "1:32 PM", or "1:32 PM · Leipzig, DE" when it moved
} EventRow;

static Window *s_window;
static ScrollLayer *s_scroll_layer;
static Layer *s_content_layer;
static Layer *s_header_layer;

static EventRow s_rows[EVENT_MAX];
static int s_count;
static State s_state;
static char s_status_message[64];

// From the payload's meta record.
static char s_courier[28];
static char s_status[36];
static char s_when[28];
static char s_place[32];
static char s_eta[28];
static char s_tracking[36];
static char s_milestone[24];

static char s_nickname[40];
static int s_index;
static int s_total;

static int s_header_h;
static int s_pad;    // horizontal breathing room at the screen edge
static int s_rail_x; // the timeline's dot column, just outside the text margin
static int s_indent; // timeline text, clear of the dots
// Vertical metrics that follow the type scale rather than a fixed line height.
static int s_caption_dy; // caption baseline to its value
static int s_day_h;      // a day heading's own line
static int s_dot_dy;     // dot centred on the event label's first line
static GFont s_hero_font;
static GFont s_value_font;
static GFont s_body_font;
static GFont s_small_font;

static void prv_copy_field(char *dst, const char *src, size_t size) {
  strncpy(dst, src, size - 1);
  dst[size - 1] = '\0';
}

// --- parsing -------------------------------------------------------------

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

static void prv_take(char *dst, size_t size, const char *line, int n) {
  char scratch[224];
  prv_copy_field(scratch, line, sizeof(scratch));
  prv_copy_field(dst, prv_field(scratch, n), size);
}

// meta:  courier<US>status<US>when<US>place<US>eta<US>tracking<US>milestone
// event: day<US>label<US>time, newest first (the phone sorts and groups).
static void prv_parse_events(const char *payload) {
  s_count = 0;
  s_courier[0] = s_status[0] = s_when[0] = '\0';
  s_place[0] = s_eta[0] = s_tracking[0] = s_milestone[0] = '\0';

  const char *p = payload;
  char line[224];
  bool meta_seen = false;

  while (*p && s_count < EVENT_MAX) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = '\0';
    p = nl ? nl + 1 : p + strlen(p);

    if (!meta_seen) {
      meta_seen = true;
      prv_take(s_courier, sizeof(s_courier), line, 0);
      prv_take(s_status, sizeof(s_status), line, 1);
      prv_take(s_when, sizeof(s_when), line, 2);
      prv_take(s_place, sizeof(s_place), line, 3);
      prv_take(s_eta, sizeof(s_eta), line, 4);
      prv_take(s_tracking, sizeof(s_tracking), line, 5);
      prv_take(s_milestone, sizeof(s_milestone), line, 6);
      continue;
    }
    if (line[0] == '\0') continue;

    EventRow *row = &s_rows[s_count];
    prv_take(row->day, sizeof(row->day), line, 0);
    prv_take(row->label, sizeof(row->label), line, 1);
    prv_take(row->time, sizeof(row->time), line, 2);
    s_count++;
  }
}

// --- layout --------------------------------------------------------------

static GSize prv_measure(const char *text, GFont font, int width) {
  return graphics_text_layout_get_content_size(
      text, font, GRect(0, 0, width, 2000), GTextOverflowModeWordWrap,
      PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft));
}

static void prv_text(GContext *ctx, const char *text, GFont font, GColor colour,
                     int x, int y, int width) {
  graphics_context_set_text_color(ctx, colour);
  graphics_draw_text(ctx, text, font, GRect(x, y - 3, width, 2000),
                     GTextOverflowModeWordWrap,
                     PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft),
                     NULL);
}

// One labelled fact: a quiet uppercase caption over its value. `quiet` drops the
// value to caption weight — the tracking number is there to be copied off the
// screen, not read, and shouldn't compete with where the parcel actually is.
static int prv_fact(GContext *ctx, const char *caption, const char *value,
                    int y, int width, bool draw, bool quiet) {
  if (!value[0]) return 0;
  int inner = width - s_pad * 2;
  GFont font = quiet ? s_small_font : s_value_font;
  GSize size = prv_measure(value, font, inner);
  if (draw) {
    prv_text(ctx, caption, s_small_font, COLOUR_DIM, s_pad, y, inner);
    prv_text(ctx, value, font, quiet ? COLOUR_DIM : COLOUR_INK, s_pad,
             y + s_caption_dy, inner);
  }
  return s_caption_dy + size.h + 3;
}

// Walks the whole page. Returns its total height; draws it too when `draw`.
static int prv_layout(GContext *ctx, int width, bool draw) {
  int y = 0;
  int inner = width - s_pad * 2;

  // Status headline, on a block of the status colour — the one thing worth
  // seeing before anything has scrolled.
  GSize head = prv_measure(s_status, s_hero_font, inner);
  GSize when = s_when[0] ? prv_measure(s_when, s_small_font, inner)
                         : (GSize) { 0, 0 };
  int hero_h = s_pad + head.h + (when.h ? when.h - 1 : 0) + s_pad;
  if (draw) {
    graphics_context_set_fill_color(ctx, status_colour(s_milestone));
    graphics_fill_rect(ctx, GRect(0, y, width, hero_h), 0, GCornerNone);
    GColor on_fill = status_colour_is_dark(s_milestone) ? GColorWhite : GColorBlack;
    prv_text(ctx, s_status, s_hero_font, on_fill, s_pad, y + s_pad, inner);
    if (when.h) {
      prv_text(ctx, s_when, s_small_font, on_fill, s_pad, y + s_pad + head.h - 1, inner);
    }
  }
  y += hero_h + 4;

  y += prv_fact(ctx, "NOW AT", s_place, y, width, draw, false);
  y += prv_fact(ctx, "EXPECTED", s_eta, y, width, draw, false);
  y += prv_fact(ctx, "TRACKING", s_tracking, y, width, draw, true);

  if (s_count > 0) {
    y += 3;
    if (draw) {
      graphics_context_set_stroke_color(ctx, COLOUR_RULE);
      graphics_draw_line(ctx, GPoint(0, y), GPoint(width, y));
    }
    y += 5;
  }

#if defined(PBL_ROUND)
  int text_x = s_pad + 8;
  int text_w = width - (s_pad + 8) * 2;
#else
  int text_x = s_indent;
  int text_w = width - s_indent - s_pad;
#endif

  for (int i = 0; i < s_count; i++) {
    EventRow *row = &s_rows[i];
    if (row->day[0]) {
      if (i > 0) y += 4;
      if (draw) {
        prv_text(ctx, row->day, s_small_font, COLOUR_DIM, text_x, y, text_w);
      }
      y += s_day_h;
    }

    GSize label = prv_measure(row->label, s_body_font, text_w);
    GSize time = row->time[0] ? prv_measure(row->time, s_small_font, text_w)
                              : (GSize) { 0, 0 };
    int entry_h = label.h + (time.h ? time.h - 2 : 0) + 5;

    if (draw) {
#if defined(PBL_RECT)
      // A dot per event on a hairline rail. The newest one takes the accent;
      // it's the event the status headline is talking about.
      graphics_context_set_fill_color(ctx, i == 0 ? COLOUR_ACCENT : COLOUR_DIM);
      graphics_fill_rect(ctx, GRect(s_rail_x, y + s_dot_dy, DOT, DOT), 0,
                         GCornerNone);
      if (i < s_count - 1) {
        graphics_context_set_stroke_color(ctx, COLOUR_RULE);
        graphics_draw_line(ctx, GPoint(s_rail_x + 2, y + s_dot_dy + DOT),
                           GPoint(s_rail_x + 2, y + entry_h + s_dot_dy));
      }
#endif
      prv_text(ctx, row->label, s_body_font, COLOUR_INK, text_x, y, text_w);
      if (time.h) {
        prv_text(ctx, row->time, s_small_font, COLOUR_DIM, text_x,
                 y + label.h - 2, text_w);
      }
    }
    y += entry_h;
  }

  return y + 8;
}

// --- rendering -----------------------------------------------------------

static void prv_draw_message(GContext *ctx, GRect bounds, const char *headline,
                             const char *detail) {
  int inner = bounds.size.w - 20;
  GSize head = prv_measure(headline, s_value_font, inner);
  GSize sub = detail[0] ? prv_measure(detail, s_small_font, inner)
                        : (GSize) { 0, 0 };
  int y = (bounds.size.h - head.h - (sub.h ? sub.h + 6 : 0)) / 2;

  graphics_context_set_text_color(ctx, COLOUR_INK);
  graphics_draw_text(ctx, headline, s_value_font, GRect(10, y - 3, inner, 200),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  if (sub.h) {
    graphics_context_set_text_color(ctx, COLOUR_DIM);
    graphics_draw_text(ctx, detail, s_small_font,
                       GRect(10, y + head.h + 3, inner, 200),
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }
}

static void prv_content_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, COLOUR_PAPER);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  if (s_state == STATE_LOADING) {
    prv_draw_message(ctx, bounds, s_nickname, "Checking with your phone…");
    return;
  }
  if (s_state == STATE_ERROR) {
    prv_draw_message(ctx, bounds, s_status_message, "Press back");
    return;
  }
  if (s_count == 0 && !s_status[0]) {
    prv_draw_message(ctx, bounds, "No events yet",
                     "The carrier hasn't scanned it.");
    return;
  }
  prv_layout(ctx, bounds.size.w, true);
}

// Courier on the left, position in the list on the right — the two things the
// scrolling content below stops telling you once it has moved.
static void prv_header_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, COLOUR_INK);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  char position[24];
  snprintf(position, sizeof(position), "%d/%d", s_index + 1, s_total);

  const char *title = s_courier[0] ? s_courier : s_nickname;
#if defined(PBL_ROUND)
  graphics_context_set_text_color(ctx, COLOUR_PAPER);
  graphics_draw_text(ctx, title, s_small_font,
                     GRect(bounds.origin.x + 20, bounds.origin.y + 1,
                           bounds.size.w - 40, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
#else
  graphics_context_set_text_color(ctx, COLOUR_PAPER);
  graphics_draw_text(ctx, title, s_small_font,
                     GRect(s_pad, bounds.origin.y - 2, bounds.size.w - s_pad * 2 - 26,
                           bounds.size.h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_context_set_text_color(ctx, COLOUR_RULE);
  graphics_draw_text(ctx, position, s_small_font,
                     GRect(bounds.size.w - s_pad - 30, bounds.origin.y - 2, 30,
                           bounds.size.h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
#endif
}

// Re-measures the page and tells the scroll layer how tall it now is.
static void prv_resize(void) {
  if (!s_scroll_layer || !s_content_layer) return;
  GRect frame = layer_get_frame(scroll_layer_get_layer(s_scroll_layer));
  int height = frame.size.h;
  if (s_state == STATE_LOADED && (s_count > 0 || s_status[0])) {
    int measured = prv_layout(NULL, frame.size.w, false);
    if (measured > height) height = measured;
  }
  layer_set_frame(s_content_layer, GRect(0, 0, frame.size.w, height));
  scroll_layer_set_content_size(s_scroll_layer, GSize(frame.size.w, height));
  layer_mark_dirty(s_content_layer);
  layer_mark_dirty(s_header_layer);
}

// --- comm handlers -------------------------------------------------------

static void prv_on_data(const char *type, const char *payload, int pending) {
  if (strcmp(type, "events") != 0) {
    return;
  }
  prv_parse_events(payload);
  // An empty payload with more coming is the phone's "still fetching" ack.
  s_state = (s_count == 0 && !s_status[0] && pending != 0) ? STATE_LOADING
                                                           : STATE_LOADED;
  prv_resize();
}

static void prv_on_error(const char *message) {
  if (s_state == STATE_LOADED && s_count > 0) {
    return; // keep the events we already showed
  }
  s_state = STATE_ERROR;
  prv_copy_field(s_status_message, message, sizeof(s_status_message));
  prv_resize();
}

// --- window --------------------------------------------------------------

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  window_set_background_color(window, COLOUR_PAPER);

  // Same threshold as the list uses, so a watch can't end up with big type on
  // one screen and small type on the other. Chalk's 180 sits below it.
  bool large = bounds.size.w >= 200;
  s_header_h = large ? 26 : 20;
  // The round mask cuts into both ends of every line, so it needs the most.
  s_pad = PBL_IF_ROUND_ELSE(large ? 24 : 16, large ? 10 : 8);
  s_rail_x = s_pad - 2;   // the rail sits just outside the text margin
  s_indent = s_pad + 10;  // text clears the dots
  s_hero_font = fonts_get_system_font(large ? FONT_KEY_GOTHIC_28_BOLD
                                            : FONT_KEY_GOTHIC_24_BOLD);
  s_value_font = fonts_get_system_font(large ? FONT_KEY_GOTHIC_24_BOLD
                                             : FONT_KEY_GOTHIC_18_BOLD);
  s_body_font = fonts_get_system_font(large ? FONT_KEY_GOTHIC_24
                                            : FONT_KEY_GOTHIC_18);
  // Captions, times and the header stay a step below the body: they label the
  // content rather than being it, and the contrast is what makes them scannable.
  s_small_font = fonts_get_system_font(large ? FONT_KEY_GOTHIC_18
                                             : FONT_KEY_GOTHIC_14);
  s_caption_dy = large ? 20 : 16;
  s_day_h = large ? 22 : 17;
  s_dot_dy = large ? 10 : 7;

  s_header_layer = layer_create(GRect(0, 0, bounds.size.w, s_header_h));
  layer_set_update_proc(s_header_layer, prv_header_update);

  s_scroll_layer = scroll_layer_create(GRect(0, s_header_h, bounds.size.w,
                                             bounds.size.h - s_header_h));
  scroll_layer_set_click_config_onto_window(s_scroll_layer, window);
  scroll_layer_set_shadow_hidden(s_scroll_layer, true);

  s_content_layer = layer_create(GRect(0, 0, bounds.size.w,
                                       bounds.size.h - s_header_h));
  layer_set_update_proc(s_content_layer, prv_content_update);
  scroll_layer_add_child(s_scroll_layer, s_content_layer);

  layer_add_child(root, scroll_layer_get_layer(s_scroll_layer));
  layer_add_child(root, s_header_layer);
  prv_resize();
}

static void prv_window_unload(Window *window) {
  // Drop the request on the way out. Left running, its retries would eventually
  // report a failure on the list window behind us for a screen the user has
  // already left.
  comm_cancel();
  layer_destroy(s_content_layer);
  layer_destroy(s_header_layer);
  scroll_layer_destroy(s_scroll_layer);
  s_content_layer = NULL;
  s_header_layer = NULL;
  s_scroll_layer = NULL;
  window_destroy(s_window);
  s_window = NULL;
}

static void prv_window_appear(Window *window) {
  comm_set_handlers(prv_on_data, prv_on_error);
}

void timeline_window_push(int index, int total, const char *nickname) {
  prv_copy_field(s_nickname, nickname, sizeof(s_nickname));
  s_index = index;
  s_total = total;
  s_count = 0;
  s_status[0] = '\0';
  s_courier[0] = '\0';
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
