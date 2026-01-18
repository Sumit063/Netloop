# NetLoop

Small TCP client/server project that evolves from sockets to libevent.

## Directory

- `src/` C sources for server and client
- `bin/` build outputs
- `scripts/bench.sh` simple load generator
- `Makefile` build rules

## How to use

```bash
sudo apt update
sudo apt install -y libevent-dev
make
```

Two terminals:

```bash
./bin/server 9090
./bin/client 127.0.0.1 9090 PING
```

Verbose server logging:

```bash
./bin/server 9090 -v
```

Chat server/client:

```bash
./bin/chat_server 9091
./bin/chat_client 127.0.0.1 9091
```

Interactive client:

```bash
./bin/client 127.0.0.1 9090
```

Bench (optional):

```bash
./scripts/bench.sh 10 100
```
