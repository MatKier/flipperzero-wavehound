#include "../wavehound_i.h"

#include <stdio.h>

#define ITEM_SEARCH_AGAIN 0
/* Result i is submenu index i + 1. */
/* "~ Node on ch N" (logged in to a mesh): rejoin through a node on channel N. */
#define ITEM_NODE_BASE 1000

/* Scene state: FRESH clears the list and starts a new scan on enter; RESUME
 * keeps the results already found (set when drilling into a result's Details,
 * so returning does not throw them away). "Search again" always starts fresh. */
#define TARGET_LIST_FRESH 0
#define TARGET_LIST_RESUME 1

/* Rows already added to the submenu, so a tick only appends the newcomers and
 * never touches existing rows (the submenu keeps the user's highlight put). */
static size_t s_shown;
static size_t s_header_count; /* count last written to the header */
static uint8_t s_spin; /* animated scan indicator phase */
static uint32_t s_picked_item; /* submenu index of the last picked result */

/* The nodes of the picked AP's network: every AP in the scan with the same
 * (non-hidden) SSID and security, the picked one first. Same-name neighbours
 * slip in too; the header's AP count makes that visible. */
static void wh_scene_target_list_collect_nodes(WavehoundApp* app, const WhTarget* ap) {
    strlcpy(app->clients_bssids[0], ap->mac, sizeof(app->clients_bssids[0]));
    app->clients_bssid_ch[0] = ap->channel;
    app->clients_bssid_count = 1;
    app->clients_channels = ap->channel ? (uint16_t)(1u << ap->channel) : 0;
    if(!ap->name[0] || app->board_fw_version < WH_FW_MESH_MIN) return;

    furi_mutex_acquire(app->results_mutex, FuriWaitForever);
    for(size_t i = 0; i < app->result_count && app->clients_bssid_count < WH_CLIENTS_BSSID_MAX;
        i++) {
        const WhTarget* t = &app->results[i];
        if(!t->is_ap || strcmp(t->mac, ap->mac) == 0 || strcmp(t->name, ap->name) != 0 ||
           strcmp(t->security, ap->security) != 0)
            continue;
        app->clients_bssid_ch[app->clients_bssid_count] = t->channel;
        strlcpy(
            app->clients_bssids[app->clients_bssid_count++], t->mac, sizeof(app->clients_bssids[0]));
        if(t->channel) app->clients_channels |= (uint16_t)(1u << t->channel);
    }
    furi_mutex_release(app->results_mutex);
}

static void wh_scene_target_list_callback(void* context, uint32_t index) {
    WavehoundApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void format_label(const WhTarget* t, char* out, size_t size) {
    if(t->is_ap) {
        /* hidden networks have no SSID: show the BSSID */
        snprintf(out, size, "%d c%u %s", t->rssi, t->channel, t->name[0] ? t->name : t->mac);
    } else if(t->host[0]) {
        /* a resolved hostname is the most useful identity */
        snprintf(out, size, "%d c%u %s", t->rssi, t->channel, t->host);
    } else if(t->dtype[0]) {
        /* no name, but mDNS/SSDP said what it is; the IP (if known) trails it */
        snprintf(out, size, "%d c%u %s %s", t->rssi, t->channel, t->dtype, t->ip);
    } else if(t->ip[0]) {
        snprintf(out, size, "%d c%u %s", t->rssi, t->channel, t->ip);
    } else {
        /* '*' marks a randomized (privacy) MAC */
        snprintf(out, size, "%d c%u %s%s", t->rssi, t->channel, t->mac, t->randomized ? "*" : "");
    }
}

static const char* wh_scene_target_list_noun(WhSearchKind kind) {
    return kind == WhSearchAccessPoints ? "networks" :
           kind == WhSearchClients      ? "clients" :
                                          "devices";
}

/* Build the DISCOVER command: AP listens always hop; device listens may be
 * parked on one channel; client listens park on the chosen AP's channel, or
 * hop only its mesh nodes' channels when they differ. While logged in the board
 * can't leave the AP's channel, so it parks there. */
static void wh_scene_target_list_send_discover(WavehoundApp* app) {
    const WhSearchKind kind = app->search_kind;
    const bool aps = kind == WhSearchAccessPoints;
    const bool clients = kind == WhSearchClients;
    const uint8_t channel = aps ? 0 : clients ? app->clients_ap.channel : app->device_channel;

    const unsigned dwell = wh_settings_scan_dwell_ms(app->settings.scan_dwell);

    char cmd[224]; /* 8 BSSIDs + 8 channels stays under the board's 256 */
    if(clients) {
        int len = snprintf(
            cmd,
            sizeof(cmd),
            "DISCOVER,CLIENTS,%u,%s,DWELL=%u",
            channel,
            app->clients_ap.mac,
            dwell);
        /* The other nodes; clients_ap may be any of them after a node switch. */
        const char* also = ",ALSO=";
        for(uint8_t i = 0; i < app->clients_bssid_count; i++) {
            if(strcmp(app->clients_bssids[i], app->clients_ap.mac) == 0) continue;
            len += snprintf(cmd + len, sizeof(cmd) - len, "%s%s", also, app->clients_bssids[i]);
            also = "/";
        }
        const bool logged_in = app->conn.connected || app->conn.connecting;
        if(!logged_in && (app->clients_channels & ~(1u << channel))) {
            const char* sep = ",HOP=";
            for(unsigned ch = 1; ch <= WH_MAX_CHANNEL; ch++) {
                if(!(app->clients_channels & (1u << ch))) continue;
                len += snprintf(cmd + len, sizeof(cmd) - len, "%s%u", sep, ch);
                sep = "/";
            }
        }
    } else {
        snprintf(cmd, sizeof(cmd), "DISCOVER,%s,%u,DWELL=%u", aps ? "AP" : "STA", channel, dwell);
    }
    wh_send(app, cmd);
}

/* Header text: either the AP-picker prompt, or a live "scanning" count. */
static void wh_scene_target_list_set_header(WavehoundApp* app, size_t count) {
    char header[40];
    if(app->picking_ap_channel) {
        strlcpy(
            header,
            app->channel_purpose == WhChannelForDeviceSearch ? "Clients of which AP:" :
                                                               "Use channel of:",
            sizeof(header));
    } else if(!app->uart) {
        strlcpy(header, "USART busy!", sizeof(header));
    } else {
        /* A block bouncing across a bracketed field -- reads as a radar
         * scanning, even at zero found. Every frame holds the same glyphs
         * (one '#', three spaces), so the header width stays constant; the
         * proportional header font would otherwise make the line jitter as
         * the block moved. */
        static const char* const scan_frames[] = {
            "[#   ]", "[ #  ]", "[  # ]", "[   #]", "[  # ]", "[ #  ]"};
        const char* bar = scan_frames[s_spin % (sizeof(scan_frames) / sizeof(scan_frames[0]))];
        if(app->search_kind == WhSearchClients && app->clients_bssid_count > 1) {
            /* A mesh: say how many nodes, so a neighbour reusing the SSID shows. */
            snprintf(
                header, sizeof(header), "Scan %s %u on %u APs", bar, (unsigned)count,
                (unsigned)app->clients_bssid_count);
        } else {
            snprintf(
                header, sizeof(header), "Scan %s %u %s", bar, (unsigned)count,
                wh_scene_target_list_noun(app->search_kind));
        }
    }
    submenu_set_header(app->submenu, header);
}

/* Append submenu rows for results heard since s_shown; returns the live count.
 * Never rebuilds existing rows, so the user's highlight stays where it is. */
static size_t wh_scene_target_list_sync_rows(WavehoundApp* app) {
    char label[64];
    furi_mutex_acquire(app->results_mutex, FuriWaitForever);
    const size_t count = app->result_count;
    for(size_t i = 0; i < count; i++) {
        format_label(&app->results[i], label, sizeof(label));
        if(i < s_shown) {
            /* Existing row: refresh it once it has learned an IP/hostname (the
             * ARP sweep streams those in after the row was first added). */
            if(app->results[i].ip[0] || app->results[i].host[0]) {
                submenu_change_item_label(app->submenu, i + 1, label);
            }
        } else {
            submenu_add_item(app->submenu, label, i + 1, wh_scene_target_list_callback, app);
        }
    }
    s_shown = count;
    furi_mutex_release(app->results_mutex);
    return count;
}

/* Logged in, the board only hears the joined node's channel. Offer one row per
 * other channel a node of this network is on; picking it rejoins through that
 * node. Needs the password to rejoin (a WPS join may not have surfaced it). */
static void wh_scene_target_list_add_node_rows(WavehoundApp* app) {
    if(app->search_kind != WhSearchClients || !app->conn.connected) return;
    if(!app->psk_buf[0] && strcmp(app->clients_ap.security, "Open") != 0) return;
    const uint16_t others = app->clients_channels & ~(1u << app->clients_ap.channel);
    char label[24];
    for(unsigned ch = 1; ch <= WH_MAX_CHANNEL; ch++) {
        if(!(others & (1u << ch))) continue;
        snprintf(label, sizeof(label), "~ Node on ch %u", ch);
        submenu_add_item(
            app->submenu, label, ITEM_NODE_BASE + ch, wh_scene_target_list_callback, app);
    }
}

/* Rejoin the network through its first node on `ch`; the connect splash then
 * returns to a fresh client list on that channel. */
static void wh_scene_target_list_switch_node(WavehoundApp* app, uint8_t ch) {
    for(uint8_t i = 0; i < app->clients_bssid_count; i++) {
        if(app->clients_bssid_ch[i] != ch) continue;
        strlcpy(app->node_prev_mac, app->clients_ap.mac, sizeof(app->node_prev_mac));
        app->node_prev_ch = app->clients_ap.channel;
        app->node_switch_pending = true;
        strlcpy(app->clients_ap.mac, app->clients_bssids[i], sizeof(app->clients_ap.mac));
        app->clients_ap.channel = ch;
        app->login_pending_save = false; /* same password as the node we leave */
        wh_ap_connect(app);
        scene_manager_next_scene(app->scene_manager, WhSceneApConnect);
        return;
    }
}

/* Clear the list and (re)start a continuous scan. */
static void wh_scene_target_list_restart(WavehoundApp* app) {
    furi_mutex_acquire(app->results_mutex, FuriWaitForever);
    app->result_count = 0;
    app->results_keep_begins = 0;
    furi_mutex_release(app->results_mutex);
    s_shown = 0;
    s_header_count = 0;

    submenu_reset(app->submenu);
    submenu_add_item(
        app->submenu, "~ Search again", ITEM_SEARCH_AGAIN, wh_scene_target_list_callback, app);
    wh_scene_target_list_add_node_rows(app);
    wh_scene_target_list_set_header(app, 0);

    if(app->uart) wh_scene_target_list_send_discover(app);
}

/* Re-enter after drilling into a result: keep every row found so far and just
 * rebuild the submenu that on_exit tore down, then resume the board scan. */
static void wh_scene_target_list_resume(WavehoundApp* app) {
    s_shown = 0;
    submenu_reset(app->submenu);
    submenu_add_item(
        app->submenu, "~ Search again", ITEM_SEARCH_AGAIN, wh_scene_target_list_callback, app);
    wh_scene_target_list_add_node_rows(app);

    const size_t count = wh_scene_target_list_sync_rows(app);
    wh_scene_target_list_set_header(app, count);
    s_header_count = count;

    if(app->uart) {
        /* The board answers DISCOVER with DISCOVER_BEGIN, which would otherwise
         * clear the results these rows were just built from. */
        furi_mutex_acquire(app->results_mutex, FuriWaitForever);
        if(app->results_keep_begins < UINT8_MAX) app->results_keep_begins++;
        furi_mutex_release(app->results_mutex);
        wh_scene_target_list_send_discover(app);
    }
}

void wh_scene_target_list_on_enter(void* context) {
    WavehoundApp* app = context;
    const bool resume =
        scene_manager_get_scene_state(app->scene_manager, WhSceneTargetList) ==
        TARGET_LIST_RESUME;
    if(resume) {
        wh_scene_target_list_resume(app);
        submenu_set_selected_item(app->submenu, s_picked_item);
    } else {
        wh_scene_target_list_restart(app);
        submenu_set_selected_item(app->submenu, ITEM_SEARCH_AGAIN);
    }
    /* The next entry is fresh unless a pick arms RESUME again. */
    scene_manager_set_scene_state(app->scene_manager, WhSceneTargetList, TARGET_LIST_FRESH);
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewSubmenu);
}

bool wh_scene_target_list_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;

    if(event.type == SceneManagerEventTypeTick) {
        /* Append devices heard since the last tick; never rebuild, so the
         * highlight stays where the user left it. */
        const size_t count = wh_scene_target_list_sync_rows(app);
        /* While a live scan is on, refresh every tick so the sweep animates;
         * otherwise only rewrite the header when the count actually changes. */
        if(app->uart && !app->picking_ap_channel) {
            s_spin++;
            wh_scene_target_list_set_header(app, count);
        } else if(count != s_header_count) {
            wh_scene_target_list_set_header(app, count);
        }
        s_header_count = count;
        return false;
    }

    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == ITEM_SEARCH_AGAIN) {
        wh_scene_target_list_restart(app);
        return true;
    }
    if(event.event > ITEM_NODE_BASE && event.event <= ITEM_NODE_BASE + WH_MAX_CHANNEL) {
        wh_scene_target_list_switch_node(app, (uint8_t)(event.event - ITEM_NODE_BASE));
        return true;
    }

    WhTarget picked_target;
    bool picked = false;
    furi_mutex_acquire(app->results_mutex, FuriWaitForever);
    if(event.event - 1 < app->result_count) {
        picked_target = app->results[event.event - 1];
        picked = true;
    }
    furi_mutex_release(app->results_mutex);
    if(!picked) return false;

    if(app->picking_ap_channel) {
        app->picking_ap_channel = false;
        if(app->channel_purpose == WhChannelForDeviceSearch) {
            /* Device search scoped to this AP: list the stations associated with
             * it, on its channel. The next scan reads clients_ap. */
            app->clients_ap = picked_target;
            wh_scene_target_list_collect_nodes(app, &picked_target);
            app->device_search_clients = true;
            /* Offer to log in to the AP first (so a silent client can later be
             * elicited); only when the board firmware supports it. The login
             * flow continues to the client list via wh_ap_login_proceed(). */
            if(app->board_fw_version >= WH_FW_LOGIN_MIN) {
                scene_manager_next_scene(app->scene_manager, WhSceneApLogin);
                return true;
            }
        } else {
            /* MAC hunt only needs the AP's channel to start on. Not pinned: a
             * client that roams to a mesh node on another channel is re-found. */
            app->target.channel = picked_target.channel;
            app->target.pin_channel = false;
        }
        app->channel_pick_pending = true;
        scene_manager_search_and_switch_to_previous_scene(
            app->scene_manager, WhSceneChannelSelect);
        return true;
    }

    app->target = picked_target;
    /* A device search parked on a channel by the user keeps the hunt there too.
     * A client starts on its AP's channel but isn't pinned, so a roam to a mesh
     * node on another channel is re-found (the hunt scene pins while logged in). */
    app->target.pin_channel = app->search_kind == WhSearchDevices && app->device_channel != 0;

    /* Keep the found list when we come back from Details, and land back on the
     * row just picked. */
    s_picked_item = event.event;
    scene_manager_set_scene_state(app->scene_manager, WhSceneTargetList, TARGET_LIST_RESUME);

    /* Pre-fill manual entry with the last target. */
    wh_parse_mac(app->target.mac, app->mac_bytes);
    scene_manager_next_scene(app->scene_manager, WhSceneDetails);
    return true;
}

void wh_scene_target_list_on_exit(void* context) {
    WavehoundApp* app = context;
    /* Stop the board scanning whenever we leave (pick, back or restart handles
     * its own resend). Harmless once the board is idle. */
    wh_send(app, "STOP");
    submenu_reset(app->submenu);
}
