#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <host> <port> <command>\n", argv[0]);
        return 1;
    }

    printf("client stub: would connect to %s:%s and send '%s'\n", argv[1], argv[2], argv[3]);
    return 0;
}
