#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "wsland/server.h"

#define CF_PRIVATE_RTF  19
#define CF_PRIVATE_HTML 20

#define WSLAND_CLIPBOARD_FRAGMENT_START_OFFSET (53)
#define WSLAND_CLIPBOARD_FRAGMENT_END_OFFSET (75)

#define DIB_WIDTH_BYTES(bits) ((((bits) + 31) & ~31) >> 3)
#define DIB_HEADER_MARKER     ((WORD) ('M' << 8) | 'B')

// static const char clipboard_html_header[] = "Version:0.9\r\nStartHTML:-1\r\nEndHTML:-1\r\nStartFragment:00000000\r\nEndFragment:00000000\r\n";
// static const char clipboard_html_fragment_start[] = "<!--StartFragment-->\r\n";
// static const char clipboard_html_fragment_end[] = "<!--EndFragment-->\r\n";

static bool clipboard_process_text_utf8(bool to_utf8, void *source_data, size_t source_count, void **target_data, size_t *target_count);
static bool clipboard_process_text_raw(bool to_utf8, void *source_data, size_t source_count, void **target_data, size_t *target_count);
static bool clipboard_process_html(bool to_utf8, void *source_data, size_t source_count, void **target_data, size_t *target_count);
static bool clipboard_process_bmp(bool to_utf8, void *source_data, size_t source_count, void **target_data, size_t *target_count);

typedef bool (*pfn_process_data)(bool to_utf8, void *source_data, size_t source_count, void **target_data, size_t *target_count);

struct wsc_clipboard_supported_format {
    uint32_t format_id;
    char *format_name;
    char *mime_type;

    pfn_process_data pfn;
};

// ReSharper disable CppUseDesignatedInitializers
static struct wsc_clipboard_supported_format clipboard_supported_formats[WS_CLIPBOARD_MAX_FORMATS] = {
    { CF_UNICODETEXT,  NULL,               "text/plain;charset=utf-8", clipboard_process_text_utf8 },
    { CF_TEXT,         NULL,               "STRING",                   clipboard_process_text_raw  },
    { CF_DIB,          NULL,               "image/bmp",                clipboard_process_bmp       },
    { CF_PRIVATE_RTF,  "Rich Text Format", "text/rtf",                 clipboard_process_text_raw  },
    { CF_PRIVATE_HTML, "HTML Format",      "text/html",                clipboard_process_html      },
};

struct windows_provide_format_task {
    struct server_client_task task;

    size_t count;
    const char *mime_types[WS_CLIPBOARD_MAX_FORMATS];
};

struct windows_provide_format_response_task {
    struct server_client_task task;

    void *data;
    size_t count;
};

struct windows_sync_format_request_task {
    struct server_client_task task;

    struct wsc_clipboard_supported_format *format;
};

static bool clipboard_process_text_utf8(bool to_utf8, void *source_data, size_t source_count, void **target_data, size_t *target_count) {

    if (to_utf8) {
        *target_data = ConvertWCharNToUtf8Alloc(source_data, source_count, target_count);
    } else {
        *target_data = ConvertUtf8NToWCharAlloc(source_data, source_count, target_count);
        if (target_data) { *target_count = (*target_count + 1) * sizeof(WCHAR); }
    }

    free(source_data);
    return target_data != NULL;
}

static bool clipboard_process_text_raw(bool to_utf8, void *source_data, size_t source_count, void **target_data, size_t *target_count) {
    LOG("clipboard ==> clipboard_process_text_raw");
    free(source_data);
    return false;
}

static bool clipboard_process_html(bool to_utf8, void *source_data, size_t source_count, void **target_data, size_t *target_count) {
    LOG("clipboard ==> clipboard_process_html");
    free(source_data);
    return false;
}

static bool clipboard_process_bmp(bool to_utf8, void *source_data, size_t source_count, void **target_data, size_t *target_count) {
    LOG("clipboard ==> clipboard_process_bmp");
    free(source_data);
    return false;
}

static struct wsc_clipboard_supported_format * clipboard_find_supported(UINT32 format_id, const char *format_name) {
    for (uint32_t idx = 0; idx < WS_CLIPBOARD_MAX_FORMATS; idx++) {
        struct wsc_clipboard_supported_format *format = &clipboard_supported_formats[idx];

        if ((format->format_name == NULL && format_id == format->format_id) ||
            (format->format_name && format_name && strcmp(format_name, format->format_name) == 0)) {
                return format;
            }
    }
    return NULL;
}

static struct wsc_clipboard_supported_format * clipboard_find_supported_by_id(UINT32 format_id) {
    for (uint32_t i = 0; i < WS_CLIPBOARD_MAX_FORMATS; i++) {
        struct wsc_clipboard_supported_format *format = &clipboard_supported_formats[i];

        if (format_id == format->format_id) {
            return format;
        }
    }
    return NULL;
}

static struct wsc_clipboard_supported_format * clipboard_find_supported_by_type(const char *mime_type) {
    for (uint32_t idx = 0; idx < WS_CLIPBOARD_MAX_FORMATS; idx++) {
        struct wsc_clipboard_supported_format *format = &clipboard_supported_formats[idx];

        if (strcmp(mime_type, format->mime_type) == 0) {
            return format;
        }
    }
    return NULL;
}

static void clipboard_send_format_data_response_fail(struct wsland_client *client) {
    CLIPRDR_FORMAT_DATA_RESPONSE formatDataResponse = {0};
    formatDataResponse.common.msgType = CB_FORMAT_DATA_RESPONSE;
    formatDataResponse.common.msgFlags = CB_RESPONSE_FAIL;
    formatDataResponse.common.dataLen = 0;
    formatDataResponse.requestedFormatData = NULL;
    client->clipboard_ctx->ServerFormatDataResponse(client->clipboard_ctx, &formatDataResponse);
}

static bool read_fd_data(int32_t fd, void **data, size_t *count) {
    size_t capacity = 4096;
    size_t size = 0;

    uint8_t *buffer = malloc(capacity);
    if (!buffer) { return false; }

    while (true) {
        if (size == capacity) {
            capacity *= 2;
            uint8_t *temp = realloc(buffer, capacity);
            if (!temp) {
                free(buffer);
                return false;
            }
            buffer = temp;
        }

        ssize_t length = read(fd, buffer + size, capacity - size);
        if (length > 0) {
            size += length;
            continue;
        }

        if (length == 0) { break; }

        if (errno == EINTR) { continue; }

        if (errno == EAGAIN || errno == EWOULDBLOCK) { break; }

        free(buffer);
        return false;
    }

    *data = buffer;
    *count = size;
    return true;
}

static int windows_sync_format_request_read(void *data, void *state);
static void handle_next_windows_sync_format_request(struct wsland_client *client, void *state) {
    if (client->clipboard.sync_flight || wl_list_empty(&client->event.windows_sync)) {
        return;
    }

    struct windows_sync_format_request_event *event = wl_container_of(client->event.windows_sync.next, event, link);
    wl_list_remove(&event->link);
    wl_list_init(&event->link);

    client->clipboard.sync_flight = event;
    if (!client->server->callback->windows_sync_format_request_write(state, event->format->mime_type, event->write_fd)) {
        client->clipboard.sync_flight = NULL;
        close(event->write_fd);
        close(event->read_fd);
        free(event);

        clipboard_send_format_data_response_fail(client);
        handle_next_windows_sync_format_request(client, state);
        return;
    }

    client->server->callback->dispatch_event(
        client->server->wsland, event->read_fd, false, client, windows_sync_format_request_read
    );
}

static int windows_sync_format_request_read(void *data, void *state) {
    struct wsland_client *client = data;
    struct windows_sync_format_request_event *event = client->clipboard.sync_flight;
    client->clipboard.sync_flight = NULL;

    if (event) {
        void *source_data;
        size_t source_count;

        if (read_fd_data(event->read_fd, &source_data, &source_count)) {
            void *target_data;
            size_t target_count;

            if (event->format->pfn(false, source_data, source_count, &target_data, &target_count)) {
                CLIPRDR_FORMAT_DATA_RESPONSE formatDataResponse = {0};
                formatDataResponse.common.msgType = CB_FORMAT_DATA_RESPONSE;
                formatDataResponse.common.msgFlags = CB_RESPONSE_OK;
                formatDataResponse.requestedFormatData = target_data;
                formatDataResponse.common.dataLen = target_count;
                client->clipboard_ctx->ServerFormatDataResponse(client->clipboard_ctx, &formatDataResponse);
                free(target_data);
            } else {
                clipboard_send_format_data_response_fail(client);
            }
        } else {
            clipboard_send_format_data_response_fail(client);
        }
        free(event);
    }

    handle_next_windows_sync_format_request(client, state);
    return false;
}

static void windows_sync_format_request_callback(bool free_only, void *data, void *state) {
    struct windows_sync_format_request_task *task = wl_container_of(data, task, task);

    if (!free_only) {
        struct wsland_client *client = task->task.client;

        int p[2] = {0};
        if (pipe2(p, O_CLOEXEC) == -1) {
            clipboard_send_format_data_response_fail(client);
            goto release;
        }

        struct windows_sync_format_request_event *event = malloc(sizeof(*event));
        event->format = task->format;
        event->write_fd = p[1];
        event->read_fd = p[0];

        wl_list_insert(client->event.windows_sync.prev, &event->link);
        handle_next_windows_sync_format_request(client, state);
    }

release:
    free(task);
}

static UINT windows_sync_format_request(CliprdrServerContext *context, const CLIPRDR_FORMAT_DATA_REQUEST *data_request) {
    struct wsland_client *client = context->custom;

    struct wsc_clipboard_supported_format *format = clipboard_find_supported_by_id(data_request->requestedFormatId);
    if (!format) {
        clipboard_send_format_data_response_fail(client);
        return CHANNEL_RC_OK;
    }

    struct windows_sync_format_request_task *task = malloc(sizeof(*task));
    task->format = format;

    server_client_task_dispatch(client, windows_sync_format_request_callback, &task->task);
    return CHANNEL_RC_OK;
}

static void handle_next_windows_provide_format_request(struct wsland_client *client) {
    if (client->clipboard.provide_flight || wl_list_empty(&client->event.windows_provide)) {
        return;
    }

    struct windows_provide_format_request_event *event = wl_container_of(client->event.windows_provide.next, event, link);
    wl_list_remove(&event->link);
    wl_list_init(&event->link);

    client->clipboard.provide_flight = event;
    CLIPRDR_FORMAT_DATA_REQUEST formatDataRequest = {0};
    formatDataRequest.common.msgType = CB_FORMAT_DATA_REQUEST;
    formatDataRequest.requestedFormatId = event->format->format_id;
    formatDataRequest.common.dataLen = 4;

    if (client->clipboard_ctx->ServerFormatDataRequest(client->clipboard_ctx, &formatDataRequest) != 0) {
        client->clipboard.provide_flight = NULL;

        close(event->fd);
        free(event);

        handle_next_windows_provide_format_request(client);
    }
}

static void windows_provide_format_response_callback(bool free_only, void *data, void *state) {
    struct windows_provide_format_response_task *task = wl_container_of(data, task, task);

    if (!free_only) {
        struct wsland_client *client = task->task.client;
        struct windows_provide_format_request_event *event = client->clipboard.provide_flight;
        client->clipboard.provide_flight = NULL;

        if (event) {
            void *target_data;
            size_t target_count;

            if (event->format->pfn(true, task->data, task->count, &target_data, &target_count)) {
                client->server->client->server->callback->windows_provide_format_data(
                    state, event->fd, event->format->mime_type, target_data, target_count
                );
                free(target_data);
            } else {
                close(event->fd);
            }
            free(event);
        } else {
            free(task->data);
        }
        handle_next_windows_provide_format_request(client);
    }
    free(task);
}

static UINT windows_provide_format_response(CliprdrServerContext *context, const CLIPRDR_FORMAT_DATA_RESPONSE *response) {
    struct wsland_client *client = context->custom;

    struct windows_provide_format_response_task *task = malloc(sizeof(*task));
    task->count = response->common.dataLen;
    task->data = malloc(response->common.dataLen);
    memcpy(task->data, response->requestedFormatData, response->common.dataLen);

    server_client_task_dispatch(client, windows_provide_format_response_callback, &task->task);
    return CHANNEL_RC_OK;
}

static void windows_provide_format_callback(bool free_only, void *data, void *state) {
    struct windows_provide_format_task *task = wl_container_of(data, task, task);

    if (!free_only) {
        struct wsland_client *client = task->task.client;

        client->clipboard.op_id++;
        client->server->callback->windows_provide_format(state, client->clipboard.op_id, task->mime_types, task->count);
    }

    free(task);
}

static UINT windows_provide_format(CliprdrServerContext *context, const CLIPRDR_FORMAT_LIST *format_list) {
    // LOG("clipboard ==> clipboard_client_format_list");
    struct wsland_client *client = context->custom;

    struct windows_provide_format_task *task = malloc(sizeof(*task));
    task->count = 0;

    for (uint32_t idx = 0; idx < format_list->numFormats; ++idx) {
        CLIPRDR_FORMAT *format = &format_list->formats[idx];
        struct wsc_clipboard_supported_format *wsc_format = clipboard_find_supported(format->formatId, format->formatName);

        if (wsc_format && wsc_format->mime_type) {
            task->mime_types[task->count++] = wsc_format->mime_type;
        }
    }

    server_client_task_dispatch(client, windows_provide_format_callback, &task->task);

    CLIPRDR_FORMAT_LIST_RESPONSE formatListResponse = {0};
    formatListResponse.common.msgType = CB_FORMAT_LIST_RESPONSE;
    formatListResponse.common.msgFlags = CB_RESPONSE_OK;
    formatListResponse.common.dataLen = 0;

    if (client->clipboard_ctx->ServerFormatListResponse(client->clipboard_ctx, &formatListResponse) != 0) {
        return -1;
    }

    return CHANNEL_RC_OK;
}

static UINT clipboard_client_temp_directory(CliprdrServerContext *context, const CLIPRDR_TEMP_DIRECTORY *temp_dir) {
    // LOG("clipboard ==> clipboard_client_temp_directory");
    return CHANNEL_RC_OK;
}

static UINT clipboard_client_capabilities(CliprdrServerContext* context, const CLIPRDR_CAPABILITIES* capabilities) {
    // LOG("clipboard ==> clipboard_client_capabilities");

    for (uint32_t idx = 0; idx < capabilities->cCapabilitiesSets; ++idx) {
        CLIPRDR_CAPABILITY_SET* cs = &capabilities->capabilitySets[idx];

        switch (cs->capabilitySetType) {
        case CB_CAPSTYPE_GENERAL: break;
        default: return -1;
        }
    }

    return CHANNEL_RC_OK;
}

static UINT clipboard_client_format_list_response(CliprdrServerContext *context, const CLIPRDR_FORMAT_LIST_RESPONSE *response) {
    // LOG("clipboard ==> clipboard_client_format_list_response");
    return CHANNEL_RC_OK;
}

void windows_provide_format_request(WslandServer *server, uint32_t op_id, const char *mime_type, int32_t fd) {
    struct wsland_client *client = server->client;
    if (!client || !client->clipboard_ctx) {
        close(fd);
        return;
    }

    struct wsc_clipboard_supported_format * format = clipboard_find_supported_by_type(mime_type);
    if (!format) {
        close(fd);
        return;
    }

    struct windows_provide_format_request_event *event = malloc(sizeof(*event));
    event->fd = fd;
    event->op_id = op_id;
    event->format = format;

    wl_list_insert(client->event.windows_provide.prev, &event->link);
    handle_next_windows_provide_format_request(client);
}

void windows_sync_format(WslandServer* server, const char* const* mime_types, size_t count) {
    struct wsland_client *client = server->client;
    if (!client || !client->clipboard_ctx) { return; }

    int num_supported_format = 0;
    CLIPRDR_FORMAT formats[WS_CLIPBOARD_MAX_FORMATS] = {0};

    for (uint32_t idx = 0; idx < count; ++idx) {
        const char* mime_type = mime_types[idx];
        struct wsc_clipboard_supported_format *wsc_format = clipboard_find_supported_by_type(mime_type);

        if (wsc_format) {
            CLIPRDR_FORMAT *format = &formats[num_supported_format++];
            format->formatId = wsc_format->format_id;
            format->formatName = wsc_format->format_name;
        }
    }

    if (num_supported_format) {
        CLIPRDR_FORMAT_LIST formatList = {0};
        formatList.common.msgType = CB_FORMAT_LIST;
        formatList.numFormats = num_supported_format;
        formatList.formats = &formats[0];

        client->clipboard.op_id++;
        client->clipboard_ctx->ServerFormatList(client->clipboard_ctx, &formatList);
    }
}

bool server_client_clipboard_init(struct wsland_client *client) {
    CliprdrServerContext *clipboard = cliprdr_server_context_new(client->vcm);
    if (!clipboard) {
        return false;
    }
    client->clipboard_ctx = clipboard;

    clipboard->custom = client;
    clipboard->TempDirectory = clipboard_client_temp_directory;
    clipboard->ClientCapabilities = clipboard_client_capabilities;
    clipboard->ClientFormatListResponse = clipboard_client_format_list_response;
    clipboard->ClientFormatDataRequest = windows_sync_format_request;
    clipboard->ClientFormatDataResponse = windows_provide_format_response;
    clipboard->ClientFormatList = windows_provide_format;
    clipboard->useLongFormatNames = FALSE;
    clipboard->streamFileClipEnabled = FALSE;
    clipboard->fileClipNoFilePaths = FALSE;
    clipboard->canLockClipData = TRUE;

    if (clipboard->Start(client->clipboard_ctx) != 0) {
        cliprdr_server_context_free(client->clipboard_ctx);
        client->clipboard_ctx = NULL;
        return false;
    }
    return true;
}

void server_client_clipboard_source_destroy(struct wsland_client *client) {
    if (client->clipboard_ctx) {
        client->clipboard_ctx->Stop(client->clipboard_ctx);
        cliprdr_server_context_free(client->clipboard_ctx);
        client->clipboard_ctx = NULL;
    }
}