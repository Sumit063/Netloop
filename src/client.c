#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_LINE 1024

static int connect_to_server(const char *host, const char *port) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *p = NULL;
    int rv;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    // Beej flow: getaddrinfo -> socket -> connect.
    rv = getaddrinfo(host, port, &hints, &res);
    if (rv != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return -1;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            continue;
        }

        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if (fd < 0) {
        fprintf(stderr, "client: failed to connect\n");
        return -1;
    }

    return fd;
}

static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
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

static ssize_t recv_line(int fd, char *out, size_t max_len, unsigned int slow_ms) {
    size_t used = 0;

    if (max_len == 0) {
        return -1;
    }

    while (used + 1 < max_len) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n == 0) {
            return 0;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (c == '\n') {
            break;
        }
        out[used++] = c;
        if (slow_ms > 0) {
            usleep(slow_ms * 1000);
        }
    }

    if (used > 0 && out[used - 1] == '\r') {
        used--;
    }

    out[used] = '\0';
    return (ssize_t)used;
}

static char *join_command(int argc, char **argv, int start) {
    size_t total = 0;
    for (int i = start; i < argc; i++) {
        total += strlen(argv[i]);
        if (i + 1 < argc) {
            total += 1;
        }
    }

    char *cmd = malloc(total + 1);
    if (!cmd) {
        return NULL;
    }

    size_t used = 0;
    for (int i = start; i < argc; i++) {
        size_t len = strlen(argv[i]);
        memcpy(cmd + used, argv[i], len);
        used += len;
        if (i + 1 < argc) {
            cmd[used++] = ' ';
        }
    }
    cmd[used] = '\0';
    return cmd;
}

int main(int argc, char **argv) {
    unsigned int slow_ms = 0;
    int argi = 1;

    if (argc >= 2 && strcmp(argv[1], "--slow") == 0) {
        if (argc < 6) {
            fprintf(stderr, "usage: %s [--slow <ms>] <host> <port> <command>\n", argv[0]);
            return 1;
        }
        slow_ms = (unsigned int)strtoul(argv[2], NULL, 10);
        argi = 3;
    } else if (argc < 4) {
        fprintf(stderr, "usage: %s [--slow <ms>] <host> <port> <command>\n", argv[0]);
        return 1;
    }

    int fd = connect_to_server(argv[argi], argv[argi + 1]);
    if (fd < 0) {
        return 1;
    }

    char *cmd = join_command(argc, argv, argi + 2);
    if (!cmd) {
        fprintf(stderr, "client: out of memory\n");
        close(fd);
        return 1;
    }

    char line[MAX_LINE + 2];
    int wrote = snprintf(line, sizeof(line), "%s\n", cmd);
    free(cmd);
    if (wrote < 0 || (size_t)wrote >= sizeof(line)) {
        fprintf(stderr, "client: command too long\n");
        close(fd);
        return 1;
    }

    if (send_all(fd, line, (size_t)wrote) < 0) {
        perror("send");
        close(fd);
        return 1;
    }

    char resp[MAX_LINE];
    ssize_t n = recv_line(fd, resp, sizeof(resp), slow_ms);
    if (n == 0) {
        printf("client: server closed\n");
    } else if (n < 0) {
        perror("recv");
    } else {
        printf("%s\n", resp);
    }

    close(fd);
    return 0;
}
