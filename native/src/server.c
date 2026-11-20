#include <unistd.h>
#include <linux/vm_sockets.h>
#include <sys/un.h>

#include "wsland/wsland.h"
#include "wsland/server.h"


static int create_vsock_fd(const uint16_t port) {
    struct sockaddr_vm socket_address;

    const int socket_fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (socket_fd < 0) {
        return -1;
    }

    const int bufferSize = 65536;

    if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize)) < 0) {
        return -1;
    }

    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize)) < 0) {
        return -1;
    }

    memset(&socket_address, 0, sizeof(socket_address));

    socket_address.svm_family = AF_VSOCK;
    socket_address.svm_cid = VMADDR_CID_ANY;
    socket_address.svm_port = port;

    const socklen_t socket_addr_size = sizeof(socket_address);

    if (bind(socket_fd, (const struct sockaddr *) &socket_address, socket_addr_size) < 0) {
        close(socket_fd);
        return -2;
    }

    const int status = listen(socket_fd, 1);

    if (status != 0) {
        close(socket_fd);
        return -4;
    }

    return socket_fd;
}

static int use_vsock_fd(const uint16_t port) {
    char *fd_str = getenv("USE_VSOCK");
    if (!fd_str) {
        return -1;
    }

    int fd;
    if (strlen(fd_str) != 0) {
        fd = atoi(fd_str); // NOLINT(*-err34-c)
        if (fd == 0) {
            fd = -1;
        }
    } else {
        fd = create_vsock_fd(port);
    }
    return fd;
}

static int wsland_server_activity(void *listener_ptr, void *_) {
    freerdp_listener *listener = listener_ptr;

    if (!listener->CheckFileDescriptor(listener)) {
        return false;
    }

    return true;
}

static void wsland_server_ready_notify(void) {
    struct sockaddr_un addr = {0};

    char *socket_path = getenv("WSLGD_NOTIFY_SOCKET");
    if (!socket_path) {
        return;
    }

    int socket_fd = socket(PF_LOCAL, SOCK_SEQPACKET, 0);
    if (socket_fd < 0) {
        return;
    }

    addr.sun_family = AF_LOCAL;
    socklen_t name_size = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path) + 1;
    socklen_t size = offsetof(struct sockaddr_un, sun_path) + name_size;

    int fd = connect(socket_fd, (struct sockaddr *)&addr, size);
    if (fd < 0) {
        goto close_socket_fd;
    }

    close(fd);

close_socket_fd:
    close(socket_fd);
}

void wsland_server_switch_next_monitor(WslandServer *server) {
    struct wsland_client *client = server->client;
    if (!client || (client->mask & SERVER_CLIENT_ACTIVATED) == 0) { return; }

    struct wl_list *next_link = client->monitor.link.next;
    if (next_link == &client->server->monitors) { next_link = client->server->monitors.next; }
    struct wsland_monitor *next_monitor = wl_container_of(next_link, next_monitor, link);
    if (next_monitor->idx && next_monitor->idx != client->monitor.idx) {
        client->monitor = *next_monitor;

        struct wsc_output_change_event *event = malloc(sizeof(*event));
        wl_list_insert(&client->event.output, &event->link);
    }
}

WslandServer * wsland_server_create(uint16_t port, const char *addr, const WslandCallback *callback, void *wsland) {
    WslandServer *server = calloc(1, sizeof(*server));
    if (!server) { return NULL; }

    server->listener = freerdp_listener_new();
    if (!server->listener) { return NULL; }

    server->listener->param4 = server;
    server->listener->PeerAccepted = server_incoming_client;
    server->addr = strdup(addr);
    server->port = port;

    const int vsock_fd = use_vsock_fd(port);
    server_generate_crypto(server);

    if (vsock_fd > 0) {
        if (!server->listener->OpenFromSocket(server->listener, vsock_fd)) {
            goto create_failed;
        }
    } else {
        if (!server->listener->Open(server->listener, server->addr, server->port)) {
            goto create_failed;
        }
    }

    HANDLE handles[WS_MAX_FDS] = { 0 };
    const int count = (int) server->listener->GetEventHandles(server->listener, handles, WS_MAX_FDS);
    if (!count) {
        goto create_failed;
    }

    for (int i = 0; i < count; ++i) {
        const int fd = GetEventFileDescriptor(handles[i]);
        callback->dispatch_event(wsland, fd, false, server->listener, wsland_server_activity);
    }

    server->wsland = wsland;
    server->callback = callback;
    server_shared_memory_config(server);
    wl_list_init(&server->monitors);

    wsland_server_ready_notify();
    return server;

create_failed:
    wsland_server_destroy(server);
    return NULL;
}

void wsland_server_destroy(WslandServer *server) {
    if (server) {
        if (server->client) {
            freerdp_peer *peer = server->client->peer;
            freerdp_peer_context_free(peer);
            freerdp_peer_free(peer);
            server->client = NULL;
        }

        if (server->listener) {
            freerdp_listener *listener = server->listener;
            freerdp_listener_free(listener);
            server->listener = NULL;
        }

        free(server->addr);
        free(server->key_content);
        free(server->cert_content);
        free(server);
    }
}