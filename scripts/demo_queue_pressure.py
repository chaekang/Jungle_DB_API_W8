import json
import socket
import time

HOST = "127.0.0.1"
PORT = 8080
WORKERS = 8
QUEUE_CAPACITY = 32
BLOCKING_CONNECTIONS = WORKERS + QUEUE_CAPACITY
EXTRA_REQUESTS = 8


def make_partial_request(table_name: str) -> bytes:
    sql = f"INSERT INTO {table_name} (name, age) VALUES ('blocked', 20);"
    body = json.dumps({"sql": sql})
    declared_length = len(body.encode("utf-8")) + 32
    request = (
        "POST /query HTTP/1.1\r\n"
        f"Host: {HOST}:{PORT}\r\n"
        "Content-Type: application/json\r\n"
        f"Content-Length: {declared_length}\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
    )
    return request.encode("utf-8")


def make_full_request(table_name: str, index: int) -> bytes:
    sql = f"INSERT INTO {table_name} (name, age) VALUES ('probe_{index}', 30);"
    body = json.dumps({"sql": sql}).encode("utf-8")
    request = (
        "POST /query HTTP/1.1\r\n"
        f"Host: {HOST}:{PORT}\r\n"
        "Content-Type: application/json\r\n"
        f"Content-Length: {len(body)}\r\n"
        "Connection: close\r\n"
        "\r\n"
    ).encode("utf-8")
    return request + body


def read_response(sock: socket.socket) -> str:
    chunks = []
    while True:
        try:
            data = sock.recv(4096)
        except OSError:
            break
        if not data:
            break
        chunks.append(data)
    return b"".join(chunks).decode("utf-8", errors="replace")


def open_blocking_connections(table_name: str):
    sockets = []
    payload = make_partial_request(table_name)

    for i in range(BLOCKING_CONNECTIONS):
        sock = socket.create_connection((HOST, PORT), timeout=3)
        sock.sendall(payload)
        sockets.append(sock)
        print(f"blocked connection opened: {i + 1}/{BLOCKING_CONNECTIONS}", flush=True)

    return sockets


def run_probe_requests(table_name: str):
    busy_count = 0
    other_count = 0

    for i in range(1, EXTRA_REQUESTS + 1):
        sock = socket.create_connection((HOST, PORT), timeout=3)
        try:
            sock.sendall(make_full_request(table_name, i))
            response = read_response(sock)
        finally:
            sock.close()

        first_line = response.splitlines()[0] if response else "(no response)"
        print(f"probe {i}: {first_line}", flush=True)

        if (not response or
                "503 Service Unavailable" in response or
                "Server is busy." in response):
            busy_count += 1
        else:
            other_count += 1

    return busy_count, other_count


def release_connections(blockers):
    for sock in blockers:
        try:
            sock.close()
        except OSError:
            pass


def main():
    table_name = f"demo_queue_{int(time.time())}"
    print(f"host: {HOST}:{PORT}")
    print(f"table: {table_name}")
    print(f"workers: {WORKERS}")
    print(f"queue capacity: {QUEUE_CAPACITY}")
    print(f"blocking connections: {BLOCKING_CONNECTIONS}")
    print(f"extra probe requests: {EXTRA_REQUESTS}")
    print("opening blocking connections...")

    blockers = open_blocking_connections(table_name)
    print("all blocking connections opened")
    time.sleep(1.0)

    print("sending extra probe requests...")
    busy_count, other_count = run_probe_requests(table_name)

    print("releasing blocking connections...")
    release_connections(blockers)
    time.sleep(1.0)

    print(f"busy responses: {busy_count}")
    print(f"non-busy responses: {other_count}")
    print("ok:", busy_count > 0)


if __name__ == "__main__":
    main()
