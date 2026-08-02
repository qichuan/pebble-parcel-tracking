#include "event_window.h"

#define PADDING 5
#define FRAME_W 4 // status-coloured border around the scrolling card
#define BUF_LEN 256

static Window *s_window;
static ScrollLayer *s_scroll_layer;
static TextLayer *s_text_layer;
static GColor s_accent;
static char s_body[BUF_LEN];

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  // The window background shows through as a border in the parcel's status
  // colour; the text sits on a plain card so it stays legible on every hue.
  window_set_background_color(window, s_accent);
#if defined(PBL_ROUND)
  GRect card = grect_inset(bounds, GEdgeInsets(0, 16));
#else
  GRect card = grect_inset(bounds, GEdgeInsets(FRAME_W, FRAME_W, 0, FRAME_W));
#endif

  s_scroll_layer = scroll_layer_create(card);
  scroll_layer_set_click_config_onto_window(s_scroll_layer, window);
  scroll_layer_set_shadow_hidden(s_scroll_layer, false);

  s_text_layer = text_layer_create(GRect(PADDING, PADDING,
                                         card.size.w - 2 * PADDING, 2000));
  text_layer_set_text(s_text_layer, s_body);
  text_layer_set_font(s_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_background_color(s_text_layer, GColorWhite);
  text_layer_set_text_color(s_text_layer, GColorBlack);
  text_layer_set_overflow_mode(s_text_layer, GTextOverflowModeWordWrap);
#if defined(PBL_ROUND)
  text_layer_set_text_alignment(s_text_layer, GTextAlignmentCenter);
#endif

  // Size the card to the text so short events don't leave a scrollable void.
  GSize used = text_layer_get_content_size(s_text_layer);
  text_layer_set_size(s_text_layer,
                      GSize(card.size.w - 2 * PADDING, used.h + PADDING));
  scroll_layer_set_content_size(s_scroll_layer,
                                GSize(card.size.w, used.h + 3 * PADDING));

  scroll_layer_add_child(s_scroll_layer, text_layer_get_layer(s_text_layer));
  layer_add_child(root, scroll_layer_get_layer(s_scroll_layer));
}

static void prv_window_unload(Window *window) {
  text_layer_destroy(s_text_layer);
  s_text_layer = NULL;
  scroll_layer_destroy(s_scroll_layer);
  s_scroll_layer = NULL;
  window_destroy(s_window);
  s_window = NULL;
}

void event_window_push(const char *when, const char *description,
                       const char *location, GColor accent) {
  s_accent = accent;
  if (location && location[0]) {
    snprintf(s_body, sizeof(s_body), "%s\n\n%s\n%s", description, location, when);
  } else {
    snprintf(s_body, sizeof(s_body), "%s\n\n%s", description, when);
  }

  if (!s_window) {
    s_window = window_create();
    window_set_window_handlers(s_window, (WindowHandlers) {
      .load = prv_window_load,
      .unload = prv_window_unload,
    });
  }
  window_stack_push(s_window, true);
}
