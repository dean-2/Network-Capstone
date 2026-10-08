# Binary File Server Protocol Specification

## 1. Protocol Overview

This project defines a custom binary application protocol over TCP for requesting files from a server.

The project provides two programs:

- `bserve` — the TCP file server
- `bcurL` — the command-line client

The server is started as:

    ./bserve ./www 9000

The client is invoked as:

    ./bcurL -v localhost:9000/index.html

All multi-byte integer fields use big-endian byte order.

A TCP connection is persistent. After completing one request/response exchange, the server keeps the connection open and may process another request on the same connection.

TCP is a byte stream and does not preserve application message boundaries. Every protocol frame therefore begins with a fixed-size header containing the exact payload length.

---

## 2. Frame Format

Every frame consists of a 12-byte fixed header followed by its payload.

| Offset | Size | Field |
|------:|-----:|-------|
| 0 | 2 | Magic |
| 2 | 1 | Version |
| 3 | 1 | Frame Type |
| 4 | 4 | Payload Length |
| 8 | 4 | Request ID |

### 2.1 Magic

The Magic field is the two-byte value `0x4254`, representing the ASCII characters `BT`.

A frame with any other magic value is invalid.

### 2.2 Version

The Version field is `0x01`.

Only protocol version 1 is valid.

### 2.3 Frame Type

| Value | Meaning |
|------:|---------|
| `0x01` | Request |
| `0x02` | Response |

If a receiver encounters an unknown frame type, it consumes the complete frame using the Payload Length field, discards the payload, and continues with the next frame.

### 2.4 Payload Length

Payload Length is a 32-bit unsigned integer.

It specifies the exact number of bytes following the 12-byte frame header.

The receiver must read exactly that many bytes before processing the frame.

### 2.5 Request ID

Request ID is a 32-bit unsigned integer.

The client assigns an ID to each request. The server copies that ID into the corresponding response.

---

## 3. Request Frame

A request uses Frame Type `0x01`.

Its payload is:

    Method
    Path Length
    Path

### 3.1 Method

Method is one byte.

| Value | Method |
|------:|--------|
| `0x01` | GET |

### 3.2 Path Length

Path Length is a 16-bit unsigned integer.

It specifies the number of bytes in the Path field.

### 3.3 Path

Path is a UTF-8 encoded byte sequence and must begin with `/`.

For example:

    /index.html

If the server is started with:

    ./bserve ./www 9000

then `/index.html` refers to `./www/index.html`.

The server must not allow a requested path to escape the configured web root. A path that attempts to escape the web root is a malformed request and receives a `400` response.

The request contains no body.

---

## 4. Response Frame

A response uses Frame Type `0x02`.

Its payload is:

    Status Code
    Header Count
    Headers
    Body

### 4.1 Status Code

Status Code is a 16-bit unsigned integer.

| Code | Meaning |
|-----:|---------|
| `200` | File successfully returned |
| `400` | Malformed request |
| `404` | Requested file does not exist |

### 4.2 Header Count

Header Count is one byte and specifies the number of encoded headers following it.

### 4.3 Header Encoding

Each header is encoded as:

    Header ID       1 byte
    Value Length    2 bytes
    Value           N bytes

The value length is an unsigned 16-bit big-endian integer.

Only these response headers are defined:

| ID | Header |
|---:|---|
| `0x01` | Content-Length |
| `0x02` | Content-Type |

`Content-Length` is mandatory in every response and specifies the exact number of body bytes.

`Content-Type` is mandatory for a successful `200` response and describes the type of the returned file.

### 4.4 Body

For a `200` response, the body contains exactly the bytes of the requested file.

For a `400` or `404` response, the body contains a short textual error message.

The number of body bytes must equal the value of the `Content-Length` header.

---

## 5. File Server Behaviour

The server is started as:

    ./bserve <web-root> <port>

For example:

    ./bserve ./www 9000

When the server receives:

    GET /index.html

it looks for the requested file below the configured web root.

If the file exists, the server returns a `200` response and sends the file's bytes as the response body.

If the file does not exist, the server returns `404`.

If the request is malformed, the server returns `400`.

The server keeps the TCP connection open after a response so that another request can be received on the same connection.

---

## 6. TCP Framing

A receiver must not assume that one TCP read contains one complete frame.

For every frame, the receiver:

1. Reads exactly 12 bytes for the fixed header.
2. Decodes the header.
3. Validates the header.
4. Reads exactly Payload Length bytes.
5. Processes or skips the complete frame.
6. Reads the next frame header.

If the connection closes before the complete 12-byte header is received, the current frame is truncated and cannot be processed.

If the connection closes before the complete declared payload is received, the current frame is truncated and cannot be processed.

In either truncated-frame case, the connection is closed.

---

## 7. Unknown Frame Types

Unknown frame types are skipped.

For example, if a receiver receives a frame with:

    Frame Type = 0x99
    Payload Length = 100

the receiver:

1. Reads the 100-byte payload.
2. Discards the payload.
3. Continues reading the next frame.

The unknown payload is not interpreted as a request or response.

---

## 8. Error Handling

### 8.1 Invalid Frame Header

A frame is invalid if its Magic value or Version is invalid.

The server does not process the frame as a request. It closes the connection because the frame cannot be safely interpreted as a valid protocol frame.

### 8.2 Malformed Request

A request is malformed if, for example:

- the request payload is too short;
- the method is not `GET`;
- the Path Length cannot be satisfied by the payload;
- the path does not begin with `/`;
- the path attempts to escape the configured web root.

The server returns `400` and keeps the connection available for subsequent frames.

### 8.3 Missing File

If the requested path does not correspond to an existing file below the configured web root, the server returns `404` and keeps the connection open.

### 8.4 Truncated Frame

If a complete fixed header or declared payload cannot be read because the TCP connection closes, the server closes the connection.

No partial frame is processed.

---

## 9. Client Behaviour

The client is invoked as:

    ./bcurL [-v] <host>:<port>/<path>

For example:

    ./bcurL -v localhost:9000/index.html

The client:

1. Parses the server address and requested path.
2. Opens one TCP connection.
3. Constructs one request frame.
4. Sends the request frame.
5. Reads the response frame.
6. Writes the response body to stdout.
7. Exits with a non-zero status for a `4xx` or `5xx` response.

The client does not open a second TCP connection for the request.

### 9.1 Verbose Mode

When `-v` is supplied, the client prints a hexadecimal dump of every protocol frame it sends or receives.

The dump includes the complete frame header and payload.

Without `-v`, the response body is written to stdout without the protocol framing bytes.

---

## 10. Persistent Connections

The server keeps the TCP connection open after successfully processing a request.

A connection may therefore contain:

    Request 1  -> Response 1
    Request 2  -> Response 2
    Request 3  -> Response 3
    ...

Each frame is delimited by its fixed 12-byte header and Payload Length.

The server processes frames sequentially on the connection.

---

## 11. Annotated Hexdump

The following example shows a complete request and response for:

    GET /index.html

Assume `www/index.html` contains the five ASCII bytes:

    Hello

### 11.1 Request Frame

Request ID is `1`.

The request payload is:

    Method       = GET
    Path Length  = 11
    Path         = /index.html

The complete frame is:

    42 54 01 01 00 00 00 0E 00 00 00 01
    01 00 0B 2F 69 6E 64 65 78 2E 68 74 6D 6C

Decoded:

| Bytes | Meaning |
|---|---|
| `42 54` | Magic = `BT` |
| `01` | Version = 1 |
| `01` | Frame Type = Request |
| `00 00 00 0E` | Payload Length = 14 |
| `00 00 00 01` | Request ID = 1 |
| `01` | Method = GET |
| `00 0B` | Path Length = 11 |
| `2F 69 6E 64 65 78 2E 68 74 6D 6C` | `/index.html` |

The request payload contains 1 + 2 + 11 = 14 bytes.

### 11.2 Response Frame

The server returns:

    Status Code     = 200
    Content-Length = 5
    Content-Type   = text/html
    Body            = Hello

The complete response frame is:

    42 54 01 02 00 00 00 18 00 00 00 01
    00 C8 02
    01 00 01 35
    02 00 09 74 65 78 74 2F 68 74 6D 6C
    48 65 6C 6C 6F

Decoded:

| Bytes | Meaning |
|---|---|
| `42 54` | Magic = `BT` |
| `01` | Version = 1 |
| `02` | Frame Type = Response |
| `00 00 00 18` | Payload Length = 24 |
| `00 00 00 01` | Request ID = 1 |
| `00 C8` | Status Code = 200 |
| `02` | Header Count = 2 |
| `01 00 01 35` | Content-Length = `"5"` |
| `02 00 09 74 65 78 74 2F 68 74 6D 6C` | Content-Type = `text/html` |
| `48 65 6C 6C 6F` | Body = `Hello` |

The response payload is:

    2 bytes status
    1 byte header count
    4 bytes Content-Length header
    12 bytes Content-Type header
    5 bytes body

for a total of 24 bytes, matching the Payload Length field.
