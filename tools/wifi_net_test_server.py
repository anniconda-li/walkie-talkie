#!/usr/bin/env python3
"""LAN test server for walkie business testing.

The server provides:
- UDP WTK1 packet logging and same-device audio echo with a server device name.
- HTTP WAV echo for AI voice tests.
"""

from __future__ import annotations

import argparse
import http.server
import socket
import socketserver
import threading
from dataclasses import dataclass
from datetime import datetime


MAGIC = b"WTK1"
HEADER_LEN = 34
DEVICE_LEN = 16
SERVER_DEVICE = b"server-echo"

PKT_TYPES = {
    1: "register",
    2: "channel",
    3: "ptt_start",
    4: "audio",
    5: "ptt_stop",
    6: "heartbeat",
}


@dataclass
class Packet:
    packet_type: int
    channel: int
    seq: int
    timestamp_ms: int
    device: str
    payload: bytes


def log(message: str) -> None:
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {message}", flush=True)


def read_u16(data: bytes, offset: int) -> int:
    return data[offset] | (data[offset + 1] << 8)


def read_u32(data: bytes, offset: int) -> int:
    return (
        data[offset]
        | (data[offset + 1] << 8)
        | (data[offset + 2] << 16)
        | (data[offset + 3] << 24)
    )


def parse_packet(data: bytes) -> Packet | None:
    if len(data) < HEADER_LEN or data[:4] != MAGIC:
        return None

    header_len = data[5]
    payload_len = read_u16(data, 32)
    if header_len != HEADER_LEN or len(data) < header_len + payload_len:
        return None

    device_raw = data[16:32].split(b"\x00", 1)[0]
    return Packet(
        packet_type=data[4],
        channel=read_u16(data, 6),
        seq=read_u32(data, 8),
        timestamp_ms=read_u32(data, 12),
        device=device_raw.decode("utf-8", errors="replace"),
        payload=data[header_len : header_len + payload_len],
    )


def make_server_echo(data: bytes) -> bytes:
    out = bytearray(data)
    out[16:32] = b"\x00" * DEVICE_LEN
    out[16 : 16 + len(SERVER_DEVICE)] = SERVER_DEVICE
    return bytes(out)


def run_udp(host: str, port: int) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind((host, port))
    except OSError as exc:
        log(f"UDP bind failed on {host}:{port}: {exc}")
        return
    log(f"UDP WTK1 listening on {host}:{port}")

    devices: dict[str, tuple[str, int, int]] = {}

    while True:
        data, addr = sock.recvfrom(2048)
        packet = parse_packet(data)
        if packet is None:
            log(f"UDP raw from {addr[0]}:{addr[1]} len={len(data)} data={data!r}")
            continue

        type_name = PKT_TYPES.get(packet.packet_type, f"type_{packet.packet_type}")
        devices[packet.device] = (addr[0], addr[1], packet.channel)
        log(
            f"UDP {type_name} from {packet.device}@{addr[0]}:{addr[1]} "
            f"ch={packet.channel} seq={packet.seq} payload={len(packet.payload)}"
        )

        if packet.packet_type == 4 and packet.payload:
            targets = [
                (dev, dev_addr)
                for dev, (ip, port, channel) in devices.items()
                if dev != packet.device and channel == packet.channel
                for dev_addr in [(ip, port)]
            ]
            if targets:
                for dev, dev_addr in targets:
                    sock.sendto(data, dev_addr)
                    log(f"UDP audio forwarded to {dev}@{dev_addr[0]}:{dev_addr[1]}")
            else:
                # A single-device business test needs a downlink packet whose
                # device field is not the local device name, otherwise the
                # client drops it.
                sock.sendto(make_server_echo(data), addr)


class AiWavHandler(http.server.BaseHTTPRequestHandler):
    server_version = "WalkieTestHTTP/1.0"

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        valid_wav = body.startswith(b"RIFF") and len(body) >= 44
        log(f"HTTP POST {self.path} len={len(body)} wav={valid_wav}")

        if self.path != "/ai/wav":
            self.send_response(404)
            self.end_headers()
            return

        if not valid_wav:
            self.send_response(400)
            self.end_headers()
            self.wfile.write(b"expected audio/wav")
            return

        # Echo the uploaded WAV so the device can verify upload + response playback.
        self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt: str, *args: object) -> None:
        return


def run_http(host: str, port: int) -> None:
    class ThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
        daemon_threads = True

    try:
        server = ThreadingHTTPServer((host, port), AiWavHandler)
    except OSError as exc:
        log(f"HTTP bind failed on {host}:{port}: {exc}")
        return

    log(f"HTTP WAV echo listening on {host}:{port}")
    server.serve_forever()


def main() -> None:
    parser = argparse.ArgumentParser(description="Walkie business test server")
    parser.add_argument("--host", default="0.0.0.0", help="bind address")
    parser.add_argument("--udp-port", type=int, default=9000, help="WTK1 UDP listen port")
    parser.add_argument("--http-port", type=int, default=18080, help="AI WAV HTTP port")
    args = parser.parse_args()

    threading.Thread(target=run_udp, args=(args.host, args.udp_port), daemon=True).start()
    threading.Thread(target=run_http, args=(args.host, args.http_port), daemon=True).start()

    log("Press Ctrl+C to stop")
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        log("Stopped")


if __name__ == "__main__":
    main()
