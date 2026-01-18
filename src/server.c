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
#define READ_TIMEOUT_SEC 5
#define WRITE_TIMEOUT_SEC 5
#define OUT_HIGH_WM (64 * 1024)
#define OUT_LOW_WM (16 * 1024)
#define RATE_TOKENS_PER_SEC 5.0
#define BURST_TOKENS 10.0

struct server_stats {
    unsigned long active_connections;
    unsigned long total_accepted;
    unsigned long bytes_in;
    unsigned long bytes_out;
    unsigned long timeouts;
    unsigned long rate_limited;
    unsigned long closed_by_client;
};

static struct server_stats g_stats;
static int g_verbose = 0;

struct client {
    struct bufferevent *bev;
    double tokens;
    struct timeval last_refill;
    char peer[NI_MAXHOST + NI_MAXSERV + 2];
    struct timeval connected_at;
};

static double elapsed_ms(const struct timeval *start, const struct timeval *end) {
    double sec = (double)(end->tv_sec - start->tv_sec);
    double usec = (double)(end->tv_usec - start->tv_usec) / 1000000.0;
    return (sec + usec) * 1000.0;
}

static void log_disconnect(struct client *c, const char *reason) {
    if (!g_verbose || !c) {
        return;
    }
    struct timeval now;
    evutil_gettimeofday(&now, NULL);
    printf("client %s disconnect: %s age_ms=%.3f\n",
        c->peer, reason, elapsed_ms(&c->connected_at, &now));
}

static void format_peer(const struct sockaddr_storage *addr, socklen_t addr_len,
    char *out, size_t out_len) {
    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];
    int rc = getnameinfo((const struct sockaddr *)addr, addr_len,
        host, sizeof(host), serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc == 0) {
        snprintf(out, out_len, "%s:%s", host, serv);
        return;
    }
    snprintf(out, out_len, "unknown");
}

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

static void close_client(struct client *c) {
    if (!c) {
        return;
    }
    if (c->bev) {
        bufferevent_free(c->bev);
    }
    if (g_stats.active_connections > 0) {
        g_stats.active_connections--;
    }
    free(c);
}

static int queue_response(struct client *c, const char *buf, size_t len) {
    int rc = bufferevent_write(c->bev, buf, len);
    if (rc == 0) {
        g_stats.bytes_out += len;
    }
    return rc;
}

static int handle_command(struct client *c, const char *line) {
    if (strcmp(line, "PING") == 0) {
        const char *resp = "PONG\n";
        queue_response(c, resp, strlen(resp));
        return 0;
    }

    if (strncmp(line, "ECHO ", 5) == 0) {
        char resp[MAX_LINE + 8];
        int wrote = snprintf(resp, sizeof(resp), "%s\n", line + 5);
        if (wrote < 0 || (size_t)wrote >= sizeof(resp)) {
            const char *err = "ERR too_long\n";
            queue_response(c, err, strlen(err));
            return 0;
        }
        queue_response(c, resp, (size_t)wrote);
        return 0;
    }

    if (strcmp(line, "STATS") == 0) {
        char resp[256];
        int wrote = snprintf(resp, sizeof(resp),
            "active_connections=%lu\n"
            "total_accepted=%lu\n"
            "bytes_in=%lu\n"
            "bytes_out=%lu\n"
            "timeouts=%lu\n"
            "rate_limited=%lu\n"
            "closed_by_client=%lu\n",
            g_stats.active_connections,
            g_stats.total_accepted,
            g_stats.bytes_in,
            g_stats.bytes_out,
            g_stats.timeouts,
            g_stats.rate_limited,
            g_stats.closed_by_client);
        if (wrote > 0) {
            queue_response(c, resp, (size_t)wrote);
        }
        return 0;
    }

    if (strcmp(line, "QUIT") == 0) {
        g_stats.closed_by_client++;
        return 1;
    }

    {
        const char *resp = "ERR unknown\n";
        queue_response(c, resp, strlen(resp));
    }

    return 0;
}

static double min_double(double a, double b) {
    return a < b ? a : b;
}

static void bucket_init(struct client *c) {
    c->tokens = BURST_TOKENS;
    evutil_gettimeofday(&c->last_refill, NULL);
}

static int bucket_consume(struct client *c) {
    struct timeval now;
    evutil_gettimeofday(&now, NULL);

    double elapsed = (double)(now.tv_sec - c->last_refill.tv_sec) +
        (double)(now.tv_usec - c->last_refill.tv_usec) / 1000000.0;
    if (elapsed > 0) {
        c->tokens = min_double(BURST_TOKENS, c->tokens + elapsed * RATE_TOKENS_PER_SEC);
        c->last_refill = now;
    }

    if (c->tokens >= 1.0) {
        c->tokens -= 1.0;
        return 1;
    }

    return 0;
}

static void maybe_pause_reads(struct client *c) {
    struct evbuffer *output = bufferevent_get_output(c->bev);
    size_t out_len = evbuffer_get_length(output);
    if (out_len > OUT_HIGH_WM) {
        bufferevent_disable(c->bev, EV_READ);
    }
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
        struct timeval t0;
        if (g_verbose) {
            evutil_gettimeofday(&t0, NULL);
        }

        if (line_len >= MAX_LINE) {
            const char *err = "ERR too_long\n";
            queue_response(c, err, strlen(err));
            log_disconnect(c, "line_too_long");
            free(line);
            close_client(c);
            return;
        }

        g_stats.bytes_in += line_len + 1;

        if (!bucket_consume(c)) {
            const char *resp = "429 SLOWDOWN\n";
            queue_response(c, resp, strlen(resp));
            g_stats.rate_limited++;
            if (g_verbose) {
                struct timeval t1;
                evutil_gettimeofday(&t1, NULL);
                printf("client %s cmd: %s latency_ms=%.3f rate_limited=1\n",
                    c->peer, line, elapsed_ms(&t0, &t1));
            }
            free(line);
            maybe_pause_reads(c);
            continue;
        }

        int rc = handle_command(c, line);
        if (g_verbose) {
            struct timeval t1;
            evutil_gettimeofday(&t1, NULL);
            printf("client %s cmd: %s latency_ms=%.3f\n",
                c->peer, line, elapsed_ms(&t0, &t1));
        }
        free(line);
        if (rc != 0) {
            log_disconnect(c, "client_quit");
            close_client(c);
            return;
        }
        maybe_pause_reads(c);
    }
}

static void client_write_cb(struct bufferevent *bev, void *arg) {
    (void)bev;
    struct client *c = arg;
    struct evbuffer *output = bufferevent_get_output(c->bev);
    size_t out_len = evbuffer_get_length(output);
    if (out_len <= OUT_LOW_WM) {
        bufferevent_enable(c->bev, EV_READ);
    }
}

static void client_event_cb(struct bufferevent *bev, short events, void *arg) {
    (void)bev;
    struct client *c = arg;

    if (events & BEV_EVENT_TIMEOUT) {
        g_stats.timeouts++;
        log_disconnect(c, "timeout");
        close_client(c);
        return;
    }

    if (events & BEV_EVENT_EOF) {
        g_stats.closed_by_client++;
        log_disconnect(c, "eof");
        close_client(c);
        return;
    }

    if (events & BEV_EVENT_ERROR) {
        if (g_verbose) {
            int err = EVUTIL_SOCKET_ERROR();
            const char *err_str = evutil_socket_error_to_string(err);
            char reason[128];
            snprintf(reason, sizeof(reason), "error:%s", err_str);
            log_disconnect(c, reason);
        }
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

        format_peer(&client_addr, client_len, c->peer, sizeof(c->peer));
        evutil_gettimeofday(&c->connected_at, NULL);
        c->bev = bufferevent_socket_new(base, client_fd, BEV_OPT_CLOSE_ON_FREE);
        if (!c->bev) {
            close_client(c);
            continue;
        }
        bucket_init(c);
        g_stats.total_accepted++;
        g_stats.active_connections++;

        bufferevent_setcb(c->bev, client_read_cb, client_write_cb, client_event_cb, c);
        {
            struct timeval read_tv = { READ_TIMEOUT_SEC, 0 };
            struct timeval write_tv = { WRITE_TIMEOUT_SEC, 0 };
            bufferevent_set_timeouts(c->bev, &read_tv, &write_tv);
        }
        bufferevent_enable(c->bev, EV_READ | EV_WRITE);

        printf("server: client connected\n");
        if (g_verbose) {
            printf("server: peer %s connected\n", c->peer);
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <port> [-v]\n", argv[0]);
        return 1;
    }

    if (argc == 3) {
        if (strcmp(argv[2], "-v") == 0) {
            g_verbose = 1;
        } else {
            fprintf(stderr, "usage: %s <port> [-v]\n", argv[0]);
            return 1;
        }
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
