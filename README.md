# NetLoop

NetLoop is a small, focused TCP client/server project in C that evolves from
classic sockets into an event-driven architecture using libevent. The goal is
to understand how scalable servers behave under load: nonblocking I/O, event
loops, buffering, and connection lifecycle handling.

> Why this exists: frameworks hide the sharp edges. NetLoop deliberately touches
them in a controlled, readable codebase.

## What is inside

- `server` - protocol server (libevent-based)
- `client` - protocol client (one-shot or interactive)
- `chat_server` - multi-client chat server
- `chat_client` - interactive chat client
- `scripts/bench.sh` - simple load generator for local testing

## Directory layout

- `src/` C sources for server/client plus chat variants
- `bin/` build outputs (created by `make`)
- `scripts/bench.sh` simple load generator
- `Makefile` build rules

## Requirements

Linux or WSL Ubuntu is recommended.

```bash
sudo apt update
sudo apt install -y build-essential libevent-dev
```

## Build

```bash
make
```

## Run the protocol server

Terminal 1:

```bash
./bin/server 9090
```

Terminal 2:

```bash
./bin/client 127.0.0.1 9090 PING
./bin/client 127.0.0.1 9090 ECHO hello
./bin/client 127.0.0.1 9090 STATS
```

Interactive mode:

```bash
./bin/client 127.0.0.1 9090
```

Protocol commands:

- `PING` -> `PONG`
- `ECHO <msg>` -> `<msg>`
- `STATS` -> multi-line key=value stats
- `QUIT` -> close connection

## Verbose logging

Enable server-side logs for per-command latency and disconnect reasons:

```bash
./bin/server 9090 -v
```

## Stats and observability

The server tracks:

- `active_connections`
- `total_accepted`
- `bytes_in` and `bytes_out`
- `timeouts`
- `rate_limited`
- `closed_by_client`

Use `STATS` from the client to inspect current counters.

## Bench script

The bench script launches N background client loops, each sending M requests.
Each request is a short-lived connection that sends `PING` and reads `PONG`.

```bash
./scripts/bench.sh 10 100
```

Why this matters: it gives a quick throughput baseline and exercises accept,
parse, respond, and close behavior under local load.

## Chat server/client

Run the chat system on a separate port:

```bash
./bin/chat_server 9091
./bin/chat_client 127.0.0.1 9091
```

Chat commands:

- `/nick <name>` set a username
- `/who` list users
- `/msg <name> <text>` direct message
- any other line broadcasts to all

The chat server routes each message to the correct client connection, and logs
joins, leaves, and message routing on the server side.

## Design notes

- Nonblocking sockets + libevent keep the server responsive under load.
- Bufferevents simplify input/output buffering and line parsing.
- Read/write timeouts close stalled connections.
- Output watermarks provide backpressure for slow readers.
- Per-connection token buckets limit abusive clients without impacting others.

## Troubleshooting

- Build fails with `event2/...` headers missing: install `libevent-dev`.
- Interactive clients may time out if idle (default read timeout is 5s).
