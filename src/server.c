#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_LINE 1024

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

static ssize_t recv_line(int fd, char *out, size_t max_len) {
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
    }

    if (used > 0 && out[used - 1] == '\r') {
        used--;
    }

    out[used] = '\0';
    return (ssize_t)used;
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

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 1;
    }

    int listener_fd = create_listener_socket(argv[1]);
    if (listener_fd < 0) {
        return 1;
    }

    printf("server: listening on %s\n", argv[1]);

    // Beej flow: accept a client socket.
    struct sockaddr_storage client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listener_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
        perror("accept");
        close(listener_fd);
        return 1;
    }

    printf("server: client connected\n");

    for (;;) {
        char line[MAX_LINE];
        ssize_t n = recv_line(client_fd, line, sizeof(line));
        if (n == 0) {
            printf("server: client closed\n");
            break;
        }
        if (n < 0) {
            perror("recv");
            break;
        }

        if (strcmp(line, "PING") == 0) {
            const char *resp = "PONG\n";
            if (send_all(client_fd, resp, strlen(resp)) < 0) {
                perror("send");
                break;
            }
        } else if (strncmp(line, "ECHO ", 5) == 0) {
            char resp[MAX_LINE + 8];
            int wrote = snprintf(resp, sizeof(resp), "%s\n", line + 5);
            if (wrote < 0 || (size_t)wrote >= sizeof(resp)) {
                const char *err = "ERR too_long\n";
                send_all(client_fd, err, strlen(err));
                continue;
            }
            if (send_all(client_fd, resp, (size_t)wrote) < 0) {
                perror("send");
                break;
            }
        } else if (strcmp(line, "QUIT") == 0) {
            printf("server: client requested quit\n");
            break;
        } else {
            const char *resp = "ERR unknown\n";
            if (send_all(client_fd, resp, strlen(resp)) < 0) {
                perror("send");
                break;
            }
        }
    }

    close(client_fd);
    close(listener_fd);
    return 0;
}
