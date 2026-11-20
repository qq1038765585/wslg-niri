#ifndef WSLAND_API_H
#define WSLAND_API_H

// ReSharper disable once CppUnusedIncludeDirective
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct wsland_server WslandServer;

typedef int32_t (*DispatchEventInvoke)(void *data, void *state);

typedef struct commit_state {
    int32_t x, y, w, h;

    int32_t opaque;
    const void *data;
} CommitState;

typedef struct wsland_callback {
    void (*dispatch_event)(void *wsland, int32_t fd, bool write, void *data, DispatchEventInvoke invoke);

    void (*keyboard_key)(void *state, uint32_t evdev, bool release);
    void (*keyboard_reset)(void *state, const uint32_t *keys, size_t count, bool caps_lock);

    void (*mouse_motion)(void *state, double x, double y);
    void (*mouse_button)(void *state, uint32_t key, bool release);
    void (*mouse_axis)(void *state, bool horizontal, int32_t value);

    void (*output_create)(void *state, uint16_t width, uint16_t height);
    void (*output_destroy)(void *state);

    void(*windows_provide_format)(void *state, uint32_t op_id, const char* const* mime_types, size_t count);
    void(*windows_provide_format_data)(void *state, int32_t fd, const char *mime_type, const uint8_t *data, size_t count);
    bool (*windows_sync_format_request_write)(void *state, const char *mime_type, int32_t fd);
} WslandCallback;

void windows_sync_format(WslandServer *server, const char* const* mime_types, size_t count);
void windows_provide_format_request(WslandServer *server, uint32_t op_id, const char *mime_type, int32_t fd);

WslandServer * wsland_server_create(uint16_t port, const char *addr, const WslandCallback *callback, void *wsland);
void wsland_server_commit_pointer(WslandServer *server, const CommitState *damage);
void wsland_server_commit_output(WslandServer *server, const CommitState *damage);
void wsland_server_switch_next_monitor(WslandServer *server);
void wsland_server_destroy(WslandServer *server);
#endif // WSLAND_API_H
