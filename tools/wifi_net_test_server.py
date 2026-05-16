#!/usr/bin/env python3
"""LAN test server for walkie business testing.

The server provides:
- UDP WTK1 packet logging and same-device audio echo with a server device name.
- HTTP chunked WAV echo for AI voice tests.
- HTTP JPEG upload receiver for camera tests.
"""

from __future__ import annotations

import argparse
import http.server
import math
import struct
import socket
import socketserver
import threading
import uuid
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from urllib.parse import parse_qs, urlparse


MAGIC = b"WTK1"
HEADER_LEN = 34
DEVICE_LEN = 16
SERVER_DEVICE = b"server-echo"

# =============================================================================
# User configuration
# =============================================================================
# Device firmware should point APP_BUSINESS_SERVER_HOST to this PC's LAN IP.
# APP_BUSINESS_UDP_PORT should match DEFAULT_UDP_PORT.
# APP_BUSINESS_AI_HTTP_URL should usually be:
#   http://<PC_LAN_IP>:<DEFAULT_HTTP_PORT>/voice_chat_binary_with_audio?language=zh
# APP_BUSINESS_CAMERA_UPLOAD_URL should usually be:
#   http://<PC_LAN_IP>:<DEFAULT_HTTP_PORT>/camera/upload
DEFAULT_BIND_HOST = "0.0.0.0"
DEFAULT_UDP_PORT = 9000
DEFAULT_HTTP_PORT = 8000
DEFAULT_WAV_SAVE_DIR = Path("tools/received_wav")
DEFAULT_JPG_SAVE_DIR = Path("tools/received_jpg")
DEFAULT_CHUNK_SIZE = 32768

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


@dataclass
class JpegInfo:
    width: int | None = None
    height: int | None = None
    progressive: bool = False


@dataclass
class AiSession:
    session_id: str
    chunks: bytearray | None = None
    total: int = 0
    received: int = 0
    reply: bytes | None = None
    save_path: Path | None = None


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


def validate_and_log_wav(body: bytes, save_dir: Path, prefix: str) -> tuple[bool, Path | None]:
    wav = parse_wav(body)
    if wav is None:
        log(f"{prefix} invalid WAV len={len(body)}")
        return False, None

    pcm = body[wav.data_offset : wav.data_offset + wav.data_size]
    duration = 0.0
    if wav.sample_rate > 0 and wav.channels > 0 and wav.bits_per_sample > 0:
        bytes_per_sample = wav.channels * wav.bits_per_sample // 8
        if bytes_per_sample > 0:
            duration = wav.data_size / bytes_per_sample / wav.sample_rate

    save_path = save_wav(body, save_dir)
    stats = pcm16_stats(pcm) if wav.audio_format == 1 and wav.bits_per_sample == 16 else "pcm_stats=unsupported"
    log(
        f"{prefix} WAV fmt={wav.audio_format} ch={wav.channels} rate={wav.sample_rate} "
        f"bits={wav.bits_per_sample} data={wav.data_size} duration={duration:.2f}s "
        f"{stats} saved={save_path}"
    )
    return True, save_path


def parse_jpeg(body: bytes) -> JpegInfo | None:
    if len(body) < 4 or body[:2] != b"\xFF\xD8" or body[-2:] != b"\xFF\xD9":
        return None

    pos = 2
    while pos + 4 <= len(body):
        if body[pos] != 0xFF:
            pos += 1
            continue
        while pos < len(body) and body[pos] == 0xFF:
            pos += 1
        if pos >= len(body):
            break

        marker = body[pos]
        pos += 1
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            continue
        if pos + 2 > len(body):
            return None

        segment_len = read_u16_be(body, pos)
        if segment_len < 2 or pos + segment_len > len(body):
            return None

        if marker in (0xC0, 0xC1, 0xC2):
            if segment_len < 7:
                return None
            height = read_u16_be(body, pos + 3)
            width = read_u16_be(body, pos + 5)
            return JpegInfo(width=width, height=height, progressive=(marker == 0xC2))

        pos += segment_len

    return JpegInfo()


def read_u16_be(data: bytes, offset: int) -> int:
    return (data[offset] << 8) | data[offset + 1]


def save_jpeg(body: bytes, save_dir: Path) -> Path:
    save_dir.mkdir(parents=True, exist_ok=True)
    path = save_dir / f"camera_upload_{datetime.now().strftime('%Y%m%d_%H%M%S_%f')}.jpg"
    path.write_bytes(body)
    return path


def save_camera_raw(body: bytes, save_dir: Path) -> Path:
    save_dir.mkdir(parents=True, exist_ok=True)
    path = save_dir / f"camera_upload_invalid_{datetime.now().strftime('%Y%m%d_%H%M%S_%f')}.bin"
    path.write_bytes(body)
    return path


def validate_and_log_jpeg(body: bytes, save_dir: Path, prefix: str) -> tuple[bool, Path | None, JpegInfo | None]:
    jpeg = parse_jpeg(body)
    if jpeg is None:
        save_path = save_camera_raw(body, save_dir)
        log(
            f"{prefix} invalid JPEG len={len(body)} "
            f"soi={body[:2].hex()} eoi={body[-2:].hex() if len(body) >= 2 else ''} "
            f"saved_raw={save_path}"
        )
        return False, save_path, None

    save_path = save_jpeg(body, save_dir)
    size_text = f"{jpeg.width}x{jpeg.height}" if jpeg.width and jpeg.height else "unknown"
    log(
        f"{prefix} JPEG len={len(body)} size={size_text} "
        f"progressive={int(jpeg.progressive)} saved={save_path}"
    )
    return True, save_path, jpeg


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
        parsed = urlparse(self.path)
        params = parse_qs(parsed.query)
        op = params.get("op", [""])[0]
        content_type = self.headers.get("Content-Type", "")
        log(f"HTTP POST {self.path} len={len(body)} content_type={content_type!r} op={op!r}")

        if op:
            self.handle_chunked_ai(op, params, body)
            return

        if parsed.path == "/camera/upload":
            self.handle_camera_upload(body, content_type)
            return

        # Backward-compatible old one-shot WAV echo endpoint.
        if parsed.path != "/ai/wav":
            self.send_error(404)
            return

        wav = parse_wav(body)
        valid_wav = wav is not None
        if not valid_wav:
            self.send_bytes(400, b"expected audio/wav", "text/plain")
            return

        validate_and_log_wav(body, self.server.save_dir, "HTTP one-shot")
        self.send_bytes(200, body, "audio/wav")

    def handle_camera_upload(self, body: bytes, content_type: str) -> None:
        if "image/jpeg" not in content_type.lower() and "image/jpg" not in content_type.lower():
            log(f"Camera upload content-type warning: {content_type!r}")

        ok, save_path, jpeg = validate_and_log_jpeg(body, self.server.jpg_save_dir, "Camera upload")
        if not ok or save_path is None:
            self.send_json(
                400,
                (
                    '{"ok":false,'
                    '"error":"invalid jpeg",'
                    f'"len":{len(body)},'
                    f'"file":"{save_path.as_posix() if save_path else ""}"'
                    '}'
                ),
            )
            return

        width = jpeg.width if jpeg and jpeg.width is not None else 0
        height = jpeg.height if jpeg and jpeg.height is not None else 0
        self.send_json(
            200,
            (
                '{"ok":true,'
                f'"len":{len(body)},'
                f'"width":{width},'
                f'"height":{height},'
                f'"file":"{save_path.as_posix()}"'
                '}'
            ),
        )

    def handle_chunked_ai(self, op: str, params: dict[str, list[str]], body: bytes) -> None:
        if op == "start":
            session_id = uuid.uuid4().hex[:12]
            self.server.ai_sessions[session_id] = AiSession(session_id=session_id)
            log(f"AI start session={session_id}")
            self.send_json(200, f'{{"session":"{session_id}","chunk_size":{DEFAULT_CHUNK_SIZE}}}')
            return

        session_id = params.get("session", [""])[0]
        session = self.server.ai_sessions.get(session_id)
        if not session:
            self.send_json(404, '{"ok":false,"error":"unknown session"}')
            return

        if op == "upload":
            try:
                index = int(params.get("index", ["0"])[0])
                offset = int(params.get("offset", ["0"])[0])
                total = int(params.get("total", ["0"])[0])
            except ValueError:
                self.send_json(400, '{"ok":false,"error":"bad upload params"}')
                return
            if total <= 0 or offset < 0 or offset + len(body) > total:
                self.send_json(400, '{"ok":false,"error":"upload range invalid"}')
                return
            if session.chunks is None:
                session.total = total
                session.chunks = bytearray(total)
            if total != session.total or session.chunks is None:
                self.send_json(409, '{"ok":false,"error":"total changed"}')
                return
            session.chunks[offset : offset + len(body)] = body
            session.received += len(body)
            log(
                f"AI upload session={session_id} index={index} offset={offset} "
                f"len={len(body)} received={session.received}/{session.total}"
            )
            self.send_json(200, '{"ok":true}')
            return

        if op == "finish":
            if session.chunks is None or session.total <= 0:
                self.send_json(400, '{"ok":false,"error":"no upload"}')
                return
            if session.received < session.total:
                self.send_json(409, '{"ok":false,"error":"upload incomplete"}')
                return
            full_wav = bytes(session.chunks)
            ok, save_path = validate_and_log_wav(full_wav, self.server.save_dir, f"AI finish session={session_id}")
            if not ok:
                self.send_json(400, '{"ok":false,"error":"invalid wav"}')
                return
            # Link test: echo uploaded WAV as AI reply. This validates upload,
            # polling, chunk download and device-side playback without a real AI backend.
            session.reply = full_wav
            session.save_path = save_path
            self.send_json(200, '{"ok":true,"status":"processing"}')
            return

        if op == "result_info":
            if session.reply is None:
                self.send_json(200, '{"ready":false}')
                return
            self.send_json(200, f'{{"ready":true,"total":{len(session.reply)},"format":"wav"}}')
            return

        if op == "result_chunk":
            if session.reply is None:
                self.send_bytes(409, b"not ready", "text/plain")
                return
            try:
                offset = int(params.get("offset", ["0"])[0])
                req_len = int(params.get("len", [str(DEFAULT_CHUNK_SIZE)])[0])
            except ValueError:
                self.send_bytes(400, b"bad result_chunk params", "text/plain")
                return
            if offset < 0 or req_len <= 0 or offset >= len(session.reply):
                self.send_bytes(416, b"range invalid", "text/plain")
                return
            chunk = session.reply[offset : offset + req_len]
            log(f"AI result_chunk session={session_id} offset={offset} len={len(chunk)}")
            self.send_bytes(200, chunk, "application/octet-stream")
            return

        self.send_json(400, '{"ok":false,"error":"unknown op"}')

    def send_json(self, status: int, text: str) -> None:
        self.send_bytes(status, text.encode("utf-8"), "application/json")

    def send_bytes(self, status: int, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt: str, *args: object) -> None:
        return


def run_http(host: str, port: int, wav_save_dir: Path, jpg_save_dir: Path) -> None:
    class ThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
        daemon_threads = True

    try:
        server = ThreadingHTTPServer((host, port), AiWavHandler)
    except OSError as exc:
        log(f"HTTP bind failed on {host}:{port}: {exc}")
        return

    server.save_dir = wav_save_dir
    server.jpg_save_dir = jpg_save_dir
    server.ai_sessions = {}
    log(f"HTTP AI WAV + camera JPEG test listening on {host}:{port}")
    log(f"Camera upload URL: http://<PC_LAN_IP>:{port}/camera/upload")
    server.serve_forever()


def main() -> None:
    parser = argparse.ArgumentParser(description="Walkie business test server")
    parser.add_argument("--host", default=DEFAULT_BIND_HOST, help="bind address")
    parser.add_argument("--udp-port", type=int, default=DEFAULT_UDP_PORT, help="WTK1 UDP listen port")
    parser.add_argument("--http-port", type=int, default=DEFAULT_HTTP_PORT, help="AI WAV HTTP port")
    parser.add_argument("--wav-save-dir", default=str(DEFAULT_WAV_SAVE_DIR), help="directory for received WAV files")
    parser.add_argument("--jpg-save-dir", default=str(DEFAULT_JPG_SAVE_DIR), help="directory for received JPEG files")
    args = parser.parse_args()

    threading.Thread(target=run_udp, args=(args.host, args.udp_port), daemon=True).start()
    threading.Thread(
        target=run_http,
        args=(args.host, args.http_port, Path(args.wav_save_dir), Path(args.jpg_save_dir)),
        daemon=True,
    ).start()

    log("Press Ctrl+C to stop")
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        log("Stopped")


if __name__ == "__main__":
    main()
