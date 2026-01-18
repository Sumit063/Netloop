#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_LINE 1024
#define INBUF_SIZE 4096

static int connect_to_server(const char *host, const char *port) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *p = NULL;
    int rv;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    // Socket flow: getaddrinfo -> socket -> connect.
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

static void print_line(const char *buf, size_t len) {
    if (len > 0 && buf[len - 1] == '\r') {
        len--;
    }
    fwrite(buf, 1, len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static int handle_socket_read(int fd, char *inbuf, size_t *in_len) {
    char tmp[1024];
    ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
    if (n == 0) {
        return 0;
    }
    if (n < 0) {
        if (errno == EINTR) {
            return 1;
        }
        perror("recv");
        return 0;
    }

    if (*in_len + (size_t)n > INBUF_SIZE) {
        fprintf(stderr, "client: incoming buffer overflow\n");
        return 0;
    }

    memcpy(inbuf + *in_len, tmp, (size_t)n);
    *in_len += (size_t)n;

    for (;;) {
        char *newline = memchr(inbuf, '\n', *in_len);
        if (!newline) {
            break;
        }
        size_t line_len = (size_t)(newline - inbuf);
        print_line(inbuf, line_len);

        size_t consumed = (size_t)(newline - inbuf + 1);
        size_t remaining = *in_len - consumed;
        memmove(inbuf, inbuf + consumed, remaining);
        *in_len = remaining;
    }

    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <host> <port>\n", argv[0]);
        return 1;
    }

    int fd = connect_to_server(argv[1], argv[2]);
    if (fd < 0) {
        return 1;
    }

    printf("connected. try /nick <name>, /who, /msg <name> <text>\n");

    char inbuf[INBUF_SIZE];
    size_t in_len = 0;

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        FD_SET(STDIN_FILENO, &rfds);

        int maxfd = fd > STDIN_FILENO ? fd : STDIN_FILENO;
        int rc = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }

        if (FD_ISSET(fd, &rfds)) {
            if (!handle_socket_read(fd, inbuf, &in_len)) {
                printf("server closed\n");
                break;
            }
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            char line[MAX_LINE];
            if (!fgets(line, sizeof(line), stdin)) {
                break;
            }

            size_t len = strlen(line);
            if (len == 0) {
                continue;
            }
            if (line[len - 1] != '\n') {
                if (len + 1 < sizeof(line)) {
                    line[len] = '\n';
                    line[len + 1] = '\0';
                    len++;
                }
            }

            if (send_all(fd, line, len) < 0) {
                perror("send");
                break;
            }
        }
    }

    close(fd);
    return 0;
}
