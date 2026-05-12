# Walkie Talkie V1

本项目当前业务重点是 WiFi/ML307C 网络、UDP 实时对讲、AI WAV 问答、音频录放、UI 状态显示。

## AI WAV 分片问答协议

客户端录音和服务器回复都使用同一套音频格式：

- Container: WAV
- Codec: PCM
- Sample rate: 16000 Hz
- Bit depth: 16-bit
- Channels: mono
- Endian: little-endian

客户端限制：

- 单次录音最长 60 秒。
- 单次回复最长 120 秒。
- HTTP 分片 body 最大 32768 字节。
- 上传 WAV 最大约 1920044 字节。
- 回复 WAV 最大约 3840044 字节。
- 第一版不做断点续传，任意分片失败则整次 AI 会话失败。

所有请求都使用 `POST`，基础 URL 由固件中的 `APP_BUSINESS_AI_HTTP_URL` 配置。若基础 URL 已经包含 query，例如 `?language=zh`，客户端会继续追加 `&op=...`。

### 1. 创建会话

请求：

```text
POST <AI_URL>&op=start&device=walkie-01
Content-Type: application/json

{}
```

响应：

```json
{"session":"abc123","chunk_size":32768}
```

字段说明：

- `session`: 服务端生成的本次问答会话 ID，后续所有请求都携带。
- `chunk_size`: 服务端建议分片大小；客户端当前固定使用 32768 字节。

### 2. 上传录音 WAV 分片

请求：

```text
POST <AI_URL>&op=upload&session=abc123&index=0&offset=0&total=1920044
Content-Type: application/octet-stream

<WAV bytes chunk>
```

字段说明：

- `index`: 分片序号，从 0 开始。
- `offset`: 当前分片在完整请求 WAV 中的字节偏移。
- `total`: 完整请求 WAV 总字节数。
- body: 当前 WAV 原始字节片，长度不超过 32768 字节，最后一片允许短片。

响应：

```json
{"ok":true}
```

### 3. 上传完成

请求：

```text
POST <AI_URL>&op=finish&session=abc123
Content-Type: application/json

{}
```

响应：

```json
{"ok":true,"status":"processing"}
```

服务端收到 `finish` 后开始把已上传的 WAV 交给 AI 处理。

### 4. 查询回复状态

请求：

```text
POST <AI_URL>&op=result_info&session=abc123
Content-Type: application/json

{}
```

未完成：

```json
{"ready":false}
```

已完成：

```json
{"ready":true,"total":123456,"format":"wav"}
```

字段说明：

- `ready`: 回复 WAV 是否已生成。
- `total`: 完整回复 WAV 总字节数，只在 `ready=true` 时必须提供。
- `format`: 固定为 `wav`。

客户端会轮询 `result_info`，总等待时间由 `APP_AI_PROCESS_TIMEOUT_MS` 控制。

### 5. 拉取回复 WAV 分片

请求：

```text
POST <AI_URL>&op=result_chunk&session=abc123&offset=0&len=32768
Content-Type: application/json

{}
```

响应 body：

```text
<WAV bytes chunk>
```

字段说明：

- `offset`: 当前拉取片在完整回复 WAV 中的字节偏移。
- `len`: 期望拉取字节数，最大 32768 字节，最后一片允许短片。
- 响应 body 必须直接返回对应范围的 WAV 原始字节，不要包 JSON，不要 base64。

客户端收齐全部回复 WAV 后会校验：

- `RIFF`
- `WAVE`
- PCM format = 1
- channels = 1
- sample rate = 16000
- bits = 16
- 存在合法 `data` chunk

校验通过后播放 `data` chunk 中的 PCM。

## ML307C 注意事项

ML307C 手册中 `AT+HTTP=<id>,<body_size>,<timeout>,<latency>` 的 `body_size` 取值范围为 `0-65535`，因此固件把每个 AI HTTP 请求体限制在 32768 字节。

大回复不依赖单次 HTTP 大响应，而是通过 `result_chunk` 分片拉取，避免响应体过大导致 UART/AT 解析不稳定。
