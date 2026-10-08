#!/usr/bin/env python3

import argparse
import os
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "bserve"
CLIENT = ROOT / "bcurL"
MAGIC = 0x4254
VERSION = 1
REQUEST = 1
RESPONSE = 2


def frame(frame_type, request_id, payload):
    return struct.pack("!HBBII", MAGIC, VERSION, frame_type, len(payload), request_id) + payload


def request_payload(path, method=1):
    path_bytes = path.encode("utf-8")
    return struct.pack("!BH", method, len(path_bytes)) + path_bytes


def receive_exact(sock, size):
    chunks = bytearray()
    while len(chunks) < size:
        part = sock.recv(size - len(chunks))
        if not part:
            raise EOFError(f"connection closed after {len(chunks)} of {size} bytes")
        chunks.extend(part)
    return bytes(chunks)


def receive_frame(sock):
    header = receive_exact(sock, 12)
    magic, version, frame_type, payload_length, request_id = struct.unpack("!HBBII", header)
    assert magic == MAGIC
    assert version == VERSION
    payload = receive_exact(sock, payload_length)
    return header + payload, frame_type, request_id, payload


def decode_response(payload):
    assert len(payload) >= 3
    status = struct.unpack("!H", payload[:2])[0]
    header_count = payload[2]
    offset = 3
    headers = {}
    for _ in range(header_count):
        header_id = payload[offset]
        value_length = struct.unpack("!H", payload[offset + 1 : offset + 3])[0]
        offset += 3
        value = payload[offset : offset + value_length]
        assert len(value) == value_length
        offset += value_length
        headers[header_id] = value
    assert 1 in headers
    content_length = int(headers[1].decode("ascii"))
    body = payload[offset:]
    assert len(body) == content_length
    return status, headers, body


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def start_server(web_root, requested_port=None, address_space_limit=None):
    port = free_port() if requested_port is None else requested_port
    process_options = {}
    if address_space_limit is not None:
        import resource

        def limit_address_space():
            resource.setrlimit(resource.RLIMIT_AS, (address_space_limit, address_space_limit))

        process_options["preexec_fn"] = limit_address_space
    process = subprocess.Popen(
        [str(SERVER), str(web_root), str(port)],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        **process_options,
    )
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            _, stderr = process.communicate()
            raise AssertionError(f"server exited early: {stderr.decode(errors='replace')}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return process, port
        except OSError:
            time.sleep(0.02)
    process.terminate()
    process.wait(timeout=5)
    raise AssertionError("server did not start listening")


def stop_server(process):
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)
    process.communicate()


def exchange(sock, request_id, path, payload=None):
    if payload is None:
        payload = request_payload(path)
    sock.sendall(frame(REQUEST, request_id, payload))
    _, frame_type, response_id, response_payload = receive_frame(sock)
    assert frame_type == RESPONSE
    assert response_id == request_id
    return decode_response(response_payload)


def check_raw_spec_frames(port, expected_body):
    request = frame(REQUEST, 1, request_payload("/index.html"))
    expected_request = bytes.fromhex(
        "42 54 01 01 00 00 00 0E 00 00 00 01 "
        "01 00 0B 2F 69 6E 64 65 78 2E 68 74 6D 6C"
    )
    assert request == expected_request
    expected_response = bytes.fromhex(
        "42 54 01 02 00 00 00 18 00 00 00 01 "
        "00 C8 02 01 00 01 35 "
        "02 00 09 74 65 78 74 2F 68 74 6D 6C "
        "48 65 6C 6C 6F"
    )
    assert expected_body == b"Hello", "SPEC hexdump fixture must contain exactly five bytes"
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        sock.sendall(request)
        actual, frame_type, request_id, payload = receive_frame(sock)
        assert frame_type == RESPONSE and request_id == 1
        assert actual == expected_response, "actual response bytes differ from SPEC hexdump"
        status, headers, body = decode_response(payload)
        assert status == 200 and headers[2] == b"text/html" and body == expected_body


def check_client(port, expected_body):
    target = f"localhost:{port}/index.html"
    plain = subprocess.run([str(CLIENT), target], cwd=ROOT, capture_output=True, check=False)
    assert plain.returncode == 0, plain.stderr.decode(errors="replace")
    assert plain.stdout == expected_body

    verbose = subprocess.run([str(CLIENT), "-v", target], cwd=ROOT, capture_output=True, check=False)
    assert verbose.returncode == 0, verbose.stderr.decode(errors="replace")
    assert verbose.stdout == expected_body, "verbose mode changed stdout body"
    request_hex = frame(REQUEST, 1, request_payload("/index.html")).hex(" ").upper()
    response_hex = bytes.fromhex(
        "42 54 01 02 00 00 00 18 00 00 00 01 "
        "00 C8 02 01 00 01 35 "
        "02 00 09 74 65 78 74 2F 68 74 6D 6C "
        "48 65 6C 6C 6F"
    ).hex(" ").upper()
    verbose_stderr = verbose.stderr.decode(errors="replace").replace("\n", " ").upper()
    assert request_hex in verbose_stderr
    assert response_hex in verbose_stderr

    missing = subprocess.run(
        [str(CLIENT), f"localhost:{port}/missing.html"], cwd=ROOT, capture_output=True, check=False
    )
    assert missing.returncode != 0
    assert missing.stdout == b"Not Found\n"


def check_persistent_and_malformed(port, expected_body):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        status, _, body = exchange(sock, 10, "/index.html")
        assert status == 200 and body == expected_body

        status, _, body = exchange(sock, 11, "/missing.html")
        assert status == 404 and body == b"Not Found\n"

        status, _, _ = exchange(sock, 12, "/", payload=b"\x02\x00\x01/")
        assert status == 400

        status, _, _ = exchange(sock, 13, "/", payload=b"\x01\x00\x05/")
        assert status == 400

        status, _, body = exchange(sock, 14, "/index.html")
        assert status == 200 and body == expected_body, "connection did not persist after 400/404"

        status, _, _ = exchange(sock, 15, "/../../outside")
        assert status == 400, "path traversal did not return 400"


def check_unknown_frame(port, expected_body):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        sock.sendall(frame(0x99, 77, b"opaque unknown payload"))
        sock.sendall(frame(REQUEST, 78, request_payload("/index.html")))
        _, frame_type, request_id, payload = receive_frame(sock)
        status, _, body = decode_response(payload)
        assert frame_type == RESPONSE and request_id == 78
        assert status == 200 and body == expected_body


def check_invalid_and_truncated_frames(port):
    invalid_magic = struct.pack("!HBBII", 0x1234, VERSION, REQUEST, 0, 1)
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        sock.sendall(invalid_magic)
        assert sock.recv(1) == b"", "invalid magic did not close the connection"

    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        sock.sendall(b"\x42\x54\x01")
        sock.shutdown(socket.SHUT_WR)
        assert sock.recv(1) == b"", "truncated header did not close the connection"

    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        sock.sendall(struct.pack("!HBBII", MAGIC, VERSION, REQUEST, 4, 1) + b"\x01\x00")
        sock.shutdown(socket.SHUT_WR)
        assert sock.recv(1) == b"", "truncated payload did not close the connection"


def check_symlink_escape():
    with tempfile.TemporaryDirectory() as directory:
        base = Path(directory)
        root = base / "root"
        root.mkdir()
        (root / "index.html").write_bytes(b"inside")
        (base / "outside.txt").write_bytes(b"outside")
        try:
            os.symlink(base / "outside.txt", root / "escape.txt")
        except (OSError, NotImplementedError):
            return
        process, port = start_server(root)
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
                status, _, _ = exchange(sock, 20, "/escape.txt")
                assert status == 400, "symlink escaping web root was not rejected as malformed"
        finally:
            stop_server(process)


def check_oversized_request():
    with tempfile.TemporaryDirectory() as directory:
        web_root = Path(directory)
        (web_root / "index.html").write_bytes(b"inside")
        process, port = start_server(web_root, address_space_limit=256 * 1024 * 1024)
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
                sock.settimeout(0.3)
                sock.sendall(struct.pack("!HBBII", MAGIC, VERSION, REQUEST, 0xffffffff, 90))
                try:
                    premature_data = sock.recv(1)
                    assert premature_data != b"", "server closed before the oversized payload was complete"
                except socket.timeout:
                    pass
                sock.shutdown(socket.SHUT_WR)
                assert sock.recv(1) == b"", "truncated oversized frame did not close the connection"

            assert process.poll() is None, "server crashed while draining an oversized request"

            oversized_payload = b"\x01\xff\xff/" + b"x" * 65535
            with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
                status, _, _ = exchange(sock, 91, "/", payload=oversized_payload)
                assert status == 400, "complete oversized request did not return 400"
                status, _, body = exchange(sock, 92, "/index.html")
                assert status == 200 and body == b"inside", "connection did not persist after oversized request"

            assert process.poll() is None, "server exited after an oversized request"
        finally:
            stop_server(process)


def run_client_against_status(status):
    body = b"status body"
    headers = [(1, str(len(body)).encode("ascii"))]
    if status == 200:
        headers.append((2, b"text/plain"))
    payload = struct.pack("!HB", status, len(headers))
    for header_id, value in headers:
        payload += struct.pack("!BH", header_id, len(value)) + value
    payload += body

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(3)
        port = listener.getsockname()[1]
        client = subprocess.Popen(
            [str(CLIENT), f"127.0.0.1:{port}/test"],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            connection, _ = listener.accept()
            with connection:
                receive_frame(connection)
                connection.sendall(frame(RESPONSE, 1, payload))
            stdout, stderr = client.communicate(timeout=5)
            return client.returncode, stdout, stderr
        finally:
            if client.poll() is None:
                client.kill()
                client.wait(timeout=5)


def check_client_response_statuses():
    for status in (200, 400, 404, 201, 399, 999):
        returncode, stdout, stderr = run_client_against_status(status)
        if status == 200:
            assert returncode == 0, stderr.decode(errors="replace")
            assert stdout == b"status body"
        elif status in (400, 404):
            assert returncode != 0, f"client returned success for status {status}"
            assert stdout == b"status body", f"normal {status} response body changed"
        else:
            assert returncode != 0, f"client accepted unsupported status {status}"
            assert stdout == b"", f"client wrote unsupported status {status} body to stdout"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, help="use a fixed port instead of an ephemeral port")
    args = parser.parse_args()

    web_root = ROOT / "www"
    expected_body = (web_root / "index.html").read_bytes()
    process, port = start_server(web_root, args.port)
    try:
        check_raw_spec_frames(port, expected_body)
        check_client(port, expected_body)
        check_persistent_and_malformed(port, expected_body)
        check_unknown_frame(port, expected_body)
        check_invalid_and_truncated_frames(port)
    finally:
        stop_server(process)
    check_symlink_escape()
    check_oversized_request()
    check_client_response_statuses()
    print("integration tests passed")


if __name__ == "__main__":
    main()
