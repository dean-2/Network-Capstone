# Binary TCP File Server

This C++17 course project implements a TCP server that serves files from a configured web root and a command-line client that requests them. The client writes the response body to standard output and can print protocol frame dumps with `-v`.

## Requirements

- Linux with POSIX TCP sockets
- A C++17 compiler (`g++` is the Makefile default)
- GNU Make
- Python 3 to run the integration tests

## Build

From the project directory, run:

```sh
make
```

## Run

Start the server with a web root and port:

```sh
./bserve ./www 9000
```

Request a file with the client:

```sh
./bcurL localhost:9000/index.html
```

Add `-v` to print hexadecimal protocol frame dumps to standard error:

```sh
./bcurL -v localhost:9000/index.html
```

## Tests

Run the protocol and integration tests with:

```sh
make test
```

The target runs `./tests/test_protocol` and `python3 tests/test_integration.py`.

## Project structure

- `SPEC.md` — complete protocol specification
- `Makefile` — build, test, and clean targets
- `include/` — protocol and file-server declarations
- `src/` — protocol, server, client, and file-serving implementations
- `tests/` — C++ protocol tests and Python integration tests
- `www/` — sample web root, including `index.html`

## Protocol

Frames use a fixed 12-byte header with multi-byte values in big-endian order. Requests use GET paths; responses carry a status, headers, and a body. See [SPEC.md](SPEC.md) for the complete protocol definition.
