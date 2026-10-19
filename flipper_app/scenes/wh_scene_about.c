#include "../wavehound_i.h"

/* Injected by fbt from fap_version in application.fam (e.g. "1.3"). */
#ifndef FAP_VERSION
#define FAP_VERSION "?"
#endif

static const char* const about_text =
    "\e#Wavehound " FAP_VERSION "\n"
    "WiFi fox hunting for the\n"
    "Flipper WiFi dev board\n"
    "running Wavehound fw.\n\n"
    "\e#Scanning\n"
    "Networks and devices\n"
    "appear live as they are\n"
    "heard. Pick one anytime\n"
    "or keep waiting. Select\n"
    "~ Search again to restart.\n\n"
    "\e#Hunt access point\n"
    "Scan for networks, pick\n"
    "one, then\n"
    "walk. Clicks speed up and\n"
    "the board LED turns red\n"
    "as you get closer.\n\n"
    "\e#Hunt devices\n"
    "Pick all channels, one\n"
    "channel, or the channel\n"
    "of a nearby AP. Then\n"
    "scan for anything\n"
    "that is not an\n"
    "access point: phones,\n"
    "laptops, plugs, cameras.\n"
    "* = randomized MAC\n"
    "(may change over time).\n\n"
    "\e#Enter MAC\n"
    "Type a MAC, then pick\n"
    "Auto (hop until heard),\n"
    "a channel, or a nearby\n"
    "AP's channel. A chosen\n"
    "channel is kept even\n"
    "when the target is quiet.\n\n"
    "\e#Device details\n"
    "After picking a result:\n"
    "vendor, type, signal.\n"
    "Hunt or Save it.\n\n"
    "\e#Hunt screen\n"
    "OK: mute  Left: reset pk\n"
    "Right: save this MAC\n"
    "?ch = still searching\n\n"
    "\e#Saved MACs\n"
    "Favorites saved from the\n"
    "hunt screen. Pick one for\n"
    "Hunt / Edit / Delete.\n\n"
    "\e#Spectrum\n"
    "OK/Left/Right: metric\n\n"
    "\e#Board info\n"
    "Firmware, chip and\n"
    "health of the dev board.\n\n"
    "Receive-only. Only hunt\n"
    "devices you own or are\n"
    "allowed to locate.";

void wh_scene_about_on_enter(void* context) {
    WavehoundApp* app = context;
    widget_reset(app->widget);
    widget_add_text_scroll_element(app->widget, 0, 0, 128, 64, about_text);
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewWidget);
}

bool wh_scene_about_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void wh_scene_about_on_exit(void* context) {
    WavehoundApp* app = context;
    widget_reset(app->widget);
}
