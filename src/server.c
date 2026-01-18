#include <errno.h>
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
#define INBUF_SIZE 4096

struct client {
    int fd;
    struct event *read_event;
    char buf[INBUF_SIZE];
    size_t buf_len;
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
    if (c->read_event) {
        event_free(c->read_event);
    }
    if (c->fd >= 0) {
        close(c->fd);
    }
    free(c);
}

static int send_response(int fd, const char *buf, size_t len) {
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return -1;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t)n;
    }

    return 0;
}

static int handle_command(struct client *c, const char *line) {
    if (strcmp(line, "PING") == 0) {
        const char *resp = "PONG\n";
        if (send_response(c->fd, resp, strlen(resp)) < 0) {
            return -1;
        }
        return 0;
    }

    if (strncmp(line, "ECHO ", 5) == 0) {
        char resp[MAX_LINE + 8];
        int wrote = snprintf(resp, sizeof(resp), "%s\n", line + 5);
        if (wrote < 0 || (size_t)wrote >= sizeof(resp)) {
            const char *err = "ERR too_long\n";
            send_response(c->fd, err, strlen(err));
            return 0;
        }
        if (send_response(c->fd, resp, (size_t)wrote) < 0) {
            return -1;
        }
        return 0;
    }

    if (strcmp(line, "QUIT") == 0) {
        return 1;
    }

    {
        const char *resp = "ERR unknown\n";
        if (send_response(c->fd, resp, strlen(resp)) < 0) {
            return -1;
        }
    }

    return 0;
}

static void client_read_cb(evutil_socket_t fd, short events, void *arg) {
    (void)events;
    struct client *c = arg;

    for (;;) {
        char tmp[1024];
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n == 0) {
            close_client(c);
            return;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            perror("recv");
            close_client(c);
            return;
        }

        if (c->buf_len + (size_t)n > sizeof(c->buf)) {
            const char *err = "ERR too_long\n";
            send_response(c->fd, err, strlen(err));
            close_client(c);
            return;
        }

        memcpy(c->buf + c->buf_len, tmp, (size_t)n);
        c->buf_len += (size_t)n;

        for (;;) {
            char *newline = memchr(c->buf, '\n', c->buf_len);
            if (!newline) {
                break;
            }

            size_t line_len = (size_t)(newline - c->buf);
            if (line_len > 0 && c->buf[line_len - 1] == '\r') {
                line_len--;
            }
            if (line_len >= MAX_LINE) {
                const char *err = "ERR too_long\n";
                send_response(c->fd, err, strlen(err));
                close_client(c);
                return;
            }

            char line[MAX_LINE];
            memcpy(line, c->buf, line_len);
            line[line_len] = '\0';

            size_t consumed = (size_t)(newline - c->buf + 1);
            size_t remaining = c->buf_len - consumed;
            memmove(c->buf, c->buf + consumed, remaining);
            c->buf_len = remaining;

            int rc = handle_command(c, line);
            if (rc != 0) {
                close_client(c);
                return;
            }
        }
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

        if (evutil_make_socket_nonblocking(client_fd) < 0) {
            close(client_fd);
            continue;
        }

        struct client *c = calloc(1, sizeof(*c));
        if (!c) {
            close(client_fd);
            continue;
        }

        c->fd = client_fd;
        c->read_event = event_new(base, client_fd, EV_READ | EV_PERSIST, client_read_cb, c);
        if (!c->read_event) {
            close_client(c);
            continue;
        }

        if (event_add(c->read_event, NULL) < 0) {
            close_client(c);
            continue;
        }

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
