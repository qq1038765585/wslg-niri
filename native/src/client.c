#include <unistd.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <wayland-util.h>
#include <freerdp/channels/drdynvc.h>
#include <linux/input-event-codes.h>
#include "wsland/server.h"


static void server_client_channel_destroy(struct wsland_client *client) {
    if (client->rail_ctx) {
        client->rail_ctx->Stop(client->rail_ctx);

        rail_server_context_free(client->rail_ctx);
        client->rail_ctx = NULL;
    }
    if (client->disp_ctx) {
        client->disp_ctx->Close(client->disp_ctx);

        disp_server_context_free(client->disp_ctx);
        client->disp_ctx = NULL;
    }
    if (client->rdpgfx_ctx) {
        client->rdpgfx_ctx->Close(client->rdpgfx_ctx);

        rdpgfx_server_context_free(client->rdpgfx_ctx);
        client->activation_graphics_completed = false;
        client->rdpgfx_ctx = NULL;
    }
    if (client->clipboard_ctx) {
        server_client_clipboard_source_destroy(client);
    }
    if (client->gfxredir_ctx) {
        client->gfxredir_ctx->Close(client->gfxredir_ctx);

        gfxredir_server_context_free(client->gfxredir_ctx);
        client->activation_graphics_redirection_completed = false;
        client->gfxredir_ctx = NULL;
    }
    if (client->drdynvc_ctx) {
        client->drdynvc_ctx->Stop(client->drdynvc_ctx);

        drdynvc_server_context_free(client->drdynvc_ctx);
        client->drdynvc_ctx = NULL;
    }
    if (client->audio_tx) {
        server_client_audio_tx_destroy(client);
        client->audio_tx = NULL;
    }
    if (client->audio_rx) {
        server_client_audio_rx_destroy(client);
        client->audio_rx = NULL;
    }
}

static bool server_client_channel_init(struct wsland_client *client) {
    freerdp_settings_set_bool(client->peer->context->settings, FreeRDP_DesktopResize, FALSE);

    if (!server_client_drdynvc_init(client)) {
        goto failed;
    }

    if (!server_client_rail_init(client)) {
        goto failed;
    }

    if (!server_client_disp_init(client)) {
        goto failed;
    }

    if (!server_client_rdpgfx_init(client)) {
        goto failed;
    }

    if (client->server->enable_gfxredir) {
        if (!server_client_gfxredir_init(client)) {
            goto failed;
        }
    }

    if (!server_client_clipboard_init(client)) {
        goto failed;
    }

    if (!server_client_audiorx_init(client)) {
        goto failed;
    }

    if (!server_client_audiotx_init(client)) {
        goto failed;
    }

    { // wait graphics channel (and optionally graphics redir channel) response from client
        int waitRetry = 0;
        while (!client->activation_graphics_completed || (client->gfxredir_ctx && !client->activation_graphics_redirection_completed)) {
            if (++waitRetry > 10000) {
                return false;
            }
            usleep(10000);
            client->peer->CheckFileDescriptor(client->peer);
            WTSVirtualChannelManagerCheckFileDescriptor(client->vcm);
        }
    }

    client->current_fid = 0;
    client->acknowledged_fid = 0;
    return true;

failed:
    server_client_channel_destroy(client);
    return false;
}

static void server_client_sync_window_state(struct wsland_client *client) {
    freerdp_peer *peer = client->peer;

    {
        RAIL_SYSPARAM_ORDER sysParamOrder = {
            .param = SPI_SETSCREENSAVESECURE,
            .setScreenSaveSecure = 0,
        };
        client->rail_ctx->ServerSysparam(client->rail_ctx, &sysParamOrder);
        peer->DrainOutputBuffer(client->peer);
    }

    {
        RAIL_SYSPARAM_ORDER sysParamOrder = {
            .param = SPI_SETSCREENSAVEACTIVE,
            .setScreenSaveActive = 0,
        };
        client->rail_ctx->ServerSysparam(client->rail_ctx, &sysParamOrder);
        peer->DrainOutputBuffer(client->peer);
    }

    {
        RAIL_ZORDER_SYNC zOrderSync = {
            .windowIdMarker = WSC_WINDOW_MARKER_ID,
        };
        client->rail_ctx->ServerZOrderSync(client->rail_ctx, &zOrderSync);
        peer->DrainOutputBuffer(client->peer);
    }

    {
        WINDOW_ORDER_INFO window_order_info = {
            .windowId = WSC_WINDOW_MARKER_ID,
            .fieldFlags = WINDOW_ORDER_TYPE_DESKTOP | WINDOW_ORDER_FIELD_DESKTOP_HOOKED | WINDOW_ORDER_FIELD_DESKTOP_ARC_BEGAN,
        };

        MONITORED_DESKTOP_ORDER monitored_desktop_order = {0};
        peer->context->update->window->MonitoredDesktop(
            peer->context->update->context, &window_order_info, &monitored_desktop_order
        );
        peer->DrainOutputBuffer(client->peer);
    }

    {
        uint32_t windowsIdArray[1] = {0};
        WINDOW_ORDER_INFO window_order_info = {
            .windowId = WSC_WINDOW_MARKER_ID,
            .fieldFlags = WINDOW_ORDER_TYPE_DESKTOP | WINDOW_ORDER_FIELD_DESKTOP_ZORDER | WINDOW_ORDER_FIELD_DESKTOP_ACTIVE_WND,
        };
        windowsIdArray[0] = WSC_WINDOW_MARKER_ID;

        MONITORED_DESKTOP_ORDER monitored_desktop_order = {
            .activeWindowId = WSC_WINDOW_DESKTOP_ID,
            .numWindowIds = 1, .windowIds = (UINT *)&windowsIdArray,
        };

        peer->context->update->window->MonitoredDesktop(
            peer->context->update->context, &window_order_info, &monitored_desktop_order
        );
        peer->DrainOutputBuffer(client->peer);
    }

    {
        WINDOW_ORDER_INFO window_order_info = {
            .windowId = WSC_WINDOW_MARKER_ID,
            .fieldFlags = WINDOW_ORDER_TYPE_DESKTOP | WINDOW_ORDER_FIELD_DESKTOP_ARC_COMPLETED,
        };

        MONITORED_DESKTOP_ORDER monitored_desktop_order = {0};
        peer->context->update->window->MonitoredDesktop(
            peer->context->update->context, &window_order_info, &monitored_desktop_order
        );
        peer->DrainOutputBuffer(client->peer);
    }

    client->activation_rail_completed = true;
}

static BOOL xf_peer_adjust_monitor_layout(freerdp_peer *peer) {
    struct wsland_client *client = (struct wsland_client*)peer->context;
    rdpSettings *settings = client->peer->context->settings;

    int count = (int)freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount);
    if (count > WS_MAX_MONITOR) {
        return FALSE;
    }

    const rdpMonitor *rdp_monitor = freerdp_settings_get_pointer(settings, FreeRDP_MonitorDefArray);

    rdpMonitor *monitors;
    if (count > 0 && rdp_monitor) {
        monitors = malloc(sizeof(*monitors) * count);
        for (int idx = 0; idx < count; idx++) {
            monitors[idx] = rdp_monitor[idx];
            if (!freerdp_settings_get_bool(settings, FreeRDP_HasMonitorAttributes)) {
                monitors[idx].attributes.physicalWidth = 0;
                monitors[idx].attributes.physicalHeight = 0;
                monitors[idx].attributes.orientation = ORIENTATION_LANDSCAPE;
                monitors[idx].attributes.desktopScaleFactor = 100;
                monitors[idx].attributes.deviceScaleFactor = 100;
            }
        }
    } else {
        count = 1;
        monitors = malloc(sizeof(*monitors) * count);
        monitors[0].x = 0;
        monitors[0].y = 0;
        monitors[0].width = (int)freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
        monitors[0].height = (int)freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
        monitors[0].attributes.orientation = freerdp_settings_get_uint16(settings, FreeRDP_DesktopOrientation);
        monitors[0].attributes.physicalWidth = freerdp_settings_get_uint32(settings, FreeRDP_DesktopPhysicalWidth);
        monitors[0].attributes.physicalHeight = freerdp_settings_get_uint32(settings, FreeRDP_DesktopPhysicalHeight);
        monitors[0].attributes.desktopScaleFactor = freerdp_settings_get_uint32(settings, FreeRDP_DesktopScaleFactor);
        monitors[0].attributes.deviceScaleFactor = freerdp_settings_get_uint32(settings, FreeRDP_DeviceScaleFactor);
        monitors[0].orig_screen = 0;
        monitors[0].is_primary = 1;
    }

    BOOL success = server_client_adjust_monitor_layout(client, count, monitors);
    free(monitors);

    return success;
}

static BOOL xf_peer_capabilities(freerdp_peer *_) {
    return TRUE;
}

static BOOL xf_peer_post_connect(freerdp_peer *_) {
    return TRUE;
}

static BOOL xf_peer_activate(freerdp_peer *peer) {
    struct wsland_client *client = (struct wsland_client*)peer->context;
    rdpSettings *settings = peer->context->settings;

    if (!freerdp_settings_get_bool(settings, FreeRDP_SurfaceCommandsEnabled)) {
        return FALSE;
    }

    if (!freerdp_settings_get_bool(settings, FreeRDP_RemoteApplicationMode)) {
        return FALSE;
    }

    if (!server_client_channel_init(client)) {
        return FALSE;
    }

    server_client_sync_window_state(client);
    if (client->mask & SERVER_CLIENT_ACTIVATED) {
        return TRUE;
    }

    xf_peer_adjust_monitor_layout(peer);
    server_client_window_create(client);

    client->mask |= SERVER_CLIENT_ACTIVATED;
    return TRUE;
}

static BOOL xf_suppress_output(rdpContext *context, BYTE allow, const RECTANGLE_16 *area) {
    return TRUE;
}

static BOOL xf_input_synchronize_event(rdpInput *input, UINT32 flags) {
    struct wsland_client *client = (struct wsland_client*)input->context;

    struct wsc_keyboard_reset_event *event = calloc(1, sizeof(*event));
    event->count = 0;

    for (uint32_t idx = 0; idx < WS_MAX_KEYS; idx++) {
        if (client->server->pressed[idx]) {
            client->server->pressed[idx] = false;

            event->keys[event->count++] = idx;
        }
    }

    event->caps_lock = (flags & KBD_SYNC_CAPS_LOCK) != 0;
    wl_list_insert(&client->event.reset, &event->link);
    return TRUE;
}

static BOOL xf_input_mouse_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y) {
    struct wsland_client *client = (struct wsland_client*)input->context;

    if (flags & PTR_FLAGS_MOVE) {
        struct wsc_mouse_motion_event *event = calloc(1, sizeof(*event));
        event->x = x - client->monitor.now.x1 + client->server->area.x1;
        event->y = y - client->monitor.now.y1 + client->server->area.y1;
        wl_list_insert(&client->event.motion, &event->link);
        goto success;
    }

    int32_t button = 0;
    if (flags & PTR_FLAGS_BUTTON1) {
        button = BTN_LEFT;
    }
    else if (flags & PTR_FLAGS_BUTTON2) {
        button = BTN_RIGHT;
    }
    else if (flags & PTR_FLAGS_BUTTON3) {
        button = BTN_MIDDLE;
    }

    if (button) {
        struct wsc_mouse_button_event *event = calloc(1, sizeof(*event));
        event->key = button;
        event->release = flags & PTR_FLAGS_DOWN ? 0 : 1;
        wl_list_insert(&client->event.button, &event->link);
        goto success;
    }

    if (flags & PTR_FLAGS_WHEEL || flags & PTR_FLAGS_HWHEEL) {
        int32_t delta = flags & WheelRotationMask;
        if (flags & PTR_FLAGS_WHEEL_NEGATIVE) { delta = -(delta - PTR_FLAGS_WHEEL_NEGATIVE); }

        struct wsc_mouse_axis_event *event = calloc(1, sizeof(*event));
        event->delta = delta;
        event->horizontal = flags & PTR_FLAGS_WHEEL ? 0 : 1;
        wl_list_insert(&client->event.axis, &event->link);
    }

success:
    return TRUE;
}

static BOOL xf_input_extended_mouse_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y) {
    return TRUE;
}

static BOOL xf_input_keyboard_event(rdpInput *input, UINT16 flags, UINT8 code) {
    struct wsland_client *client = (struct wsland_client*)input->context;

    bool is_release = flags & KBD_FLAGS_RELEASE;
    bool is_extended = flags & KBD_FLAGS_EXTENDED;

    uint32_t full_code = code;
    if (is_extended) { full_code |= KBD_FLAGS_EXTENDED; }

    uint32_t scan_code = GetVirtualKeyCodeFromVirtualScanCode(full_code, 4);
    if (is_extended) { scan_code |= KBDEXT; }

    bool notify = false;
    uint32_t key_code = GetKeycodeFromVirtualKeyCode(scan_code, WINPR_KEYCODE_TYPE_XKB);

    if (is_release) {
        if (client->server->pressed[key_code]) {
            client->server->pressed[key_code] = false;
            notify = true;
        }
    } else {
        if (!client->server->pressed[key_code]) {
            client->server->pressed[key_code] = true;
            notify = true;
        }
    }

    if (notify) {
        struct wsc_keyboard_input_event *event = calloc(1, sizeof(*event));
        event->key = key_code;
        event->release = is_release;
        wl_list_insert(&client->event.input, &event->link);
    }

    return TRUE;
}

static BOOL xf_input_unicode_keyboard_event(rdpInput *input, UINT16 flags, UINT16 code) {
    return TRUE;
}

static int client_context_new(freerdp_peer *peer, struct wsland_client *client) {
    client->peer = peer;

    client->dispatch_fd = -1;
    wl_list_init(&client->dispatch_tasks);

    // todo set_selection for clipboard_listener
    return true;
}

static void client_context_free(freerdp_peer *_, struct wsland_client *client) {
    if (!client) {
        return;
    }

    client->mask &= ~SERVER_CLIENT_ACTIVATED;
    client->mask &= ~SERVER_OUTPUT_ENABLED;

    {
        struct server_client_task *task, *temp;
        wl_list_for_each_reverse_safe(task, temp, &client->dispatch_tasks, link) {
            wl_list_remove(&task->link);

            task->invoke(true, task, NULL);
        }
        pthread_mutex_destroy(&client->dispatch_mutex);

        if (client->dispatch_fd != -1) {
            close(client->dispatch_fd);
            client->dispatch_fd = -1;
        }
    }

    if (client->render.gfx_buffer) {
        server_client_buffer_destroy(client);
    }

    server_client_channel_destroy(client);
    client->server->client = NULL;
}

static int server_client_task_check(void *server_ptr, void *state) {
    struct wsland_server* server = server_ptr;
    if (!server->client) { return false; }

    struct wsland_client *client = server->client;

    eventfd_t dummy;
    eventfd_read(client->dispatch_fd, &dummy);

    struct server_client_task *task, *temp;
    pthread_mutex_lock(&client->dispatch_mutex);
    wl_list_for_each_reverse_safe(task, temp, &client->dispatch_tasks, link) {
        wl_list_remove(&task->link);
        break;
    }
    pthread_mutex_unlock(&client->dispatch_mutex);
    task->invoke(false, task, state);
    return true;
}

static bool server_client_task_init(struct wsland_client* client) {
    if (pthread_mutex_init(&client->dispatch_mutex, NULL) == -1) { // NOLINT(*-posix-return)
        goto mutex_failed;
    }

    client->dispatch_fd = eventfd(0, EFD_SEMAPHORE | EFD_CLOEXEC);
    if (client->dispatch_fd == -1) {
        goto dispatch_fd_error;
    }

    client->server->callback->dispatch_event(
        client->server->wsland, client->dispatch_fd, false, client->server, server_client_task_check
    );
    return true;

dispatch_fd_error:
    pthread_mutex_destroy(&client->dispatch_mutex);
mutex_failed:
    return false;
}

// ReSharper disable once CppDFAConstantFunctionResult
static int server_client_activity(void *server_ptr, void *state) {
    struct wsland_server *server = server_ptr;

    if (!server->client) {
        return false;
    }

    struct wsland_client *client = server->client;
    freerdp_peer *peer = server->client->peer;
    if (!peer->CheckFileDescriptor(peer)) {
        goto peer_release;
    }

    if (client->vcm) {
        if (!client->peer->activated || !WTSVirtualChannelManagerIsChannelJoined(client->vcm, DRDYNVC_SVC_CHANNEL_NAME)) {
            return true;
        }

        if (!WTSVirtualChannelManagerCheckFileDescriptor(client->vcm)) {
            goto peer_release;
        }
    }

    server_client_event_handle(client, state);
    return true;

peer_release:
    client->server->callback->output_destroy(state);

    freerdp_peer_context_free(peer);
    freerdp_peer_free(peer);
    return false;
}

extern PWtsApiFunctionTable FreeRDP_InitWtsApi(void);
static bool server_client_init(struct wsland_server *server, freerdp_peer *peer) {
    bool start = false;

    if (server->client) {
        return false;
    }

    peer->ContextSize = sizeof(struct wsland_client);
    peer->ContextNew = (psPeerContextNew)client_context_new;
    peer->ContextFree = (psPeerContextFree)client_context_free;
    freerdp_peer_context_new(peer);

    struct wsland_client *client = (struct wsland_client*)peer->context;
    client->server = server;

    rdpSettings *settings = peer->context->settings;
    if (server->key_content && server->cert_content) {
        rdpCertificate *cert = freerdp_certificate_new_from_pem(server->cert_content);
        rdpPrivateKey *key = freerdp_key_new_from_pem(server->key_content);

        if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1) ||
            !freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, cert, 1)) {
            freerdp_certificate_free(cert);
            freerdp_key_free(key);
            goto init_failed;
        }
    } else {
        freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, FALSE);
    }
    freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_ExtSecurity, FALSE);

    if (!peer->Initialize(peer)) {
        goto init_failed;
    }
    start = true;

    freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
    freerdp_settings_set_bool(settings, FreeRDP_NSCodec, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, FALSE);
    freerdp_settings_set_uint32(settings, FreeRDP_OsMajorType, OSMAJORTYPE_UNIX);
    freerdp_settings_set_uint32(settings, FreeRDP_OsMinorType, OSMINORTYPE_PSEUDO_XSERVER);
    freerdp_settings_set_bool(settings, FreeRDP_FrameMarkerCommandEnabled, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_SurfaceFrameMarkerEnabled, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_RefreshRect, TRUE);
    freerdp_settings_set_uint32(settings, FreeRDP_RemoteApplicationSupportLevel, RAIL_LEVEL_SUPPORTED |
        RAIL_LEVEL_SHELL_INTEGRATION_SUPPORTED | RAIL_LEVEL_LANGUAGE_IME_SYNC_SUPPORTED |
        RAIL_LEVEL_SERVER_TO_CLIENT_IME_SYNC_SUPPORTED | RAIL_LEVEL_HANDSHAKE_EX_SUPPORTED);
    freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_SupportMonitorLayoutPdu, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_SupportErrorInfoPdu, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_HasExtendedMouseEvent, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_HasHorizontalWheel, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_FastPathInput, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_AudioCapture, TRUE);

    freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_SupportMultitransport, FALSE);
    freerdp_settings_set_uint32(settings, FreeRDP_MultitransportFlags, 0);

    peer->AdjustMonitorsLayout = xf_peer_adjust_monitor_layout;
    peer->Capabilities = xf_peer_capabilities;
    peer->PostConnect = xf_peer_post_connect;
    peer->Activate = xf_peer_activate;

    rdpUpdate *update = peer->context->update;
    update->SuppressOutput = xf_suppress_output;

    rdpInput *input = peer->context->input;
    input->SynchronizeEvent = xf_input_synchronize_event;
    input->MouseEvent = xf_input_mouse_event;
    input->ExtendedMouseEvent = xf_input_extended_mouse_event;
    input->KeyboardEvent = xf_input_keyboard_event;
    input->UnicodeKeyboardEvent = xf_input_unicode_keyboard_event;

    HANDLE handles[WS_MAX_FDS];
    int count = (int)peer->GetEventHandles(peer, handles, WS_MAX_FDS);
    if (!count) {
        goto init_failed;
    }

    WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi());
    client->vcm = WTSOpenServerA((LPSTR)client);
    if (client->vcm) {
        handles[count++] = WTSVirtualChannelManagerGetEventHandle(client->vcm);
    }

    for (int i = 0; i < count; ++i) {
        const int fd = GetEventFileDescriptor(handles[i]);
        server->callback->dispatch_event(server->wsland, fd, false, server, server_client_activity);
    }

    if (!server_client_task_init(client)) {
        goto init_failed;
    }

    if (!server->client) {
        server->client = client;
    }

    wl_list_init(&client->event.output);

    wl_list_init(&client->event.motion);
    wl_list_init(&client->event.button);
    wl_list_init(&client->event.axis);

    wl_list_init(&client->event.input);
    wl_list_init(&client->event.reset);

    wl_list_init(&client->event.windows_provide);
    wl_list_init(&client->event.windows_sync);
    return true;

init_failed:
    if (start) {
        peer->Close(peer);
    }
    return false;
}

BOOL server_incoming_client(freerdp_listener *listener, freerdp_peer *peer) {
    struct wsland_server *server = listener->param4;

    if (!server_client_init(server, peer)) {
        return FALSE;
    }
    return TRUE;
}