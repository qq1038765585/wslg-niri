#include "wsland/server.h"


struct wsc_monitor_change_data {
    struct server_client_task task;

    rdpMonitor monitors[WS_MAX_MONITOR];
    size_t count;

    struct wsland_client *client;
};

static void when_client_monitor_change(bool free_only, void *user_data, void *_) {
    struct wsc_monitor_change_data *change_data = wl_container_of(user_data, change_data, task);

    if (!free_only) {
        struct wsland_client *client = change_data->client;
        BOOL success = server_client_adjust_monitor_layout(client, change_data->count, change_data->monitors);

        if (success) {
            MONITOR_DEF *reset_monitor = malloc(sizeof(MONITOR_DEF) * change_data->count);
            for (uint32_t idx = 0; idx < change_data->count; ++idx) {
                rdpMonitor* m = &change_data->monitors[idx];

                reset_monitor[idx].left = m->x;
                reset_monitor[idx].top = m->y;
                reset_monitor[idx].right = m->width;
                reset_monitor[idx].bottom = m->height;
                reset_monitor[idx].flags = m->is_primary;
            }

            RDPGFX_RESET_GRAPHICS_PDU reset_graphics = {0};
            reset_graphics.monitorCount = change_data->count;
            reset_graphics.monitorDefArray = reset_monitor;
            reset_graphics.height = client->server->area.y2;
            reset_graphics.width = client->server->area.x2;
            change_data->client->rdpgfx_ctx->ResetGraphics(change_data->client->rdpgfx_ctx, &reset_graphics);
            free(reset_monitor);
        }
    }

    free(change_data);
}

static unsigned int disp_client_monitor_layout_change(DispServerContext *context, const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU *monitor_change) {
    struct wsland_client *client = context->custom;

    struct wsc_monitor_change_data *change_data = malloc(sizeof(*change_data));
    change_data->client = client;
    change_data->count = monitor_change->NumMonitors;

    for (uint32_t idx = 0; idx < change_data->count; idx++) {
        DISPLAY_CONTROL_MONITOR_LAYOUT *monitor = &monitor_change->Monitors[idx];

        change_data->monitors[idx] = (rdpMonitor) {
            .x = monitor->Left,
            .y = monitor->Top,
            .width = (int)monitor->Width,
            .height = (int)monitor->Height,
            .is_primary = (monitor->Flags & DISPLAY_CONTROL_MONITOR_PRIMARY) != 0,
            .attributes = (MONITOR_ATTRIBUTES) {
                .physicalWidth = monitor->PhysicalWidth,
                .physicalHeight = monitor->PhysicalHeight,
                .desktopScaleFactor = monitor->DesktopScaleFactor,
                .deviceScaleFactor = monitor->DeviceScaleFactor,
                .orientation = monitor->Orientation,
            },
            .orig_screen = 0,
        };
    }

    server_client_task_dispatch(client, when_client_monitor_change, &change_data->task);
    return CHANNEL_RC_OK;
}

bool server_client_disp_init(struct wsland_client *client) {
    DispServerContext *disp_ctx = disp_server_context_new(client->vcm);
    if (!disp_ctx) {
        return false;
    }
    client->disp_ctx = disp_ctx;

    disp_ctx->custom = client;
    disp_ctx->MaxNumMonitors = WS_MAX_MONITOR;
    disp_ctx->MaxMonitorAreaFactorA = DISPLAY_CONTROL_MAX_MONITOR_WIDTH;
    disp_ctx->MaxMonitorAreaFactorB = DISPLAY_CONTROL_MAX_MONITOR_HEIGHT;
    disp_ctx->DispMonitorLayout = disp_client_monitor_layout_change;
    if (disp_ctx->Open(disp_ctx) != CHANNEL_RC_OK) {
        return false;
    }

    if (disp_ctx->DisplayControlCaps(disp_ctx) != CHANNEL_RC_OK) {
        return false;
    }
    return true;
}