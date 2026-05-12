#!/usr/bin/env python3
"""LAN test server for walkie business testing.

The server provides:
- UDP WTK1 packet logging and same-device audio echo with a server device name.
- HTTP WAV echo for AI voice tests.
"""

from __future__ import annotations

import argparse
import http.server
import math
import struct
import socket
import socketserver
import threading
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path


MAGIC = b"WTK1"
HEADER_LEN = 34
DEVICE_LEN = 16
SERVER_DEVICE = b"server-echo"
DEFAULT_SAVE_DIR = Path("tools/received_wav")

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


@dataclass
class WavInfo:
    audio_format: int
    channels: int
    sample_rate: int
    bits_per_sample: int
    data_offset: int
    data_size: int


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


def parse_wav(body: bytes) -> WavInfo | None:
    if len(body) < 44 or body[:4] != b"RIFF" or body[8:12] != b"WAVE":
        return None

    pos = 12
    audio_format = channels = sample_rate = bits_per_sample = None
    data_offset = data_size = None

    while pos + 8 <= len(body):
        chunk_id = body[pos : pos + 4]
        chunk_size = read_u32(body, pos + 4)
        chunk_data = pos + 8
        chunk_end = chunk_data + chunk_size
        if chunk_end > len(body):
            return None

        if chunk_id == b"fmt ":
            if chunk_size < 16:
                return None
            audio_format, channels, sample_rate, _byte_rate, _block_align, bits_per_sample = struct.unpack_from(
                "<HHIIHH", body, chunk_data
            )
        elif chunk_id == b"data":
            data_offset = chunk_data
            data_size = chunk_size
            break

        pos = chunk_end + (chunk_size & 1)

    if (
        audio_format is None
        or channels is None
        or sample_rate is None
        or bits_per_sample is None
        or data_offset is None
        or data_size is None
    ):
        return None

    return WavInfo(
        audio_format=audio_format,
        channels=channels,
        sample_rate=sample_rate,
        bits_per_sample=bits_per_sample,
        data_offset=data_offset,
        data_size=data_size,
    )


def pcm16_stats(pcm: bytes) -> str:
    sample_count = len(pcm) // 2
    if sample_count == 0:
        return "samples=0"

    samples = struct.unpack_from(f"<{sample_count}h", pcm[: sample_count * 2])
    min_v = min(samples)
    max_v = max(samples)
    mean = sum(samples) / sample_count
    rms = math.sqrt(sum(s * s for s in samples) / sample_count)
    peak = max(abs(min_v), abs(max_v))
    clipped = sum(1 for s in samples if s <= -32760 or s >= 32760)
    zero_cross = sum(
        1
        for prev, cur in zip(samples, samples[1:])
        if (prev < 0 <= cur) or (prev > 0 >= cur)
    )
    zcr = zero_cross / max(sample_count - 1, 1)

    return (
        f"samples={sample_count} min={min_v} max={max_v} "
        f"mean={mean:.1f} rms={rms:.1f} peak={peak} "
        f"clipped={clipped} zcr={zcr:.3f}"
    )


def save_wav(body: bytes, save_dir: Path) -> Path:
    save_dir.mkdir(parents=True, exist_ok=True)
    path = save_dir / f"ai_upload_{datetime.now().strftime('%Y%m%d_%H%M%S_%f')}.wav"
    path.write_bytes(body)
    return path


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
        wav = parse_wav(body)
        valid_wav = wav is not None
        content_type = self.headers.get("Content-Type", "")
        log(f"HTTP POST {self.path} len={len(body)} content_type={content_type!r} wav={valid_wav}")

        if self.path != "/ai/wav":
            self.send_response(404)
            self.end_headers()
            return

        if not valid_wav:
            self.send_response(400)
            self.end_headers()
            self.wfile.write(b"expected audio/wav")
            return

        assert wav is not None
        pcm = body[wav.data_offset : wav.data_offset + wav.data_size]
        duration = 0.0
        if wav.sample_rate > 0 and wav.channels > 0 and wav.bits_per_sample > 0:
            bytes_per_sample = wav.channels * wav.bits_per_sample // 8
            if bytes_per_sample > 0:
                duration = wav.data_size / bytes_per_sample / wav.sample_rate

        save_path = save_wav(body, self.server.save_dir)
        stats = pcm16_stats(pcm) if wav.audio_format == 1 and wav.bits_per_sample == 16 else "pcm_stats=unsupported"
        log(
            "WAV "
            f"fmt={wav.audio_format} ch={wav.channels} rate={wav.sample_rate} "
            f"bits={wav.bits_per_sample} data={wav.data_size} duration={duration:.2f}s "
            f"{stats} saved={save_path}"
        )

        # Echo the uploaded WAV so the device can verify upload + response playback.
        self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt: str, *args: object) -> None:
        return


def run_http(host: str, port: int, save_dir: Path) -> None:
    class ThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
        daemon_threads = True

    try:
        server = ThreadingHTTPServer((host, port), AiWavHandler)
    except OSError as exc:
        log(f"HTTP bind failed on {host}:{port}: {exc}")
        return

    server.save_dir = save_dir
    log(f"HTTP WAV echo listening on {host}:{port}")
    server.serve_forever()


def main() -> None:
    parser = argparse.ArgumentParser(description="Walkie business test server")
    parser.add_argument("--host", default="0.0.0.0", help="bind address")
    parser.add_argument("--udp-port", type=int, default=9000, help="WTK1 UDP listen port")
    parser.add_argument("--http-port", type=int, default=18080, help="AI WAV HTTP port")
    parser.add_argument("--save-dir", default=str(DEFAULT_SAVE_DIR), help="directory for received WAV files")
    args = parser.parse_args()

    threading.Thread(target=run_udp, args=(args.host, args.udp_port), daemon=True).start()
    threading.Thread(
        target=run_http,
        args=(args.host, args.http_port, Path(args.save_dir)),
        daemon=True,
    ).start()

    log("Press Ctrl+C to stop")
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        log("Stopped")


if __name__ == "__main__":
    main()
