#pragma once

#include <expansion/expansion.h>
#include <furi.h>
#include <gui/gui.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/widget.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>

#include "scenes/wh_scene.h"
#include "views/wh_hunt_view.h"
#include "views/wh_spectrum_view.h"
#include "wh_uart.h"

#define WH_MAX_RESULTS 96
#define WH_TICK_MS 250
#define WH_MAX_CHANNEL 13
#define WH_MAX_SAVED 64
#define WH_SAVED_NAME_MAX 24

typedef enum {
    WhViewSubmenu,
    WhViewByteInput,
    WhViewTextInput,
    WhViewWidget,
    WhViewHunt,
    WhViewSpectrum,
} WhViewId;

typedef enum {
    WhEventResultsReady = 100,
    WhEventMacEntered,
    WhEventChannelPicked,
    WhEventBoardInfoReady,
    WhEventSaveTarget,
    WhEventNameEntered,
    WhEventDetailsHunt,
    WhEventDetailsSave,
} WhCustomEvent;

/** A hunt target the user saved by name, persisted to the SD card. */
typedef struct {
    char name[WH_SAVED_NAME_MAX];
    char mac[18]; /* "aa:bb:cc:dd:ee:ff" */
    uint8_t channel; /* 0 = auto / hop */
} WhSavedMac;

/** Who asked the ChannelSelect scene for a channel. */
typedef enum {
    WhChannelForDeviceSearch,
    WhChannelForMacHunt,
} WhChannelPurpose;

/** What the Searching scene listens for (all are timed passive listens). */
typedef enum {
    WhSearchAccessPoints, /* access points only */
    WhSearchDevices,      /* every transmitter that is not an access point */
    WhSearchClients,      /* stations associated with a chosen access point */
} WhSearchKind;

/** One huntable thing: an access point or any other transmitter heard. */
typedef struct {
    char mac[18];    /* "aa:bb:cc:dd:ee:ff" */
    char name[33];   /* SSID for access points, empty if hidden/unknown */
    uint8_t channel; /* 0 = unknown, board hops */
    int8_t rssi;
    uint16_t frames;
    bool is_ap;
    bool randomized; /* locally administered MAC */
    bool pin_channel; /* user chose the channel: board must not hop away */
} WhTarget;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;

    Submenu* submenu;
    ByteInput* byte_input;
    TextInput* text_input;
    Widget* widget;
    WhHuntView* hunt;
    WhSpectrumView* spectrum;

    WhUart* uart;
    Expansion* expansion;
    bool otg_enabled_by_us;
    bool board_alive;

    /* Search results. Written by the UART worker between *_BEGIN and *_END. */
    FuriMutex* results_mutex;
    WhTarget results[WH_MAX_RESULTS];
    size_t result_count;
    uint16_t result_total; /* devices heard, may exceed result_count */
    volatile uint16_t progress_found;

    WhSearchKind search_kind;

    /* Channel selection for device search / MAC hunt. */
    WhChannelPurpose channel_purpose;
    uint8_t device_channel;   /* device search: 0 = hop all channels */
    bool picking_ap_channel;  /* AP list is open only to borrow a channel */
    bool channel_pick_pending; /* an AP was picked; ChannelSelect continues */

    WhTarget target; /* what the Hunt scene follows */
    uint8_t mac_bytes[6]; /* ByteInput buffer, keeps the last entered MAC */

    /* The access point whose clients we are listing (WhSearchClients), chosen
     * via the device-search "Clients of nearby AP..." picker. */
    WhTarget clients_ap;
    bool device_search_clients; /* device search picked a nearby AP: scope to its clients */

    /* Board Info text, filled by the UART worker between INFO_BEGIN/INFO_END
     * (guarded by results_mutex). */
    FuriString* board_info;

    /* Saved MACs (favorites), loaded from the SD card at startup. */
    WhSavedMac saved[WH_MAX_SAVED];
    size_t saved_count;
    uint8_t save_channel;      /* channel captured when saving from the hunt */
    char name_buf[WH_SAVED_NAME_MAX]; /* TextInput buffer for the saved/edited name */

    /* Editing an existing saved entry (the Edit scene's working copy lives in
     * name_buf, mac_bytes and edit_channel). */
    bool editing;             /* name/MAC editors update the working copy in place */
    size_t edit_index;        /* which saved[] entry is being edited */
    uint8_t edit_channel;     /* working channel while editing */
    bool edit_channel_mode;   /* Edit scene is showing its channel chooser */
} WavehoundApp;

/** Send a line to the board (no-op without a UART). */
void wh_send(WavehoundApp* app, const char* line);

/** Parse "aa:bb:cc:dd:ee:ff" into bytes. Returns false on malformed input. */
bool wh_parse_mac(const char* str, uint8_t mac[6]);
