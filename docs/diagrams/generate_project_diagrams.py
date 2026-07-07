from __future__ import annotations

from pathlib import Path
from typing import Iterable

from PIL import Image, ImageDraw, ImageFont


OUT_DIR = Path(__file__).resolve().parent
FONT_REGULAR = Path("C:/Windows/Fonts/msyh.ttc")
FONT_BOLD = Path("C:/Windows/Fonts/msyhbd.ttc")

COLORS = {
    "ink": "#172033",
    "muted": "#5d6678",
    "line": "#2e3648",
    "panel": "#f7f8fb",
    "box": "#ffffff",
    "blue": "#eaf3ff",
    "green": "#edf8f1",
    "orange": "#fff4e6",
    "purple": "#f4efff",
    "red": "#fff1f1",
    "gray": "#eef1f6",
}


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    path = FONT_BOLD if bold and FONT_BOLD.exists() else FONT_REGULAR
    return ImageFont.truetype(str(path), size)


def text_size(draw: ImageDraw.ImageDraw, text: str, ft: ImageFont.FreeTypeFont) -> tuple[int, int]:
    box = draw.textbbox((0, 0), text, font=ft)
    return box[2] - box[0], box[3] - box[1]


def wrap_line(draw: ImageDraw.ImageDraw, text: str, ft: ImageFont.FreeTypeFont, max_width: int) -> list[str]:
    parts: list[str] = []
    buf = ""
    for ch in text:
        test = buf + ch
        if buf and text_size(draw, test, ft)[0] > max_width:
            parts.append(buf)
            buf = ch
        else:
            buf = test
    if buf:
        parts.append(buf)
    return parts or [""]


def wrap_text(draw: ImageDraw.ImageDraw, text: str, ft: ImageFont.FreeTypeFont, max_width: int) -> list[str]:
    lines: list[str] = []
    for raw in text.split("\n"):
        lines.extend(wrap_line(draw, raw, ft, max_width))
    return lines


def draw_multiline_center(
    draw: ImageDraw.ImageDraw,
    rect: tuple[int, int, int, int],
    text: str,
    ft: ImageFont.FreeTypeFont,
    fill: str = COLORS["ink"],
    leading: int = 8,
) -> None:
    x1, y1, x2, y2 = rect
    lines = wrap_text(draw, text, ft, x2 - x1 - 22)
    heights = [text_size(draw, line, ft)[1] for line in lines]
    total_h = sum(heights) + leading * (len(lines) - 1)
    y = y1 + ((y2 - y1) - total_h) / 2
    for line, h in zip(lines, heights):
        w, _ = text_size(draw, line, ft)
        draw.text((x1 + ((x2 - x1) - w) / 2, y), line, font=ft, fill=fill)
        y += h + leading


def box(
    draw: ImageDraw.ImageDraw,
    rect: tuple[int, int, int, int],
    text: str,
    *,
    fill: str = COLORS["box"],
    outline: str = COLORS["line"],
    width: int = 2,
    text_size_px: int = 22,
    bold: bool = False,
) -> None:
    draw.rounded_rectangle(rect, radius=6, fill=fill, outline=outline, width=width)
    draw_multiline_center(draw, rect, text, font(text_size_px, bold=bold))


def panel(
    draw: ImageDraw.ImageDraw,
    rect: tuple[int, int, int, int],
    title: str,
    *,
    fill: str = COLORS["panel"],
) -> None:
    draw.rounded_rectangle(rect, radius=8, fill=fill, outline=COLORS["line"], width=2)
    draw.text((rect[0] + 18, rect[1] + 14), title, font=font(22, bold=True), fill=COLORS["ink"])


def arrow(
    draw: ImageDraw.ImageDraw,
    start: tuple[int, int],
    end: tuple[int, int],
    *,
    color: str = COLORS["line"],
    width: int = 3,
) -> None:
    draw.line([start, end], fill=color, width=width)
    sx, sy = start
    ex, ey = end
    dx = ex - sx
    dy = ey - sy
    if abs(dx) >= abs(dy):
        sign = 1 if dx >= 0 else -1
        points = [(ex, ey), (ex - sign * 14, ey - 8), (ex - sign * 14, ey + 8)]
    else:
        sign = 1 if dy >= 0 else -1
        points = [(ex, ey), (ex - 8, ey - sign * 14), (ex + 8, ey - sign * 14)]
    draw.polygon(points, fill=color)


def elbow(
    draw: ImageDraw.ImageDraw,
    points: Iterable[tuple[int, int]],
    *,
    color: str = COLORS["line"],
    width: int = 3,
) -> None:
    pts = list(points)
    if len(pts) < 2:
        return
    for a, b in zip(pts, pts[1:]):
        draw.line([a, b], fill=color, width=width)
    arrow(draw, pts[-2], pts[-1], color=color, width=width)


def header(draw: ImageDraw.ImageDraw, title: str, subtitle: str, w: int) -> None:
    draw.text((40, 26), title, font=font(36, bold=True), fill=COLORS["ink"])
    draw.text((42, 78), subtitle, font=font(20), fill=COLORS["muted"])
    draw.line([(40, 112), (w - 40, 112)], fill="#d9dee8", width=2)


def save(img: Image.Image, name: str) -> None:
    path = OUT_DIR / name
    img.save(path)
    print(path)


def architecture_diagram() -> None:
    w, h = 1900, 1080
    img = Image.new("RGB", (w, h), "white")
    draw = ImageDraw.Draw(img)
    header(
        draw,
        "ESP32 对讲设备与 AI 后端整体结构图",
        "当前代码：LVGL UI → app 业务 → service 门面 → device driver；AI/相机走 HTTP，实时对讲走 WTK1 UDP + 20ms PCM。",
        w,
    )

    panel(draw, (40, 225, 275, 500), "用户与 UI 层", fill="#fbfcff")
    panel(draw, (325, 175, 600, 650), "业务编排层", fill="#fbfcff")
    panel(draw, (650, 145, 930, 850), "app 业务模块层", fill="#fbfcff")
    panel(draw, (980, 145, 1265, 850), "service 能力层", fill="#fbfcff")
    panel(draw, (1315, 145, 1595, 850), "device 驱动适配层", fill="#fbfcff")
    panel(draw, (1645, 145, 1860, 850), "后端服务", fill="#fbfcff")

    ui_ops = (65, 300, 250, 370)
    ui_lvgl = (65, 405, 250, 475)
    box(draw, ui_ops, "用户操作\nPTT / AI / 相机 / 设置", fill=COLORS["blue"])
    box(draw, ui_lvgl, "LVGL UI\nui_shell\nui_event", fill=COLORS["blue"], text_size_px=20)
    arrow(draw, ((ui_ops[0] + ui_ops[2]) // 2, ui_ops[3]), ((ui_lvgl[0] + ui_lvgl[2]) // 2, ui_lvgl[1]))

    business = (355, 315, 570, 510)
    box(
        draw,
        business,
        "app_business\n启动顺序编排\nUI 事件转发\n音频会话互斥\n音量 / 亮度设置",
        fill=COLORS["green"],
        text_size_px=21,
        bold=True,
    )
    arrow(draw, (ui_lvgl[2], (ui_lvgl[1] + ui_lvgl[3]) // 2), (business[0], (business[1] + business[3]) // 2))

    app_boxes = {
        "intercom": (685, 205, 895, 285, "app_intercom\n实时 UDP 对讲\nWTK1 20ms PCM"),
        "ai": (685, 335, 895, 430, "app_ai_voice\nAI 语音问答\nWAV 录音\n轮询 / 拉取播放"),
        "camera": (685, 480, 895, 570, "app_camera\nRGB565 预览\nJPEG 拍照上传"),
        "network": (685, 620, 895, 700, "app_network\nWiFi / 伪4G切换\nNVS 保存恢复"),
        "status": (685, 735, 895, 815, "app_status_monitor\n电量 / 网络状态\n掉线恢复"),
    }
    for rect in app_boxes.values():
        box(draw, rect[:4], rect[4], fill=COLORS["box"], text_size_px=18)
        elbow(draw, [(business[2], (business[1] + business[3]) // 2), (635, (rect[1] + rect[3]) // 2), (rect[0], (rect[1] + rect[3]) // 2)])

    service_boxes = {
        "network": (1010, 210, 1235, 315, "service_network\nUDP send/read\nHTTP POST\n网络 I/O 互斥"),
        "audio": (1010, 365, 1235, 455, "service_audio\nES8311 采集/播放\n音量控制"),
        "camera": (1010, 505, 1235, 585, "service_camera\nRGB565 预览\nJPEG 帧获取"),
        "screen": (1010, 625, 1235, 705, "service_screen\nLCD / LVGL\n背光 / 亮度"),
        "init": (1010, 745, 1235, 820, "service_init\n绑定 driver ops\n当前 WiFi 后端"),
    }
    for rect in service_boxes.values():
        box(draw, rect[:4], rect[4], fill=COLORS["orange"], text_size_px=18)

    def mid(r: tuple[int, int, int, int, str]) -> int:
        return (r[1] + r[3]) // 2

    elbow(draw, [(895, mid(app_boxes["intercom"])), (965, mid(app_boxes["intercom"])), (965, mid(service_boxes["network"])), (1010, mid(service_boxes["network"]))])
    elbow(draw, [(895, mid(app_boxes["intercom"])), (950, mid(app_boxes["intercom"])), (950, mid(service_boxes["audio"])), (1010, mid(service_boxes["audio"]))])
    elbow(draw, [(895, mid(app_boxes["ai"])), (965, mid(app_boxes["ai"])), (965, mid(service_boxes["network"])), (1010, mid(service_boxes["network"]))])
    elbow(draw, [(895, mid(app_boxes["ai"])), (950, mid(app_boxes["ai"])), (950, mid(service_boxes["audio"])), (1010, mid(service_boxes["audio"]))])
    elbow(draw, [(895, mid(app_boxes["camera"])), (960, mid(app_boxes["camera"])), (960, mid(service_boxes["camera"])), (1010, mid(service_boxes["camera"]))])
    elbow(draw, [(895, mid(app_boxes["camera"])), (970, mid(app_boxes["camera"])), (970, mid(service_boxes["network"])), (1010, mid(service_boxes["network"]))])
    elbow(draw, [(895, mid(app_boxes["network"])), (950, mid(app_boxes["network"])), (950, mid(service_boxes["init"])), (1010, mid(service_boxes["init"]))])
    elbow(draw, [(895, mid(app_boxes["status"])), (965, mid(app_boxes["status"])), (965, mid(service_boxes["screen"])), (1010, mid(service_boxes["screen"]))])
    elbow(draw, [(895, mid(app_boxes["status"])), (950, mid(app_boxes["status"])), (950, mid(service_boxes["network"])), (1010, mid(service_boxes["network"]))])

    driver_boxes = {
        "wifi": (1345, 215, 1565, 320, "d_wifi\nWiFi STA / 伪4G热点\nUDP / HTTP\n下行读取"),
        "audio": (1345, 365, 1565, 445, "d_es8311\nI2S 音频\nI2C 寄存器"),
        "camera": (1345, 505, 1565, 585, "d_camera\nOV2640\nDVP / SCCB"),
        "lcd": (1345, 625, 1565, 725, "d_lcd\nd_power_control\nLCD / 背光 / PCA9557\nSPI / I2C / GPIO"),
    }
    for rect in driver_boxes.values():
        box(draw, rect[:4], rect[4], fill=COLORS["purple"], text_size_px=17)

    arrow(draw, (1235, mid(service_boxes["network"])), (1345, mid(driver_boxes["wifi"])))
    arrow(draw, (1235, mid(service_boxes["audio"])), (1345, mid(driver_boxes["audio"])))
    arrow(draw, (1235, mid(service_boxes["camera"])), (1345, mid(driver_boxes["camera"])))
    arrow(draw, (1235, mid(service_boxes["screen"])), (1345, mid(driver_boxes["lcd"])))
    elbow(draw, [(1235, mid(service_boxes["init"])), (1290, mid(service_boxes["init"])), (1290, 270), (1345, 270)])

    backend_boxes = {
        "udp": (1670, 205, 1835, 305, "UDP 转发服务\nWTK1 原样转发\nAUDIO=20ms\n640B PCM"),
        "fastapi": (1670, 355, 1835, 490, "FastAPI HTTP\n/ai/* 语音协议\n/camera/upload\n按 device 隔离上下文"),
        "ai": (1670, 545, 1835, 705, "AI 能力\nASR Paraformer\nLLM + memory\nTTS 设备 WAV\nQwen-VL 视觉识别"),
    }
    for rect in backend_boxes.values():
        box(draw, rect[:4], rect[4], fill=COLORS["red"], text_size_px=16)
    elbow(draw, [(1565, mid(driver_boxes["wifi"])), (1620, mid(driver_boxes["wifi"])), (1620, mid(backend_boxes["udp"])), (1670, mid(backend_boxes["udp"]))])
    elbow(draw, [(1565, mid(driver_boxes["wifi"])), (1620, mid(driver_boxes["wifi"])), (1620, mid(backend_boxes["fastapi"])), (1670, mid(backend_boxes["fastapi"]))])
    arrow(draw, (1752, backend_boxes["fastapi"][3]), (1752, backend_boxes["ai"][1]))

    note = (
        "统一设备身份：APP_DEVICE_ID → AI / 相机 / 对讲均使用同一 device_id。\n"
        "AI 音频格式保持 WAV PCM s16le 16kHz mono；上传 8192B 分片，回复拉取 32768B 分片。\n"
        "实时对讲不使用 WebSocket、不使用 Opus；当前回到 UDP WTK1 20ms/640B PCM。"
    )
    box(draw, (80, 900, 1820, 1020), note, fill="#f7f9fc", outline="#c8cfda", text_size_px=22)
    save(img, "01_project_architecture_current.png")


def network_diagram() -> None:
    w, h = 1500, 840
    img = Image.new("RGB", (w, h), "white")
    draw = ImageDraw.Draw(img)
    header(
        draw,
        "网络切换与统一网络接口流程图",
        "当前实现：设置页选择 WiFi 或伪4G；app_network 管理模式切换；service_network 向上提供统一 UDP/HTTP/状态接口。",
        w,
    )

    panel(draw, (45, 150, 365, 610), "设置页 / UI 输入", fill="#fbfcff")
    panel(draw, (430, 150, 755, 610), "网络业务模块", fill="#fbfcff")
    panel(draw, (820, 150, 1115, 610), "统一 service_network", fill="#fbfcff")
    panel(draw, (1180, 150, 1455, 610), "上层业务复用", fill="#fbfcff")

    boxes = {
        "wifi": (85, 225, 325, 315, "选择 WiFi\n扫描 AP\n输入 SSID 密码"),
        "g4": (85, 385, 325, 475, "选择 4G\n连接伪4G热点\nSSID=14"),
        "save": (470, 210, 715, 300, "保存网络模式\nNVS: wifi_ssid\nwifi_pwd"),
        "switch": (470, 330, 715, 420, "切换链路\nservice_network_deinit\n连接 WiFi 或伪4G"),
        "bind": (470, 455, 715, 550, "绑定 service_network\n当前使用 d_wifi ops\nready/status 可查询"),
        "svc": (850, 215, 1085, 535, "service_network\n统一门面\n\nUDP connect/send\nread_downlink 轮询\nHTTP POST\nget_status/is_ready\n\nI/O mutex 串行化\n避免 HTTP 与 UDP\n同时进入底层 driver"),
        "intercom": (1215, 205, 1420, 280, "app_intercom\nUDP 对讲\n掉线重建 UDP"),
        "ai": (1215, 325, 1420, 400, "app_ai_voice\nHTTP /ai/*\nWAV 上传下载"),
        "camera": (1215, 445, 1420, 520, "app_camera\n/camera/upload\nJPEG body"),
        "status": (1215, 540, 1420, 605, "status_monitor\n网络格数\n掉线恢复"),
    }
    for key, rect in boxes.items():
        fill = COLORS["blue"] if key in {"wifi", "g4"} else COLORS["green"] if key in {"save", "switch", "bind"} else COLORS["orange"] if key == "svc" else COLORS["box"]
        box(draw, rect[:4], rect[4], fill=fill, text_size_px=17)

    arrow(draw, (325, 270), (466, 255))
    arrow(draw, (325, 430), (466, 255))
    arrow(draw, (592, 304), (592, 326))
    arrow(draw, (592, 424), (592, 451))
    arrow(draw, (715, 502), (846, 375))
    for key in ["intercom", "ai", "camera", "status"]:
        r = boxes[key]
        arrow(draw, (1085, 375), (1211, (r[1] + r[3]) // 2))

    bottom1 = (
        "关键输入：网络模式、WiFi SSID/密码、伪4G热点状态、service_network ready 状态。\n"
        "关键输出：统一 UDP/HTTP/下行读取接口、网络状态显示、UDP 重连通知、HTTP 请求结果。"
    )
    bottom2 = (
        "注意：当前“4G”路径在业务层表现为 APP_NETWORK_MODE_4G，底层仍通过 d_wifi 连接固定热点；"
        "service_network 对 app 层屏蔽这个差异。"
    )
    box(draw, (65, 660, 1435, 735), bottom1, fill="#f7f9fc", outline="#c8cfda", text_size_px=21)
    box(draw, (65, 755, 1435, 805), bottom2, fill="#fff8ee", outline="#d8bd91", text_size_px=20)
    save(img, "02_network_switch_service_flow_current.png")


def intercom_diagram() -> None:
    w, h = 1550, 880
    img = Image.new("RGB", (w, h), "white")
    draw = ImageDraw.Draw(img)
    header(
        draw,
        "实时对讲关键接口流程图",
        "当前实现：WTK1 自定义 UDP 包，AUDIO payload 为 20ms/640B PCM；服务端按频道转发，设备端 jitter buffer 播放。",
        w,
    )

    panel(draw, (45, 150, 425, 700), "设备 A：发送端", fill="#fbfcff")
    panel(draw, (585, 150, 950, 700), "后端 UDP 转发服务", fill="#fbfcff")
    panel(draw, (1125, 150, 1505, 700), "设备 B：接收端", fill="#fbfcff")

    sender = [
        (95, 215, 375, 285, "用户按下 PTT"),
        (95, 325, 375, 400, "app_business\n抢占音频会话锁"),
        (95, 440, 375, 520, "service_audio_read\n20ms PCM\n320 samples / 640B"),
        (95, 560, 375, 660, "WTK1 打包\nUDP 发送\nchannel / seq / device\nPCM 640B"),
    ]
    server = [
        (635, 215, 900, 305, "接收 WTK1 包\nREGISTER / CHANNEL\nPTT / AUDIO"),
        (635, 365, 900, 450, "按 channel 选择目标设备\n排除发送端 device_id"),
        (635, 520, 900, 620, "AUDIO 原样转发\n不编码 / 不重传\npayload 仍是\n20ms PCM"),
    ]
    receiver = [
        (1175, 205, 1455, 280, "read_downlink\n轮询 UDP 下行"),
        (1175, 315, 1455, 390, "扫描 WTK1 魔数\n保留半包，只消费完整包"),
        (1175, 425, 1455, 505, "解析头并过滤\n频道匹配且不是本机"),
        (1175, 545, 1455, 635, "jitter buffer 播放\n32 包容量\n起播 10 包\n补偿或跳过缺口"),
    ]
    for rect in sender:
        box(draw, rect[:4], rect[4], fill=COLORS["blue"], text_size_px=16)
    for rect in server:
        box(draw, rect[:4], rect[4], fill=COLORS["green"], text_size_px=16)
    for rect in receiver:
        box(draw, rect[:4], rect[4], fill=COLORS["orange"], text_size_px=16)

    for stack in [sender, server, receiver]:
        for a, b in zip(stack, stack[1:]):
            x = (a[0] + a[2]) // 2
            arrow(draw, (x, a[3] + 4), (x, b[1] - 4))

    elbow(draw, [(375, 610), (500, 610), (500, 260), (631, 260)])
    elbow(draw, [(900, 570), (1040, 570), (1040, 242), (1171, 242)])
    arrow(draw, (1315, 639), (1315, 656))
    box(draw, (1175, 660, 1455, 690), "service_audio_write 播放 PCM", fill="#fff7e8", text_size_px=15)

    protocol = (
        "WTK1 包头：magic(4) + type(1) + header_len(1=34) + channel(2LE) + seq(4LE) + timestamp_ms(4LE) "
        "+ device(16) + payload_len(2LE)。\n"
        "AUDIO payload：PCM s16le / 16kHz / mono / 20ms / 320 samples / 640B；当前不使用 Opus，也没有设备端重传机制。"
    )
    runtime = (
        "接收保护：按 session/设备名过滤旧流；jitter 起播 10 包，自适应最高 16 包；连续缺包用简单补偿，"
        "有后续帧时可跳过缺口；240ms 空闲后关闭播放输出。"
    )
    box(draw, (65, 720, 1485, 790), protocol, fill="#f7f9fc", outline="#c8cfda", text_size_px=19)
    box(draw, (65, 810, 1485, 860), runtime, fill="#fff8ee", outline="#d8bd91", text_size_px=19)
    save(img, "03_realtime_intercom_protocol_flow_current.png")


def main() -> None:
    architecture_diagram()
    network_diagram()
    intercom_diagram()


if __name__ == "__main__":
    main()
