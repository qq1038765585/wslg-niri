#ifndef WSLAND_SERVER_H
#define WSLAND_SERVER_H

#include <stdatomic.h>
#include <wsland/wsland.h>
#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/server/disp.h>
#include <freerdp/server/rail.h>
#include <freerdp/server/audin.h>
#include <freerdp/server/rdpsnd.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/server/drdynvc.h>
#include <freerdp/server/cliprdr.h>
#include <freerdp/server/gfxredir.h>
#include <wayland-util.h>
#include <pixman.h>

#define WS_MAX_FDS 32
#define WS_MAX_KEYS 256
#define WS_MAX_MONITOR 16
#define WS_CLIPBOARD_MAX_FORMATS 5

#define WSC_WINDOW_MARKER_ID  0xFFFFFFFE
#define WSC_WINDOW_DESKTOP_ID 0xFFFFFFFF
#define WS_SHARED_MEMORY_NAME_SIZE (32 + 4 + 2)
#define WSC_ORDER_ICON_ROWLENGTH( W, BPP ) ((((W) * (BPP) + 31) / 32) * 4)

#define WSC_WINDOW_FULLSCREEN_STYLE (WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_GROUP | WS_TABSTOP)
#define WSC_WINDOW_NORMAL_STYLE (WSC_WINDOW_FULLSCREEN_STYLE | WS_THICKFRAME | WS_CAPTION)

#define DISPATCH(C, T, D, CB) { \
    struct server_client_task_data *_data = malloc(sizeof(*_data)); \
    _data->client = C; _data->T = *(D); \
    server_client_task_dispatch(C, CB, &_data->task); \
}

#define LOG(...) WLog_ERR("wsland", __VA_ARGS__)

struct wsc_monitor_change_data;
struct windows_provide_format_task;
struct windows_provide_format_response_task;
struct windows_sync_format_request_task;
struct wsc_clipboard_supported_format;

enum server_mask {
    SERVER_CLIENT_ACTIVATED = 1 << 0,
    SERVER_OUTPUT_ENABLED = 1 << 1,
};

struct wsland_monitor {
    int idx;
    bool primary;
    int phys_width, phys_height;
    int scale_desktop, scale_device;
    pixman_box32_t now, max, work, bar;
    struct wl_list link;
};

struct server_shared_memory {
    int fd;
    void *addr;
    size_t size;
    char name[WS_SHARED_MEMORY_NAME_SIZE + 1];
};

struct wsland_server {
    struct wsland_client *client;
    struct wl_list monitors;

    pixman_box32_t area;
    bool pressed[WS_MAX_KEYS];

    char *addr;
    uint16_t port;

    char *key_content;
    char *cert_content;

    bool enable_gfxredir;
    char *shared_memory_mount_point;
    size_t shared_memory_mount_point_size;

    bool enable_window_snap_arrange;
    bool enable_window_shadow_remoting;

    void *wsland;
    freerdp_listener *listener;
    const WslandCallback *callback;
};

struct wsland_client {
    rdpContext ctx;
    freerdp_peer *peer;

    HANDLE vcm;
    enum server_mask mask;

    RailServerContext *rail_ctx;
    DispServerContext *disp_ctx;
    RdpgfxServerContext *rdpgfx_ctx;
    DrdynvcServerContext *drdynvc_ctx;
    GfxRedirServerContext *gfxredir_ctx;
    CliprdrServerContext *clipboard_ctx;

    struct {
        atomic_int op_id;
        struct windows_provide_format_request_event *provide_flight;
        struct windows_sync_format_request_event *sync_flight;
    } clipboard;

    /*uint32_t clipboard_op_id;
    char *clipboard_mime_type;
    uint32_t clipboard_length;
    uint8_t *clipboard_data;
    int32_t clipboard_fd;*/

    atomic_int current_fid;
    atomic_int acknowledged_fid;
    int wid, sid, bid, pid;

    bool handshake_completed;
    bool activation_rail_completed;
    bool activation_graphics_completed;
    bool activation_graphics_redirection_completed;
    bool is_acknowledged_suspended;
    bool mouse_button_swap;

    struct wl_list dispatch_tasks;
    pthread_mutex_t dispatch_mutex;
    int dispatch_fd;

    struct client_audio_tx *audio_tx;
    struct client_audio_rx *audio_rx;

    struct {
        atomic_bool pending;
        struct server_shared_memory gfx_memory;
        void *gfx_buffer;
    } render;

    struct {
        struct wl_list output;

        struct wl_list motion;
        struct wl_list button;
        struct wl_list axis;

        struct wl_list input;
        struct wl_list reset;

        struct wl_list windows_provide;
        struct wl_list windows_sync;
    } event;

    pixman_box32_t current;
    struct wsland_monitor monitor;
    struct wsland_server *server;
};

struct audio_block_info {
    UINT64 submissionTime;
    UINT64 ackReceivedTime;
    UINT64 ackPlayedTime;
};

struct client_audio_tx {
    rdpsnd_server_context *tx_ctx;

    BOOL audioExitSignal;
    int pulseAudioSinkListenerFd;
    int pulseAudioSinkFd;
    pthread_t pulseAudioSinkThread;
    int bytesPerFrame;
    UINT audioBufferSize;
    BYTE* audioBuffer;
    BYTE lastBlockSent;
    UINT64 lastNetworkLatency;
    UINT64 accumulatedNetworkLatency;
    UINT accumulatedNetworkLatencyCount;
    UINT64 lastRenderedLatency;
    UINT64 accumulatedRenderedLatency;
    UINT accumulatedRenderedLatencyCount;
    struct audio_block_info blockInfo[256];
    int nextValidBlock;
    UINT PAVersion;
    int audioSem;
};

struct client_audio_rx {
    audin_server_context *rx_ctx;

    BOOL audioInExitSignal;
    int pulseAudioSourceListenerFd;
    int pulseAudioSourceFd;
    int closeAudioSourceFd;
    pthread_t pulseAudioSourceThread;
    BOOL isAudioInStreamOpened;
};

typedef void (*server_client_task_invoke)(bool free_only, void *data, void *state);

struct server_client_task {
    server_client_task_invoke invoke;
    struct wl_list link;

    struct wsland_client *client;
};

struct wsc_output_change_event {
    struct wl_list link;
};

struct wsc_keyboard_input_event {
    uint32_t key;
    bool release;

    struct wl_list link;
};

struct wsc_keyboard_reset_event {
    bool caps_lock;
    uint32_t keys[WS_MAX_KEYS];
    size_t count;

    struct wl_list link;
};

struct wsc_mouse_motion_event {
    double x, y;

    struct wl_list link;
};

struct wsc_mouse_button_event {
    uint32_t key;
    bool release;

    struct wl_list link;
};

struct wsc_mouse_axis_event {
    bool horizontal;
    int delta;

    struct wl_list link;
};

struct windows_provide_format_request_event {
    struct wsc_clipboard_supported_format *format;
    uint32_t op_id;
    int32_t fd;

    struct wl_list link;
};

struct windows_sync_format_request_event {
    struct wsc_clipboard_supported_format *format;
    int32_t read_fd, write_fd;

    struct wl_list link;
};

struct server_client_task_data {
    struct server_client_task task;

    union {
        RAIL_ACTIVATE_ORDER activate;
        RAIL_SYSPARAM_ORDER sysparam;
        RAIL_SYSCOMMAND_ORDER syscommand;
        RAIL_GET_APPID_REQ_ORDER appid;
    };

    struct wsland_client *client;
};

void server_client_event_handle(struct wsland_client *client, void *state);

void server_client_syscommand(bool free_only, void *user_data, void *_);
void server_client_rail_client_sysparam(bool free_only, void *user_data, void *_);
void server_client_rail_client_activate(bool free_only, void *user_data, void *_);
void server_client_appid_response(bool free_only, void *user_data, void *_);

void server_client_task_dispatch(struct wsland_client *client, server_client_task_invoke invoke, struct server_client_task *task);
void server_client_clipboard_source_destroy(struct wsland_client *client);
void server_client_audio_tx_destroy(struct wsland_client *client);
void server_client_audio_rx_destroy(struct wsland_client *client);

bool server_client_drdynvc_init(struct wsland_client *client);
bool server_client_gfxredir_init(struct wsland_client *client);
bool server_client_clipboard_init(struct wsland_client *client);
bool server_client_audiotx_init(struct wsland_client *client);
bool server_client_audiorx_init(struct wsland_client *client);
bool server_client_rdpgfx_init(struct wsland_client *client);
bool server_client_rail_init(struct wsland_client *client);
bool server_client_disp_init(struct wsland_client *client);

void server_client_window_create(struct wsland_client *client);
void server_client_window_update(struct wsland_client *client);
void server_client_buffer_resize(struct wsland_client *client);
void server_client_buffer_destroy(struct wsland_client *client);

BOOL server_client_adjust_monitor_layout(struct wsland_client *client, uint32_t count, rdpMonitor *monitors);
BOOL server_incoming_client(freerdp_listener *listener, freerdp_peer *peer);

bool server_shared_memory_new(struct wsland_server *server, struct server_shared_memory *shared_memory);
void server_shared_memory_free(struct server_shared_memory *shared_memory);
void server_shared_memory_config(struct wsland_server *server);

void server_generate_crypto(struct wsland_server *server);
#endif // WSLAND_SERVER_H