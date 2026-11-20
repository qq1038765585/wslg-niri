#include <fcntl.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <cairo/cairo.h>
#include <sys/eventfd.h>
#include "wsland/wsland.h"
#include "wsland/server.h"


static int window_id = 0, surface_id = 0, buffer_id = 0, pool_id = 0;

static bool box_empty(pixman_box32_t *box) {
    return box == NULL || box->x2 <=0 || box->y2 <= 0;
}

static bool box_equal(pixman_box32_t *a, pixman_box32_t *b) {
    if (box_empty(a)) { a = NULL; }
    if (box_empty(b)) { b = NULL; }
    if (a == NULL || b == NULL) { return a == b; }
    return a->x1 == b->x1 && a->y1 == b->y1 && a->x2 == b->x2 && a->y2 == b->y2;
}

static bool box_contains_box(pixman_box32_t *bigger, pixman_box32_t *smaller) {
    if (box_empty(bigger) || box_empty(smaller)) {
        return false;
    }

    return smaller->x1 >= bigger->x1
        && smaller->x1 + smaller->x2 <= bigger->x1 + bigger->x2
        && smaller->y1 >= bigger->y1
        && smaller->y1 + smaller->y2 <= bigger->y1 + bigger->y2;
}

BOOL server_client_adjust_monitor_layout(struct wsland_client *client, uint32_t count, rdpMonitor *monitors) {
    {
        struct wsland_monitor *wwm, *temp;
        wl_list_for_each_safe(wwm, temp, &client->server->monitors, link) {
            wl_list_remove(&wwm->link);
            free(wwm);
        }
    }

    pixman_region32_t r;
    pixman_region32_init(&r);
    for (uint32_t idx = 0; idx < count; idx++) {
        rdpMonitor *monitor = &monitors[idx];
        struct wsland_monitor *wwm = malloc(sizeof(*wwm));
        wwm->idx = (int)(idx + 1);
        wwm->primary = monitor->is_primary;
        wwm->scale_desktop = (int)monitor->attributes.desktopScaleFactor;
        wwm->scale_device = (int)monitor->attributes.deviceScaleFactor;
        wwm->phys_height = (int)monitor->attributes.physicalHeight;
        wwm->phys_width = (int)monitor->attributes.physicalWidth;
        wwm->max.y2 = monitor->height;
        wwm->max.x2 = monitor->width;
        wwm->max.y1 = monitor->y;
        wwm->max.x1 = monitor->x;
        wwm->now = wwm->max;

        wl_list_init(&wwm->link);
        wl_list_insert(&client->server->monitors, &wwm->link);
        pixman_region32_union_rect(&r, &r, wwm->max.x1, wwm->max.y1, wwm->max.x2, wwm->max.y2);
    }

    struct wsland_monitor *wwm, *temp;
    wl_list_for_each(temp, &client->server->monitors, link) {
        if (!wwm) {
            wwm = temp;
        }

        if (temp->primary) {
            wwm = temp;
            break;
        }
    }

    assert(wwm);
    client->monitor = *wwm;

    struct wsc_output_change_event *event = calloc(1, sizeof(*event));
    wl_list_insert(&client->event.output, &event->link);

    client->server->area = *pixman_region32_extents(&r);
    pixman_region32_fini(&r);
    return TRUE;
}

void server_client_event_handle(struct wsland_client *client, void *state) {
    if ((client->mask & SERVER_CLIENT_ACTIVATED) == 0) { return; }

    if (!wl_list_empty(&client->event.output)) {
        struct wsc_output_change_event *event, *temp;
        wl_list_for_each_reverse_safe(event, temp, &client->event.output, link) {
            wl_list_remove(&event->link);

            if (!box_equal(&client->current, &client->monitor.now)) {
                client->mask &= ~SERVER_OUTPUT_ENABLED;
                client->server->callback->output_destroy(state);

                client->current = client->monitor.now;
                client->server->callback->output_create(state, client->monitor.now.x2, client->monitor.now.y2);
                server_client_window_update(client);

                client->mask |= SERVER_OUTPUT_ENABLED;
            }
            free(event);
        }
    }

    if (!wl_list_empty(&client->event.motion)) {
        struct wsc_mouse_motion_event *event, *temp;
        wl_list_for_each_reverse_safe(event, temp, &client->event.motion, link) {
            wl_list_remove(&event->link);

            client->server->callback->mouse_motion(state, event->x, event->y);
            free(event);
        }
    }

    if (!wl_list_empty(&client->event.button)) {
        struct wsc_mouse_button_event *event, *temp;
        wl_list_for_each_reverse_safe(event, temp, &client->event.button, link) {
            wl_list_remove(&event->link);

            client->server->callback->mouse_button(state, event->key, event->release);
            free(event);
        }
    }

    if (!wl_list_empty(&client->event.axis)) {
        struct wsc_mouse_axis_event *event, *temp;
        wl_list_for_each_reverse_safe(event, temp, &client->event.axis, link) {
            wl_list_remove(&event->link);

            client->server->callback->mouse_axis(state, event->horizontal, event->delta);
            free(event);
        }
    }

    if (!wl_list_empty(&client->event.input)) {
        struct wsc_keyboard_input_event *event, *temp;
        wl_list_for_each_reverse_safe(event, temp, &client->event.input, link) {
            wl_list_remove(&event->link);

            client->server->callback->keyboard_key(state, event->key, event->release);
            free(event);
        }
    }

    if (!wl_list_empty(&client->event.reset)) {
        struct wsc_keyboard_reset_event *event, *temp;
        wl_list_for_each_reverse_safe(event, temp, &client->event.reset, link) {
            wl_list_remove(&event->link);

            client->server->callback->keyboard_reset(state, event->keys, event->count, event->caps_lock);
            free(event);
        }
    }
}

void server_client_syscommand(bool free_only, void *user_data, void *_) {
    struct server_client_task_data *data = wl_container_of(user_data, data, task);

    if (!free_only) {
        if (!free_only && (int)data->syscommand.windowId == data->client->wid) {
            switch (data->syscommand.command) {
            case SC_MINIMIZE:
                {
                    WINDOW_ORDER_INFO window_order_info = {0};
                    WINDOW_STATE_ORDER window_state_order = {0};

                    window_order_info.windowId = data->client->wid;
                    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW;

                    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW;
                    window_state_order.showState = WINDOW_SHOW_MINIMIZED;

                    rdpUpdate *update = data->client->peer->context->update;
                    update->BeginPaint(update->context);
                    update->window->WindowUpdate(update->context, &window_order_info, &window_state_order);
                    update->EndPaint(update->context);
                }
                break;
            case SC_MAXIMIZE:
                {
                    WINDOW_ORDER_INFO window_order_info = {0};
                    WINDOW_STATE_ORDER window_state_order = {0};

                    window_order_info.windowId = data->client->wid;
                    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW;

                    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW;
                    window_state_order.showState = WINDOW_SHOW_MAXIMIZED;

                    rdpUpdate *update = data->client->peer->context->update;
                    update->BeginPaint(update->context);
                    update->window->WindowUpdate(update->context, &window_order_info, &window_state_order);
                    update->EndPaint(update->context);
                }
                break;
            default:
                break;
            }
        }
    }
    free(data);
}

void server_client_rail_client_sysparam(bool free_only, void *user_data, void *_) {
    struct server_client_task_data *data = wl_container_of(user_data, data, task);
    struct wsland_client *client = data->client;

    if (!free_only) {
        uint32_t p = data->sysparam.params;
        bool is_work_area = p & SPI_MASK_SET_WORK_AREA;
        bool is_taskbar_pos = p & SPI_MASK_TASKBAR_POS;

        if (is_work_area || is_taskbar_pos) {
            RECTANGLE_16 rect = is_work_area ? data->sysparam.workArea : data->sysparam.taskbarPos;

            pixman_box32_t area = (pixman_box32_t){ .x1 = rect.left, .y1 = rect.top, .x2 = rect.right - rect.left, .y2 = rect.bottom - rect.top };

            struct wsland_monitor *wwm;
            wl_list_for_each(wwm, &client->server->monitors, link) {
                if (box_contains_box(&wwm->max, &area)) {
                    if (is_work_area) {
                        wwm->now = area;

                        if (wwm->idx == client->monitor.idx) {
                            client->monitor = *wwm;

                            struct wsc_output_change_event *event = calloc(1, sizeof(*event));
                            wl_list_insert(&client->event.output, &event->link);
                        }
                    } else {
                        wwm->bar = area;
                    }
                    break;
                }
            }
        }
    }
    free(data);
}

void server_client_rail_client_activate(bool free_only, void *user_data, void *_) {
    struct server_client_task_data *data = wl_container_of(user_data, data, task);

    if (!free_only && (int)data->activate.windowId == data->client->wid) {
        if (data->activate.enabled) {
            WINDOW_ORDER_INFO window_order_info = {0};
            WINDOW_STATE_ORDER window_state_order = {0};

            window_order_info.windowId = data->client->wid;
            window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW;

            window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW;
            window_state_order.showState = WINDOW_SHOW;

            rdpUpdate *update = data->client->peer->context->update;
            update->BeginPaint(update->context);
            update->window->WindowUpdate(update->context, &window_order_info, &window_state_order);
            update->EndPaint(update->context);
        }
    }
    free(data);
}

void server_client_appid_response(bool free_only, void *user_data, void *_) {
    struct server_client_task_data *data = wl_container_of(user_data, data, task);


    if (!free_only && (int)data->appid.windowId == data->client->wid) {
        RAIL_GET_APPID_RESP_EX resp = {0};
        resp.windowID = data->client->wid;

        char buffer[256];
        snprintf(buffer, sizeof(buffer), "niri-%d", (int)getpid());

        size_t wchar_len = 0;
        WCHAR *wchar_data = ConvertUtf8NToWCharAlloc(buffer, strlen(buffer), &wchar_len);
        memcpy(resp.applicationID, wchar_data, wchar_len);

        RailServerContext *rail_ctx = data->client->rail_ctx;
        rail_ctx->ServerGetAppidRespEx(rail_ctx, &resp);
        free(wchar_data);
    }
    free(data);
}

void server_client_task_dispatch(struct wsland_client *client, server_client_task_invoke invoke, struct server_client_task *task) {
    task->client = client;
    task->invoke = invoke;

    pthread_mutex_lock(&client->dispatch_mutex);
    wl_list_insert(&client->dispatch_tasks, &task->link);
    pthread_mutex_unlock(&client->dispatch_mutex);

    eventfd_write(client->dispatch_fd, 1);
}

static cairo_status_t fetch_icon_data_func(void *closure, unsigned char *data, unsigned int length) {
    FILE *fp = closure;

    size_t n = fread(data, 1, length, fp);
    return n != length ? CAIRO_STATUS_READ_ERROR : CAIRO_STATUS_SUCCESS;
}

static void server_client_icon_sync(struct wsland_client *client) {
    FILE *fetch_icon_data = fopen("/usr/share/icons/wsl/linux.png", "r");
    if (!fetch_icon_data) { return; }

    cairo_surface_t *source = cairo_image_surface_create_from_png_stream(fetch_icon_data_func, fetch_icon_data);
    if (cairo_surface_status(source) == CAIRO_STATUS_SUCCESS) {
        int width = cairo_image_surface_get_width(source);
        int height = cairo_image_surface_get_height(source);

        if (width > 0 && height > 0) {
            double tw = 96.0, th = 96.0;
            bool bits_color_allocated = false;
            void *bits_color = NULL, *bits_mask = NULL;
            cairo_surface_t *target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)tw, (int)th);
            cairo_t *cr = cairo_create(target);

            cairo_translate(cr, tw, th);
            cairo_rotate(cr, M_PI);

            cairo_scale(cr, tw / width, th / height);
            cairo_set_source_surface(cr, source, 0, 0);

            cairo_paint(cr);
            cairo_destroy(cr);

            int w = cairo_image_surface_get_width(target);
            int h = cairo_image_surface_get_height(target);
            int s = cairo_image_surface_get_stride(target);

            int stride_color = WSC_ORDER_ICON_ROWLENGTH(w, 32);
            int size_color = stride_color * h;

            if (stride_color != s) {
                size_color = stride_color * h;
                bits_color = malloc(size_color);
                bits_color_allocated = true;
            } else {
                bits_color = cairo_image_surface_get_data(target);
            }

            int stride_mask = WSC_ORDER_ICON_ROWLENGTH(w, 1);
            int size_mask = stride_mask * h;

            bits_mask = malloc(size_mask);
            char *src_color = (char *)cairo_image_surface_get_data(target);
            char *dst_color = bits_color;
            char *dst_mask = bits_mask;

            for (int i = 0; i < h; i++) {
                uint32_t *src = (uint32_t *)src_color;
                uint32_t *dst = (uint32_t *)dst_color;
                char *mask = dst_mask;

                for (int j = 0; j < w; j++) {
                    if (dst != src) {
                        *dst = *src;
                    }
                    if (*dst & 0xFF000000) {
                        mask[j / 8] |= 0x80 >> (j % 8); // NOLINT(*-narrowing-conversions)
                    }
                    dst++;
                    src++;
                }
                dst_color += stride_color;
                dst_mask += stride_mask;
                src_color += s;
            }


            WINDOW_ORDER_INFO order_info = {0};
            WINDOW_ICON_ORDER icon_order = {0};
            ICON_INFO icon_info = {0};

            order_info.windowId = client->wid;
            order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_ICON;
            icon_info.cacheEntry = 0xFFFF;
            icon_info.cacheId = 0xFF;
            icon_info.bpp = 32;
            icon_info.width = w;
            icon_info.height = h;
            icon_info.cbColorTable = 0;
            icon_info.cbBitsMask = size_mask;
            icon_info.cbBitsColor = size_color;
            icon_info.bitsMask = bits_mask;
            icon_info.colorTable = NULL;
            icon_info.bitsColor = bits_color;
            icon_order.iconInfo = &icon_info;

            rdpUpdate *update = client->peer->context->update;
            update->BeginPaint(update->context);
            update->window->WindowIcon(update->context, &order_info, &icon_order);
            update->EndPaint(update->context);

            free(bits_mask);
            if (bits_color_allocated) { free(bits_color); }
            cairo_surface_destroy(target);
        }
    }
    cairo_surface_destroy(source);
    pclose(fetch_icon_data);
}

void server_client_window_create(struct wsland_client *client) {
    if (!client->activation_rail_completed || !client->activation_graphics_completed) {
        LOG("rail or graphics channel not ready!");
        return;
    }

    WINDOW_ORDER_INFO window_order_info = {0};
    WINDOW_STATE_ORDER window_state_order = {0};

    client->wid = ++window_id;
    window_order_info.windowId = client->wid;
    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_STATE_NEW;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_STYLE;
    window_state_order.style = WSC_WINDOW_NORMAL_STYLE;
    window_state_order.extendedStyle = WS_EX_LAYERED;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_OWNER;
    window_state_order.ownerWindowId = WSC_WINDOW_DESKTOP_ID;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_OFFSET;
    window_state_order.clientOffsetX = 0;
    window_state_order.clientOffsetY = 0;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_CLIENT_DELTA;
    window_state_order.windowClientDeltaX = 0;
    window_state_order.windowClientDeltaY = 0;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VIS_OFFSET;
    window_state_order.visibleOffsetX = 0;
    window_state_order.visibleOffsetY = 0;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_OFFSET;
    window_state_order.windowOffsetX = client->monitor.now.x1;
    window_state_order.windowOffsetY = client->monitor.now.y1;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW | WINDOW_ORDER_FIELD_TASKBAR_BUTTON;
    window_state_order.showState = WINDOW_HIDE;
    window_state_order.TaskbarButton = true;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE;
    window_state_order.clientAreaWidth = client->monitor.now.x2;
    window_state_order.clientAreaHeight = client->monitor.now.y2;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_SIZE;
    window_state_order.windowWidth = client->monitor.now.x2;
    window_state_order.windowHeight = client->monitor.now.y2;

    RECTANGLE_16 window_rect = {
        .left = 0,
        .top = 0,
        .right = client->monitor.now.x2,
        .bottom = client->monitor.now.y2
    };

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_RECTS;
    window_state_order.numWindowRects = 1;
    window_state_order.windowRects = &window_rect;

    RECTANGLE_16 window_vis = {
        .left = 0,
         .top = 0,
         .right = client->monitor.now.x2,
         .bottom = client->monitor.now.y2,
    };

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VISIBILITY;
    window_state_order.numVisibilityRects = 1;
    window_state_order.visibilityRects = &window_vis;

    RAIL_UNICODE_STRING backend_window_title = {.length = 0, .string = NULL};
    if (utf8_string_to_rail_string("niri", &backend_window_title)) {
        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_TITLE;
        window_state_order.titleInfo = backend_window_title;
    }

    rdpUpdate *update = client->peer->context->update;
    update->BeginPaint(update->context);
    update->window->WindowCreate(update->context, &window_order_info, &window_state_order);
    update->EndPaint(update->context);

    if (backend_window_title.string) {
        free(backend_window_title.string);
    }

    server_client_icon_sync(client);

    {
        RAIL_MINMAXINFO_ORDER minmax_order = {0};
        minmax_order.windowId = client->wid;
        minmax_order.maxPosX = 0;
        minmax_order.maxPosY = 0;
        minmax_order.maxWidth = (int16_t)client->monitor.now.x2;
        minmax_order.maxHeight = (int16_t)client->monitor.now.y2;
        minmax_order.maxTrackWidth = (int16_t)client->monitor.now.x2;
        minmax_order.maxTrackHeight = (int16_t)client->monitor.now.y2;
        minmax_order.minTrackHeight = 0;
        minmax_order.minTrackWidth = 0;
        client->rail_ctx->ServerMinMaxInfo(client->rail_ctx, &minmax_order);
    }
}

void server_client_window_update(struct wsland_client *client) {
    if (!client->wid) {
        LOG("window not create!");
        return;
    }

    WINDOW_ORDER_INFO window_order_info = {0};
    WINDOW_STATE_ORDER window_state_order = {0};

    window_order_info.windowId = client->wid;
    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW | WINDOW_ORDER_FIELD_TASKBAR_BUTTON;
    window_state_order.showState = WINDOW_SHOW;
    window_state_order.TaskbarButton = false;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_OFFSET;
    window_state_order.windowOffsetX = client->monitor.now.x1;
    window_state_order.windowOffsetY = client->monitor.now.y1;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE;
    window_state_order.clientAreaHeight = client->monitor.now.y2;
    window_state_order.clientAreaWidth = client->monitor.now.x2;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_SIZE;
    window_state_order.windowHeight = client->monitor.now.y2;
    window_state_order.windowWidth = client->monitor.now.x2;

    RECTANGLE_16 window_rect = {
        .left = 0,
        .top = 0,
        .right = client->monitor.now.x2,
        .bottom = client->monitor.now.y2
    };

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_RECTS;
    window_state_order.numWindowRects = 1;
    window_state_order.windowRects = &window_rect;

    RECTANGLE_16 window_vis = {
        .left = 0,
         .top = 0,
         .right = client->monitor.now.x2,
         .bottom = client->monitor.now.y2
    };

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VISIBILITY;
    window_state_order.numVisibilityRects = 1;
    window_state_order.visibilityRects = &window_vis;

    rdpUpdate *update = client->peer->context->update;
    update->BeginPaint(update->context);
    update->window->WindowUpdate(update->context, &window_order_info, &window_state_order);
    update->EndPaint(update->context);

    server_client_buffer_resize(client);
}

static void wait_render_pending_complete(struct wsland_client *client) {
    int waitRetry = 0;
    while (client->render.pending || client->current_fid != client->acknowledged_fid) {
        if (++waitRetry > 1000) {
            break;
        }
        usleep(10000);
        client->peer->CheckFileDescriptor(client->peer);
        WTSVirtualChannelManagerCheckFileDescriptor(client->vcm);
    }
}

void server_client_buffer_resize(struct wsland_client *client) {
    if (!client->wid || box_empty(&client->monitor.now)) {
        return;
    }

    wait_render_pending_complete(client);
    if (client->server->enable_gfxredir) {
        if (client->render.gfx_buffer) {

            server_client_buffer_destroy(client);
            client->render.gfx_memory.name[0] = '\0';
        }

        int buffer_bpp = 4;
        int width = client->monitor.now.x2;
        int height = client->monitor.now.y2;
        long page_size = sysconf(_SC_PAGESIZE);
        client->render.gfx_memory.size = width * height * buffer_bpp + (page_size - 1) & ~(page_size - 1);
        if (server_shared_memory_new(client->server, &client->render.gfx_memory)) {
            uint16_t section_name[WS_SHARED_MEMORY_NAME_SIZE + 1];

            for (uint32_t idx = 0; idx < WS_SHARED_MEMORY_NAME_SIZE; idx++) {
                section_name[idx] = (uint16_t)client->render.gfx_memory.name[idx];
            }
            section_name[WS_SHARED_MEMORY_NAME_SIZE] = 0;

            GFXREDIR_OPEN_POOL_PDU open_pool_pdu = {0};
            open_pool_pdu.poolId = ++pool_id;
            open_pool_pdu.poolSize = client->render.gfx_memory.size;
            open_pool_pdu.sectionNameLength = WS_SHARED_MEMORY_NAME_SIZE + 1;
            open_pool_pdu.sectionName = section_name;

            if (client->gfxredir_ctx->OpenPool(client->gfxredir_ctx, &open_pool_pdu) == 0) {
                GFXREDIR_CREATE_BUFFER_PDU create_buffer_pdu = {0};
                create_buffer_pdu.poolId = open_pool_pdu.poolId;
                create_buffer_pdu.bufferId = ++buffer_id;
                create_buffer_pdu.stride = width * buffer_bpp;
                create_buffer_pdu.height = height;
                create_buffer_pdu.width = width;
                create_buffer_pdu.offset = 0;
                create_buffer_pdu.format = GFXREDIR_BUFFER_PIXEL_FORMAT_ARGB_8888;
                if (client->gfxredir_ctx->CreateBuffer(client->gfxredir_ctx, &create_buffer_pdu) == 0) {
                    client->render.gfx_buffer = client->render.gfx_memory.addr;
                    client->bid = (int)create_buffer_pdu.bufferId;
                    client->pid = (int)create_buffer_pdu.poolId;
                }
            }

            if (!client->render.gfx_buffer) {
                server_client_buffer_destroy(client);
            }
        }
    } else {
        uint32_t prev_surface_id = client->sid;

        RDPGFX_CREATE_SURFACE_PDU create_surface = {0};
        create_surface.surfaceId = ++surface_id;
        create_surface.height = client->monitor.now.y2;
        create_surface.width = client->monitor.now.x2;
        create_surface.pixelFormat = GFX_PIXEL_FORMAT_ARGB_8888;
        if (client->rdpgfx_ctx->CreateSurface(client->rdpgfx_ctx, &create_surface) == 0) {
            client->sid = create_surface.surfaceId;
        }

        RDPGFX_MAP_SURFACE_TO_SCALED_WINDOW_PDU map_surface_to_window = {0};
        map_surface_to_window.windowId = client->wid;
        map_surface_to_window.surfaceId = client->sid;
        map_surface_to_window.mappedWidth = client->monitor.now.x2;
        map_surface_to_window.mappedHeight = client->monitor.now.y2;
        map_surface_to_window.targetWidth = client->monitor.now.x2;
        map_surface_to_window.targetHeight = client->monitor.now.y2;
        client->rdpgfx_ctx->MapSurfaceToScaledWindow(client->rdpgfx_ctx, &map_surface_to_window);

        if (prev_surface_id) {
            RDPGFX_DELETE_SURFACE_PDU deleteSurface = {0};
            deleteSurface.surfaceId = (uint16_t)prev_surface_id;
            client->rdpgfx_ctx->DeleteSurface(client->rdpgfx_ctx, &deleteSurface);
        }
    }
}

void server_client_buffer_destroy(struct wsland_client *client) {
    GfxRedirServerContext *redir_ctx = client->gfxredir_ctx;

    if (client->bid) {
        GFXREDIR_DESTROY_BUFFER_PDU destroy_buffer_pdu = {0};
        destroy_buffer_pdu.bufferId = client->bid;
        redir_ctx->DestroyBuffer(redir_ctx, &destroy_buffer_pdu);
        client->bid = 0;
    }

    if (client->pid) {
        GFXREDIR_CLOSE_POOL_PDU close_pool_pdu = {0};
        close_pool_pdu.poolId = client->pid;
        redir_ctx->ClosePool(redir_ctx, &close_pool_pdu);
        client->pid = 0;
    }

    server_shared_memory_free(&client->render.gfx_memory);
    client->render.gfx_buffer = NULL;
}

void wsland_server_commit_pointer(WslandServer *server, const CommitState *damage) {
    if (!server->client) { return; }
    struct wsland_client *client = server->client;

    int cursor_bpp = 4;
    int cursor_stride = damage->w * cursor_bpp;
    int cursor_size = cursor_stride * damage->h;

    POINTER_LARGE_UPDATE pointerUpdate = {0};
    pointerUpdate.xorBpp = cursor_bpp * 8;
    pointerUpdate.cacheIndex = 0;
    pointerUpdate.hotSpotX = damage->x;
    pointerUpdate.hotSpotY = damage->y;
    pointerUpdate.height = damage->h,
    pointerUpdate.width = damage->w,
    pointerUpdate.lengthXorMask = cursor_size;
    pointerUpdate.xorMaskData = (uint8_t *)damage->data;
    pointerUpdate.lengthAndMask = 0;
    pointerUpdate.andMaskData = NULL;

    rdpUpdate *update = client->peer->context->update;
    update->BeginPaint(update->context);
    update->pointer->PointerLarge(update->context, &pointerUpdate);
    update->EndPaint(update->context);
}

void wsland_server_commit_output(WslandServer *server, const CommitState *damage) {
    if (!server->client || !server->client->wid || (server->client->mask & SERVER_OUTPUT_ENABLED) == 0) { return; }
    struct wsland_client *client = server->client;
    RDPGFX_START_FRAME_PDU start_frame = {0};
    bool buffer_opaque = damage->opaque;

    uint32_t damage_bpp = 4;
    if (client->server->enable_gfxredir) {
        if (!client->render.gfx_buffer) {
            goto render_release;
        }

        uint32_t tw = client->monitor.now.x2;
        uint32_t th = client->monitor.now.y2;
        size_t output_stride = tw * damage_bpp;
        size_t damage_stride = damage->w * damage_bpp;
        for (int row = 0; row < damage->h; row++) {
            const uint8_t *src = (uint8_t *)damage->data + (size_t)row * damage_stride;
            uint8_t *dst = (uint8_t *)client->render.gfx_buffer + (size_t)(damage->y + row) * output_stride + (size_t)damage->x * damage_bpp;
            memcpy(dst, src, damage_stride);
        }

        GFXREDIR_PRESENT_BUFFER_PDU present_buffer_pdu = {0};
        present_buffer_pdu.timestamp = 0;
        present_buffer_pdu.orientation = 0;
        present_buffer_pdu.windowId = client->wid;
        present_buffer_pdu.bufferId = client->bid;
        present_buffer_pdu.presentId = ++client->current_fid;
        present_buffer_pdu.targetHeight = th;
        present_buffer_pdu.targetWidth = tw;
        present_buffer_pdu.dirtyRect.left = damage->x;
        present_buffer_pdu.dirtyRect.top = damage->y;
        present_buffer_pdu.dirtyRect.width = damage->w;
        present_buffer_pdu.dirtyRect.height = damage->h;
        if (buffer_opaque) {
            RECTANGLE_32 opaque_rect = {
                .left = damage->x, .top = damage->y,
                .width = damage->w, .height = damage->h,
            };
            present_buffer_pdu.numOpaqueRects = 1;
            present_buffer_pdu.opaqueRects = &opaque_rect;
        } else {
            present_buffer_pdu.numOpaqueRects = 0;
            present_buffer_pdu.opaqueRects = NULL;
        }
        if (client->gfxredir_ctx->PresentBuffer(client->gfxredir_ctx, &present_buffer_pdu) == 0) {
            client->render.pending = true;
        }

        goto render_release;
    }

    if (!start_frame.frameId) {
        start_frame.frameId = ++client->current_fid;
        client->rdpgfx_ctx->StartFrame(client->rdpgfx_ctx, &start_frame);
    }

    RDPGFX_SURFACE_COMMAND surface_command = {0};
    surface_command.surfaceId = client->sid;
    surface_command.format = PIXEL_FORMAT_BGRA32;
    surface_command.left = damage->x;
    surface_command.top = damage->y;
    surface_command.right = damage->x + damage->w;
    surface_command.bottom = damage->y + damage->h;
    surface_command.height = damage->h;
    surface_command.width = damage->w;
    surface_command.contextId = 0;
    surface_command.extra = NULL;

    surface_command.codecId = RDPGFX_CODECID_UNCOMPRESSED;
    surface_command.length = damage->w * damage->h * damage_bpp;
    surface_command.data = &((uint8_t *)damage->data)[0];
    client->rdpgfx_ctx->SurfaceCommand(client->rdpgfx_ctx, &surface_command);

render_release:
    if (start_frame.frameId) {
        RdpgfxServerContext *gfx_ctx = client->rdpgfx_ctx;
        RDPGFX_END_FRAME_PDU endFrame = { .frameId = start_frame.frameId };
        gfx_ctx->EndFrame(gfx_ctx, &endFrame);
    }
}

bool server_shared_memory_new(struct wsland_server *server, struct server_shared_memory *shared_memory) {
    int fd = -1;
    void *addr = NULL;
    char path[server->shared_memory_mount_point_size + 1 + WS_SHARED_MEMORY_NAME_SIZE + 1];

    if (shared_memory->size <= 0) {
        goto release;
    }

    if (shared_memory->name[0] == '\0') {
        const int fd_uuid = open("/proc/sys/kernel/random/uuid", O_RDONLY);

        if (fd_uuid < 0) {
            goto release;
        }

        if (read(fd_uuid, &shared_memory->name[1], 32 + 4) < 0) {
            goto release;
        }

        close(fd_uuid);
        shared_memory->name[0] = '{';
        shared_memory->name[WS_SHARED_MEMORY_NAME_SIZE - 1] = '}';
        shared_memory->name[WS_SHARED_MEMORY_NAME_SIZE] = '\0';
    } else if (strlen(shared_memory->name) != WS_SHARED_MEMORY_NAME_SIZE ||
        shared_memory->name[0] != '{' ||
        shared_memory->name[WS_SHARED_MEMORY_NAME_SIZE - 1] != '}' ||
        shared_memory->name[WS_SHARED_MEMORY_NAME_SIZE] != '\0') {
            goto release;
    }

    strcpy(path, server->shared_memory_mount_point);
    strcat(path, "/");
    strcat(path, shared_memory->name);

    fd = open(path, O_CREAT | O_RDWR | O_EXCL, S_IWUSR | S_IRUSR);
    if (fd < 0) {
        goto release;
    }

    if (fallocate(fd, 0, 0, (long)shared_memory->size) < 0) {
        goto release;
    }

    addr = mmap(NULL, shared_memory->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        goto release;
    }

    shared_memory->fd = fd;
    shared_memory->addr = addr;
    return true;

release:
    if (fd > 0) {
        close(fd);
    }
    if (addr) {
        munmap(addr, shared_memory->size);
    }

    shared_memory->fd = -1;
    shared_memory->addr = NULL;
    return false;
}

void server_shared_memory_free(struct server_shared_memory *shared_memory) {
    if (!shared_memory) { return; }

    if (shared_memory->addr) {
        munmap(shared_memory->addr, shared_memory->size);
        shared_memory->addr = NULL;
    }
    if (shared_memory->fd > 0) {
        close(shared_memory->fd);
        shared_memory->fd = -1;
    }
}

void server_shared_memory_config(struct wsland_server *server) {
    bool enable_gfxredir = false;
    char *mount_point = getenv("WSL2_SHARED_MEMORY_MOUNT_POINT");
    if (mount_point) {
        server->shared_memory_mount_point = mount_point;
        server->shared_memory_mount_point_size = strlen(server->shared_memory_mount_point);
        enable_gfxredir = true;
    }

    if (enable_gfxredir) {
        enable_gfxredir = false;

        for (int idx = 1; idx <=3; ++idx) {
            struct server_shared_memory shared_memory = {0};
            shared_memory.size = sysconf(_SC_PAGESIZE);
            if (server_shared_memory_new(server, &shared_memory)) {
                *(uint32_t *)shared_memory.addr = 0x12344321;
                server_shared_memory_free(&shared_memory);
                enable_gfxredir = true;
                break;
            }

            sleep(3);
        }

        if (!enable_gfxredir) {
            exit(0);
        }
    }

    server->enable_gfxredir = enable_gfxredir;
}