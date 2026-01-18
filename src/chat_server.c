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
#define MAX_NAME 32

struct client {
    struct bufferevent *bev;
    char name[MAX_NAME];
    struct client *next;
};

static struct client *g_clients = NULL;
static unsigned long g_next_id = 1;

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

    // Socket flow: getaddrinfo -> socket -> bind -> listen.
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

static void add_client(struct client *c) {
    c->next = g_clients;
    g_clients = c;
}

static void remove_client(struct client *c) {
    struct client **cur = &g_clients;
    while (*cur) {
        if (*cur == c) {
            *cur = c->next;
            return;
        }
        cur = &(*cur)->next;
    }
}

static int name_in_use(const char *name, struct client *self) {
    struct client *cur = g_clients;
    while (cur) {
        if (cur != self && strcmp(cur->name, name) == 0) {
            return 1;
        }
        cur = cur->next;
    }
    return 0;
}

static void send_line(struct client *c, const char *line) {
    bufferevent_write(c->bev, line, strlen(line));
}

static void broadcast_line(const char *line) {
    struct client *cur = g_clients;
    while (cur) {
        bufferevent_write(cur->bev, line, strlen(line));
        cur = cur->next;
    }
}

static struct client *find_client(const char *name) {
    struct client *cur = g_clients;
    while (cur) {
        if (strcmp(cur->name, name) == 0) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

static void handle_line(struct client *c, char *line) {
    if (strncmp(line, "/nick ", 6) == 0) {
        const char *new_name = line + 6;
        if (new_name[0] == '\0' || strlen(new_name) >= MAX_NAME) {
            send_line(c, "ERR bad_nick\n");
            return;
        }
        if (name_in_use(new_name, c)) {
            send_line(c, "ERR name_in_use\n");
            return;
        }
        snprintf(c->name, sizeof(c->name), "%s", new_name);
        send_line(c, "OK nick\n");
        return;
    }

    if (strcmp(line, "/who") == 0) {
        struct client *cur = g_clients;
        while (cur) {
            char out[MAX_LINE];
            snprintf(out, sizeof(out), "USER %s\n", cur->name);
            send_line(c, out);
            cur = cur->next;
        }
        return;
    }

    if (strncmp(line, "/msg ", 5) == 0) {
        char *target = line + 5;
        char *space = strchr(target, ' ');
        if (!space) {
            send_line(c, "ERR usage\n");
            return;
        }
        *space = '\0';
        char *msg = space + 1;
        if (msg[0] == '\0') {
            send_line(c, "ERR usage\n");
            return;
        }

        struct client *dst = find_client(target);
        if (!dst) {
            send_line(c, "ERR no_such_user\n");
            return;
        }

        {
            char out[MAX_LINE];
            snprintf(out, sizeof(out), "DM %s: %s\n", c->name, msg);
            send_line(dst, out);
        }
        send_line(c, "OK sent\n");
        return;
    }

    {
        char out[MAX_LINE];
        snprintf(out, sizeof(out), "%s: %s\n", c->name, line);
        broadcast_line(out);
    }
}

static void close_client(struct client *c) {
    if (!c) {
        return;
    }
    remove_client(c);
    if (c->bev) {
        bufferevent_free(c->bev);
    }
    free(c);
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
            send_line(c, "ERR too_long\n");
            free(line);
            close_client(c);
            return;
        }

        handle_line(c, line);
        free(line);
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

        snprintf(c->name, sizeof(c->name), "anon%lu", g_next_id++);
        add_client(c);

        c->bev = bufferevent_socket_new(base, client_fd, BEV_OPT_CLOSE_ON_FREE);
        if (!c->bev) {
            close_client(c);
            continue;
        }

        bufferevent_setcb(c->bev, client_read_cb, NULL, client_event_cb, c);
        bufferevent_enable(c->bev, EV_READ | EV_WRITE);

        send_line(c, "INFO welcome\n");
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
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

    printf("chat server: listening on %s\n", argv[1]);
    event_base_dispatch(base);

    event_free(listen_event);
    event_base_free(base);
    close(listener_fd);
    return 0;
}
