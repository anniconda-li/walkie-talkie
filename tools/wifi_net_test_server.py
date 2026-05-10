#!/usr/bin/env python3
"""Simple LAN TCP/UDP echo server for WiFi smoke testing."""

from __future__ import annotations

import argparse
import socket
import threading
from datetime import datetime


def log(message: str) -> None:
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {message}", flush=True)


def run_udp(host: str, port: int) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((host, port))
    log(f"UDP listening on {host}:{port}")

    while True:
        data, addr = sock.recvfrom(4096)
        log(f"UDP from {addr[0]}:{addr[1]} len={len(data)} data={data!r}")
        sock.sendto(b"udp-echo:" + data, addr)


def run_tcp(host: str, port: int) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((host, port))
    sock.listen(8)
    log(f"TCP listening on {host}:{port}")

    while True:
        conn, addr = sock.accept()
        with conn:
            data = conn.recv(4096)
            log(f"TCP from {addr[0]}:{addr[1]} len={len(data)} data={data!r}")
            if data:
                conn.sendall(b"tcp-ack:" + data)


def main() -> None:
    parser = argparse.ArgumentParser(description="WiFi TCP/UDP test server")
    parser.add_argument("--host", default="0.0.0.0", help="bind address")
    parser.add_argument("--udp-port", type=int, default=33333, help="UDP listen port")
    parser.add_argument("--tcp-port", type=int, default=33334, help="TCP listen port")
    args = parser.parse_args()

    threading.Thread(target=run_udp, args=(args.host, args.udp_port), daemon=True).start()
    threading.Thread(target=run_tcp, args=(args.host, args.tcp_port), daemon=True).start()

    log("Press Ctrl+C to stop")
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        log("Stopped")


if __name__ == "__main__":
    main()
