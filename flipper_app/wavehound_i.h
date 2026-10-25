#pragma once

#include <expansion/expansion.h>
#include <furi.h>
#include <gui/gui.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <notification/notification_messages.h>

#include "scenes/wh_scene.h"
#include "views/wh_hunt_view.h"
#include "views/wh_spectrum_view.h"
#include "wh_settings.h"
#include "wh_uart.h"

#define WH_MAX_RESULTS 96
#define WH_TICK_MS 250
#define WH_MAX_CHANNEL 13
#define WH_MAX_SAVED 64
#define WH_SAVED_NAME_MAX 24
#define WH_AP_SSID_MAX 33  /* 32 bytes + NUL */
#define WH_AP_PSK_MAX 65   /* 64 bytes + NUL (WPA passphrase or 64-hex PSK) */
#define WH_MAX_SAVED_AP 32

/* The board firmware version at which AP login (CONNECT/WPS/ELICIT) appeared;
 * the login UI is hidden for older boards (which answer those commands ERR). */
#define WH_FW_LOGIN_MIN 9

/* The board firmware version at which the RESOLVE command appeared; the hunt
 * screen only asks older boards to re-resolve if they understand it. */
#define WH_FW_RESOLVE_MIN 15

/* The board firmware version at which the FP (fingerprint) command appeared; the
 * Scan action is hidden on older boards, which would answer it ERR. */
#define WH_FW_FP_MIN 17

/* Ports a fingerprint can report (the board's fixed scan set, see PROTOCOL.md). */
#define WH_FP_MAX_PORTS 8

typedef enum {
    WhViewSubmenu,
    WhViewByteInput,
    WhViewTextInput,
    WhViewWidget,
    WhViewHunt,
    WhViewSpectrum,
    WhViewVarItemList,
} WhViewId;

typedef enum {
    WhEventMacEntered = 100,
    WhEventChannelPicked,
    WhEventBoardInfoReady,
    WhEventSaveTarget,
    WhEventNameEntered,
    WhEventDetailsHunt,
    WhEventDetailsSave,
    WhEventSettingsReset,
    /* AP login (the "Clients of nearby AP" path). */
    WhEventPskEntered,  /* password keyboard finished */
    WhEventConnGotIp,   /* board reported CONN,GOTIP */
    WhEventConnFail,    /* board reported CONN,FAIL */
    WhEventWpsOk,       /* board reported WPS,OK (credentials captured) */
    WhEventWpsFail,     /* board reported WPS,FAIL */
    WhEventApSave,      /* save-prompt: store the credentials */
    WhEventApDiscard,   /* save-prompt: don't store / skip */
    WhEventElicitToggle, /* hunt screen: Down pressed to toggle the elicitation ping */
    WhEventDetailsFingerprint, /* details screen: center pressed to fingerprint the target */
    WhEventHuntFingerprint,    /* hunt screen: Up pressed to fingerprint the target */
    WhEventFingerprintUpdate,  /* an FP,... line arrived: redraw the fingerprint scene */
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

/** What the live scan (TargetList) listens for. */
typedef enum {
    WhSearchAccessPoints, /* access points only */
    WhSearchDevices,      /* every transmitter that is not an access point */
    WhSearchClients,      /* stations associated with a chosen access point */
} WhSearchKind;

/** One huntable thing: an access point or any other transmitter heard. */
typedef struct {
    char mac[18];    /* "aa:bb:cc:dd:ee:ff" */
    char name[33];   /* SSID for access points, empty if hidden/unknown */
    char security[5]; /* "Open"/"WEP"/"WPA"/"WPA2"/"WPA3", empty if unknown */
    char phy[3];     /* "g"/"n"/"ac"/"ax" (802.11 mode), empty if unknown */
    uint8_t channel; /* 0 = unknown, board hops */
    int8_t rssi;
    uint16_t frames;
    bool is_ap;
    bool randomized; /* locally administered MAC */
    bool pin_channel; /* user chose the channel: board must not hop away */
    char ip[16];   /* IPv4 on the associated subnet (from IP,... lines), else empty */
    char host[33]; /* best-effort hostname, else empty */
    char dtype[16]; /* device class from mDNS/SSDP ("Chromecast", "Printer"), else empty */
} WhTarget;

/** An IP/host learned before its device row existed (ARP replies can beat the
 * client scan, and the post-login subnet sweep runs before the client list is
 * open). Held until the matching row appears, then copied into it. */
typedef struct {
    char mac[18];
    char ip[16];
    char host[33];
    char dtype[16];
} WhPendingIp;

/** A target fingerprint, built from the board's FP,... lines (guarded by
 * results_mutex; the fingerprint scene reads it on WhEventFingerprintUpdate). */
typedef struct {
    char mac[18];
    char ip[16];
    bool running;  /* a scan is in progress (FP,BEGIN seen, no FP,DONE yet) */
    bool done;     /* the scan finished (FP,DONE or FP,ERR) */
    bool err;      /* the scan could not run (FP,ERR) */
    char err_msg[24];
    uint16_t ports[WH_FP_MAX_PORTS];
    uint8_t port_count;
    char server[40]; /* HTTP Server: header, else empty */
    char title[40];  /* HTTP page <title>, else empty */
    char dtype[16];  /* board's device-class guess, else empty */
} WhFingerprint;

/** A saved AP credential, persisted to the SD card (plaintext, like saved MACs). */
typedef struct {
    char ssid[WH_AP_SSID_MAX];
    char security[5]; /* "Open"/"WEP"/"WPA"/"WPA2"/"WPA3", best-effort label */
    char psk[WH_AP_PSK_MAX];
} WhSavedAp;

/** Board association state, updated from CONN,.../WPS,... lines (event-published:
 * each field is written before the custom event that makes a scene read it). */
typedef struct {
    volatile bool connected;  /* holds a DHCP lease */
    volatile bool connecting; /* a CONNECT / WPS is in flight */
    char ssid[WH_AP_SSID_MAX];
    char ip[16];
    int fail_reason;     /* last CONN,FAIL code */
    char fail_msg[32];   /* human-readable last failure (CONN,FAIL / ERR / timeout) */
} WhConn;

typedef struct {
    Gui* gui;
    NotificationApp* notifications;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;

    Submenu* submenu;
    ByteInput* byte_input;
    TextInput* text_input;
    Widget* widget;
    VariableItemList* var_item_list;
    WhHuntView* hunt;
    WhSpectrumView* spectrum;

    WhSettings settings;

    WhUart* uart;
    Expansion* expansion;
    bool otg_enabled_by_us;
    bool board_alive;

    /* Live scan results, written by the UART worker as DEV lines stream in. */
    FuriMutex* results_mutex;
    WhTarget results[WH_MAX_RESULTS];
    size_t result_count;
    /* Pending DISCOVER_BEGINs that resume a scan: each keeps the results instead
     * of clearing them (re-sent MACs update in place, so rows stay matched to
     * their slot). A count, so back-to-back resumes can't lose one. */
    uint8_t results_keep_begins;

    /* IPs that arrived before their row (guarded by results_mutex); applied when
     * the row is created, cleared when the link drops. */
    WhPendingIp pending_ips[WH_MAX_RESULTS];
    size_t pending_ip_count;

    WhSearchKind search_kind;

    /* Channel selection for device search / MAC hunt. */
    WhChannelPurpose channel_purpose;
    uint8_t device_channel;   /* device search: 0 = hop all channels */
    bool picking_ap_channel;  /* AP list is open only to borrow a channel */
    bool channel_pick_pending; /* an AP was picked; ChannelSelect continues */

    WhTarget target; /* what the Hunt scene follows */
    WhFingerprint fp; /* result of the last/running fingerprint (results_mutex) */
    uint8_t mac_bytes[6]; /* ByteInput buffer, keeps the last entered MAC */
    bool from_saved_hunt; /* hunt was launched from a saved MAC: apply saved_hunt_mode */

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

    /* AP login + association ("Clients of nearby AP" can log in first). */
    int board_fw_version;      /* parsed from READY/PONG; 0 = unknown */
    WhConn conn;
    WhSavedAp saved_aps[WH_MAX_SAVED_AP];
    size_t saved_ap_count;
    char psk_buf[WH_AP_PSK_MAX]; /* TextInput buffer for an AP password */
    bool login_pending_save;     /* a freshly entered / WPS psk awaits a save choice */
    size_t ap_edit_index;        /* which saved_aps[] entry the Saved-APs menu manages */
    bool ap_password_edit;       /* password editor just updates the stored psk, no connect */
} WavehoundApp;

/** Send a line to the board (no-op without a UART). */
void wh_send(WavehoundApp* app, const char* line);

/** Send CONNECT for app->clients_ap using app->psk_buf (SSID/PSK hex-encoded). */
void wh_ap_connect(WavehoundApp* app);

/** Continue from the AP-login flow to the client list (collapses to ChannelSelect). */
void wh_ap_login_proceed(WavehoundApp* app);

/** Drop the AP link (if any) so a new search can hop/park freely. */
void wh_conn_drop(WavehoundApp* app);

/** Whether a fingerprint can be run on app->target now: associated, the target
 * is a station with a known IP, and the board firmware supports FP. */
bool wh_fingerprint_avail(WavehoundApp* app);

/** Reset app->fp for app->target and send FP,<mac> to begin a scan. */
void wh_fingerprint_start(WavehoundApp* app);

/** Parse "aa:bb:cc:dd:ee:ff" into bytes. Returns false on malformed input. */
bool wh_parse_mac(const char* str, uint8_t mac[6]);

/** Fill app->settings with defaults, then overlay any stored values. */
void wh_settings_load(WavehoundApp* app);

/** Persist app->settings to the SD card. */
void wh_settings_save(WavehoundApp* app);

/** Push the hunt-screen settings (sound, haptic, range, response) into the view. */
void wh_settings_apply(WavehoundApp* app);
