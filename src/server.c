#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 1;
    }

    printf("server stub: would listen on port %s\n", argv[1]);
    return 0;
}
