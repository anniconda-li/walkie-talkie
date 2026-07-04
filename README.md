# Walkie Talkie V1

ESP-IDF 对讲机固件项目，当前覆盖 WiFi/ML307C 网络、UDP 实时对讲、AI 语音问答、相机上传、音频录放、LCD/触摸 UI 和状态监控。

## 项目结构

- `main/`: 固件入口，负责启动 driver、service 和 app。
- `components/wdriver/`: 板级外设基础封装，包括 I2C、I2S、SPI、UART 和引脚配置。
- `components/device/`: 具体设备驱动，包括 WiFi、ML307C、LCD、触摸、相机、电池、麦克风和功放/codec。
- `components/service/`: 面向业务的能力抽象，包括网络、音频、屏幕、相机和电池服务。
- `components/app/`: 业务层，包括 UDP 对讲、AI 问答、相机业务、网络切换、UI 和状态监控。
- `components/osal/`: FreeRTOS 任务、队列、互斥锁、堆和日志的轻量封装。
- `tools/wifi_net_test_server.py`: 局域网测试服务，提供 UDP 对讲测试、AI WAV 分片协议和 JPEG 上传接口。

## 关键配置

主要业务配置集中在 `components/app/inc/app_config.h`：

- `APP_BUSINESS_DEVICE_NAME`: 设备名，默认 `walkie-01`。
- `APP_BUSINESS_SERVER_HOST`: UDP 对讲服务器地址。
- `APP_BUSINESS_UDP_PORT`: UDP 对讲端口，默认 `9000`。
- `APP_BUSINESS_HTTP_BASE_URL`: FastAPI 业务服务根地址，例如 `http://<PC_LAN_IP>:8000`。
- `APP_AI_UPLOAD_CHUNK_BYTES`: AI 请求音频上传分片大小，当前 `8192` 字节。
- `APP_AI_REPLY_CHUNK_BYTES`: AI 回复音频拉取分片大小，当前 `32768` 字节。
- `AUTO_PLAY_REPLY_AUDIO`: AI 回复语音是否自动播放，默认 `0`，即文本先显示、语音按钮手动播放。
- `APP_BUSINESS_AUDIO_SAMPLE_RATE`: 业务音频采样率，当前 `16000` Hz。
- `APP_BUSINESS_AI_MAX_MS`: 单次 AI 录音上限，当前 `60000` ms。
- `APP_BUSINESS_AI_REPLY_MAX_MS`: 单次 AI 回复音频上限，当前 `120000` ms。

板级引脚和外设开关集中在 `components/wdriver/inc/wdriver_config.h`。当前默认音频后端为 `INMP441 + MAX98357A`，WiFi 网络方案下默认不初始化 ML307C UART。

网络后端由 service 装配层选择并暴露为统一接口。WiFi 和 ML307C 都通过 `service_network` 向 app 层提供 UDP、TCP 和 HTTP POST 能力。

## AI 语音链路

AI 问答采用半双工流程：

1. 用户长按 AI 按钮。
2. `app_ai_voice` 抢占音频会话，与 PTT 互斥。
3. AI 任务循环读取 PCM，直接写入请求 WAV 缓冲区的 `data` 区。
4. 用户松手后停止采集，回填 WAV 头。
5. 固件创建 AI session，并按 `APP_AI_UPLOAD_CHUNK_BYTES` 分片上传请求 WAV。
6. 固件通知服务端处理并轮询 `result_info`。
7. 服务端先返回 `answer_text`，UI 立即更新回答文本。
8. TTS 在服务端后台生成；语音未就绪时 UI 喇叭按钮为灰色禁用。
9. `audio_ready=true` 或 `reply_wav_ready=true` 后，UI 喇叭按钮变为亮色可点击。
10. 默认不自动下载或播放 WAV；用户点击喇叭按钮后才按 `result_chunk` 分片拉取回复 WAV。
11. 客户端不缓存完整回复 WAV，而是流式解析 `RIFF/WAVE/fmt/data`，解析出的 PCM 进入小型预缓冲队列。
12. 播放端连续消费 PCM 队列，播放后丢弃旧数据。

当前内存策略：

- 请求侧保留一次完整请求 WAV 缓冲，最大约 `1,920,044` 字节。
- 回复侧不保留完整 WAV，只保留一个 HTTP 分片缓冲和约 512ms 的 PCM 预缓冲。
- 回复 WAV 允许 `data` chunk 不在 44 字节处；客户端会跳过额外 chunk，例如 `JUNK`。
- 若任一分片下载、WAV 解析或播放失败，本次 AI 会话失败并释放音频会话锁。
- 自动播放可通过 `AUTO_PLAY_REPLY_AUDIO` 开启；默认值必须保持 `0`，避免文本显示和音频播放绑定。

## UI 字体和 AI 播放按钮

UI 固定使用中文。项目内已经导入裁剪后的 LVGL C 字体：

- `components/app/src/ui/fonts/ui_font_16.c`: 默认正文字体，通过 `ui_font_normal()` 使用；由 `SourceHanSansSC-Regular-2.otf` 裁切生成，覆盖 ASCII、CJK 基本汉字区 `0x4E00-0x9FFF`、常用中文标点和当前 LVGL symbol 图标，供 AI 回答框和主要中文 UI 使用。该字库需要 `CONFIG_LV_FONT_FMT_TXT_LARGE=y`。
- `components/app/src/ui/fonts/ui_font_14.c`: 小号字体，通过 `ui_font_small()` 使用；保持轻量裁剪，仅用于小号固定 UI 文案。

应用入口会在 `ui_init()` 中把屏幕默认字体设置为 `ui_font_normal()`。AI 回答文本、等待状态文本和新增喇叭按钮 label 也会显式设置为 `ui_font_normal()`，避免中文或兜底文本回落到 LVGL 默认 Montserrat 字体。

AI 页面新增一个 40x40 的 LVGL 原生按钮，创建位置在 `components/app/src/ui/ui_app_ai.c`。按钮内容优先使用 `LV_SYMBOL_AUDIO`；当前 LVGL 已提供该 symbol。按钮状态由 `ui_event_set_ai_audio_button_state()` 管理：

- `UI_AI_AUDIO_BTN_HIDDEN`: 初始隐藏。
- `UI_AI_AUDIO_BTN_WAITING`: 文本已显示但语音未生成，灰色禁用。
- `UI_AI_AUDIO_BTN_READY`: 语音已生成，亮色可点击。
- `UI_AI_AUDIO_BTN_PLAYING`: 正在下载或播放，临时禁用。
- `UI_AI_AUDIO_BTN_FAILED`: 语音生成失败，灰色禁用。

## AI WAV 分片协议

客户端录音和服务端回复使用同一套音频格式：

- Container: WAV
- Codec: PCM
- Sample rate: 16000 Hz
- Bit depth: 16-bit
- Channels: mono
- Endian: little-endian

所有 AI HTTP 请求均使用 `POST`。固件只配置服务根地址，路由由 `APP_BUSINESS_HTTP_ROUTE_*` 宏拼接。

### 1. 创建会话

```text
POST <AI_URL>/ai/start
Content-Type: application/json

{"device":"walkie-01","language":"zh"}
```

响应：

```json
{"session":"abc123","chunk_size":8192}
```

字段：

- `session`: 服务端生成的本次问答会话 ID。
- `chunk_size`: 服务端建议分片大小；当前固件按 `APP_AI_UPLOAD_CHUNK_BYTES` 字节上传请求音频。

### 2. 上传请求 WAV 分片

```text
POST <AI_URL>/ai/upload?session=abc123&index=0&offset=0&total=1920044
Content-Type: application/octet-stream

<WAV bytes chunk>
```

字段：

- `index`: 分片序号，从 0 开始。
- `offset`: 当前分片在完整请求 WAV 中的字节偏移。
- `total`: 完整请求 WAV 总字节数。
- body: 当前 WAV 原始字节片，长度不超过 `APP_AI_UPLOAD_CHUNK_BYTES` 字节，最后一片允许短片。

响应：

```json
{"ok":true}
```

### 3. 上传完成

```text
POST <AI_URL>/ai/finish?session=abc123
Content-Type: application/json

{}
```

响应：

```json
{"ok":true,"status":"processing"}
```

服务端收到 `finish` 后开始处理已经上传完成的 WAV。

### 4. 查询回复状态

```text
POST <AI_URL>/ai/result_info?session=abc123
Content-Type: application/json

{}
```

未完成：

```json
{"ready":false}
```

文本已完成、语音后台生成中：

```json
{
  "ok": true,
  "session": "abc123",
  "status": "text_ready",
  "asr_text": "用户提问文本",
  "answer_text": "先显示给用户的回答文本",
  "audio_ready": false,
  "reply_wav_ready": false,
  "reply_wav_size": 0,
  "reply_duration": 0,
  "tts_status": "running",
  "tts_error": null
}
```

语音已完成：

```json
{
  "ok": true,
  "session": "abc123",
  "status": "audio_ready",
  "asr_text": "用户提问文本",
  "answer_text": "先显示给用户的回答文本",
  "audio_ready": true,
  "reply_wav_ready": true,
  "reply_wav_size": 123456,
  "reply_duration": 4.2,
  "tts_status": "done",
  "tts_error": null
}
```

语音生成失败：

```json
{
  "ok": true,
  "session": "abc123",
  "status": "audio_failed",
  "answer_text": "文本回答仍然保留显示",
  "audio_ready": false,
  "reply_wav_ready": false,
  "reply_wav_size": 0,
  "tts_status": "failed",
  "tts_error": "TTS error message"
}
```

字段：

- `answer_text`: LLM 文本回答。客户端拿到非空值后立即刷新 AI 回答区域。
- `status`: 推荐使用 `text_ready`、`audio_ready` 或 `audio_failed`。
- `audio_ready` / `reply_wav_ready`: 任一为 true 时客户端认为回复 WAV 可下载。
- `reply_wav_size`: 完整回复 WAV 总字节数；旧字段 `total` 仍兼容。
- `tts_status`: 推荐使用 `pending`、`running`、`done`、`failed` 或 `disabled`。
- `tts_error`: TTS 失败原因，可为 `null` 或字符串。
- `ready` / `total`: 旧格式兼容字段。`ready=true` 等价于音频已完成，`total` 等价于 `reply_wav_size`。

客户端兼容判断：

- 文本可显示：`status=="text_ready"` 或 `answer_text` 非空。
- 音频可播放：`status=="audio_ready"`、`audio_ready==true`、`reply_wav_ready==true` 或旧格式 `ready==true`。
- 音频失败：`status=="audio_failed"` 或 `tts_status=="failed"`。

### 5. 拉取回复 WAV 分片

```text
POST <AI_URL>/ai/result_chunk?session=abc123&offset=0&len=32768
Content-Type: application/json

{}
```

响应 body：

```text
<WAV bytes chunk>
```

字段：

- `offset`: 当前拉取片在完整回复 WAV 中的字节偏移。
- `len`: 期望拉取字节数，最大 `APP_AI_REPLY_CHUNK_BYTES` 字节，最后一片允许短片。
- 响应 body 必须直接返回对应范围的 WAV 原始字节，不要包 JSON，不要 base64。

客户端拉取过程中会流式校验：

- `RIFF`
- `WAVE`
- PCM format = 1
- channels = 1
- sample rate = 16000
- bits = 16
- 存在合法 `data` chunk

校验通过后，从 `data` chunk 中解析 PCM，并通过预缓冲队列连续播放。

## UDP 对讲

UDP 对讲使用 `WTK1` 自定义包。测试服务会记录设备注册、频道切换、PTT start/stop 和音频包；单设备测试时，服务端会把同设备音频包改写为 `server-echo` 后回发，避免客户端因设备名相同而丢弃。

业务层的 UDP 对讲路径在 `components/app/src/app_intercom.c`，音频帧为：

- 16000 Hz
- 16-bit PCM
- mono
- 每包 20ms
- 每包 320 samples / 640 bytes

AI 录音和 PTT 发送通过 `app_business_audio_session_try_begin()` 互斥，避免同时占用麦克风和扬声器链路。

## 相机上传

相机业务通过 `APP_BUSINESS_HTTP_ROUTE_CAMERA_UPLOAD` 上传 JPEG：

```text
POST <AI_URL>/camera/upload
Content-Type: image/jpeg

<JPEG bytes>
```

测试服务会校验 SOI/EOI、解析基础尺寸信息，并保存到 `tools/received_jpg`。

## ML307C 注意事项

ML307C 的 HTTP 能力当前基于 AT 指令封装。`AT+HTTP=<id>,<body_size>,<timeout>,<latency>` 的 `body_size` 范围为 `0-65535`，因此固件把 AI HTTP 单片请求体限制在对应的上传/拉取分片大小内。

当前 AI 回复不依赖单次大 HTTP 响应，而是通过 `result_chunk` 分片拉取并边播边丢弃，避免大响应占用本地内存，也降低 UART/AT 解析压力。

如果后续要做真正的 HTTP request/response 流式传输，WiFi 可以基于底层 HTTP client 或 socket 实现；ML307C 需要确认模块是否支持持续写入 request body 和持续读取 response body，否则需要走 TCP 自组 HTTP 或继续保持分片协议。

## 本地测试服务

首次运行前建议创建 Python 3.11 虚拟环境并安装依赖：

```powershell
py -3.11 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r tools\requirements.txt
```

启动默认测试服务：

```powershell
.\.venv\Scripts\python.exe tools\wifi_net_test_server.py --host 0.0.0.0 --http-port 8000 --udp-port 9000
```

测试 AI 长回复和非 44 字节 `data` 起点：

```powershell
.\.venv\Scripts\python.exe tools\wifi_net_test_server.py --host 0.0.0.0 --http-port 8000 --udp-port 9000 --ai-reply-repeat 5 --ai-reply-extra-chunk
```

参数：

- `--host`: HTTP/UDP 绑定地址，默认 `0.0.0.0`。
- `--http-port`: FastAPI HTTP 端口，默认 `8000`。
- `--udp-port`: UDP 对讲端口，默认 `9000`。
- `--wav-save-dir`: 保存收到的 AI 请求 WAV。
- `--jpg-save-dir`: 保存收到的相机 JPEG。
- `--ai-reply-repeat`: 将上传 WAV 的 PCM 重复 N 次作为回复，用于测试长回复边拉边播。
- `--ai-reply-extra-chunk`: 在回复 WAV 的 `fmt` 和 `data` 之间插入 `JUNK` chunk，用于测试客户端流式 WAV 解析。

固件侧需要把 `APP_BUSINESS_HTTP_BASE_URL` 和 `APP_BUSINESS_SERVER_HOST` 改为测试机局域网 IP，例如：

```c
#define APP_BUSINESS_SERVER_HOST   "192.168.1.100"
#define APP_BUSINESS_HTTP_BASE_URL "http://192.168.1.100:8000"
```

## 常见联调问题

- AI 播放在分片边界卡顿：确认当前固件包含后台下载 + PCM 预缓冲实现；旧实现会在每个 HTTP 请求之间停播。
- AI 回复失败：检查服务端 `result_info.reply_wav_size` 或兼容字段 `total` 是否正确，`result_chunk` 是否按 offset/len 返回纯 WAV 字节。
- WAV 解析失败：确认回复为 PCM 16kHz、mono、16-bit，并且存在合法 `fmt ` 和 `data` chunk。
- ML307C HTTP 失败：确认单片 body 不超过对应的上传/拉取分片大小，URL 和 header 没有超过 AT 命令缓冲限制。
- 相机上传失败：确认 body 是完整 JPEG，且 `Content-Type` 包含 `image/jpeg` 或 `image/jpg`。
- UDP 没有回音：确认设备和测试服务在同一局域网，端口为 `9000`，防火墙允许 UDP 入站。
