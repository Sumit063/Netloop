#include <errno.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/util.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_LINE 1024

struct client {
    struct bufferevent *bev;
};

static int create_listener_socket(const char *port) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *p = NULL;
    int rv;
    int fd = -1;
    int yes = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    // Beej flow: getaddrinfo -> socket -> bind -> listen.
    rv = getaddrinfo(NULL, port, &hints, &res);
    if (rv != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return -1;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            continue;
        }

        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
            close(fd);
            fd = -1;
            continue;
        }

        if (bind(fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if (fd < 0) {
        fprintf(stderr, "server: failed to bind\n");
        return -1;
    }

    if (listen(fd, 16) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

static void close_client(struct client *c) {
    if (!c) {
        return;
    }
    if (c->bev) {
        bufferevent_free(c->bev);
    }
    free(c);
}

static int handle_command(struct client *c, const char *line) {
    if (strcmp(line, "PING") == 0) {
        const char *resp = "PONG\n";
        bufferevent_write(c->bev, resp, strlen(resp));
        return 0;
    }

    if (strncmp(line, "ECHO ", 5) == 0) {
        char resp[MAX_LINE + 8];
        int wrote = snprintf(resp, sizeof(resp), "%s\n", line + 5);
        if (wrote < 0 || (size_t)wrote >= sizeof(resp)) {
            const char *err = "ERR too_long\n";
            bufferevent_write(c->bev, err, strlen(err));
            return 0;
        }
        bufferevent_write(c->bev, resp, (size_t)wrote);
        return 0;
    }

    if (strcmp(line, "QUIT") == 0) {
        return 1;
    }

    {
        const char *resp = "ERR unknown\n";
        bufferevent_write(c->bev, resp, strlen(resp));
    }

    return 0;
}

static void client_read_cb(struct bufferevent *bev, void *arg) {
    (void)bev;
    struct client *c = arg;
    struct evbuffer *input = bufferevent_get_input(c->bev);

    for (;;) {
        size_t line_len = 0;
        char *line = evbuffer_readln(input, &line_len, EVBUFFER_EOL_LF);
        if (!line) {
            break;
        }

        if (line_len >= MAX_LINE) {
            const char *err = "ERR too_long\n";
            bufferevent_write(c->bev, err, strlen(err));
            free(line);
            close_client(c);
            return;
        }

        int rc = handle_command(c, line);
        free(line);
        if (rc != 0) {
            close_client(c);
            return;
        }
    }
}

static void client_event_cb(struct bufferevent *bev, short events, void *arg) {
    (void)bev;
    struct client *c = arg;

    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
        close_client(c);
    }
}

static void accept_cb(evutil_socket_t fd, short events, void *arg) {
    (void)events;
    struct event_base *base = arg;

    for (;;) {
        struct sockaddr_storage client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            perror("accept");
            return;
        }

        struct client *c = calloc(1, sizeof(*c));
        if (!c) {
            close(client_fd);
            continue;
        }

        c->bev = bufferevent_socket_new(base, client_fd, BEV_OPT_CLOSE_ON_FREE);
        if (!c->bev) {
            close_client(c);
            continue;
        }

        bufferevent_setcb(c->bev, client_read_cb, NULL, client_event_cb, c);
        bufferevent_enable(c->bev, EV_READ | EV_WRITE);

        printf("server: client connected\n");
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 1;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return 1;
    }

    int listener_fd = create_listener_socket(argv[1]);
    if (listener_fd < 0) {
        return 1;
    }

    if (evutil_make_socket_nonblocking(listener_fd) < 0) {
        perror("evutil_make_socket_nonblocking");
        close(listener_fd);
        return 1;
    }

    struct event_base *base = event_base_new();
    if (!base) {
        fprintf(stderr, "server: failed to create event_base\n");
        close(listener_fd);
        return 1;
    }

    struct event *listen_event = event_new(base, listener_fd, EV_READ | EV_PERSIST, accept_cb, base);
    if (!listen_event) {
        fprintf(stderr, "server: failed to create listen event\n");
        event_base_free(base);
        close(listener_fd);
        return 1;
    }

    if (event_add(listen_event, NULL) < 0) {
        fprintf(stderr, "server: failed to add listen event\n");
        event_free(listen_event);
        event_base_free(base);
        close(listener_fd);
        return 1;
    }

    printf("server: listening on %s\n", argv[1]);
    event_base_dispatch(base);

    event_free(listen_event);
    event_base_free(base);
    close(listener_fd);
    return 0;
}
