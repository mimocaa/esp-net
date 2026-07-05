# esp-net — ESP32-C3 WiFi/HTTP 固件

基于 ESP-IDF，作为 STM32（`cat/`）与云后端（`service_backend/`）之间的通信中继。

## 引脚分配

| 外设 | 信号 | GPIO | 说明 |
|------|------|------|------|
| SPI2 (FSPI) 从机 | MISO | 5 | 下行播放(24k PCM16)→ STM |
| | MOSI | 6 | 上行麦克风(16k PCM16)← STM |
| | SCLK | 7 | 由 STM 主机提供时钟 |
| | CS | 10 | STM 主机拉低选中 |
| | HANDSHAKE | 4 | 相位输出(低=Record / 高=Play) |

> 所有引脚在 `main/Kconfig.projbuild` 中可配置，通过 `menuconfig` 修改。

## Kconfig 配置项（`menuconfig`）

### Wi-Fi
- `WIFI_SSID` / `WIFI_PASSWD`：自动连接的目标 Wi-Fi。

### HTTP
- `SERVER_BASE_URL`：后端地址（**必须含端口**，如 `http://192.168.1.100:3000`）。
- `DATA_BUFFER_SIZE`(2048)：HTTP 接收缓冲。
- `HTTP_TIMEOUT_MS`(8000)：连接/读写超时。
- `HTTP_MAX_RETRIES`(3)：单次 `/chat` 请求重试次数。
- `HTTP_RETRY_DELAY_MS`(2000)：重试间隔。

### SPI 桥接
- `SPI2_MISO/MOSI/SCLK/CS/HANDSHAKE`：引脚分配。
- `SPI2_HANDSHAKE_ACTIVE_HIGH`(y)：握手有效电平。
- `SPI_FRAME_SIZE`(1280)：固定帧长（字节）。
- `SPI_RING_BYTES`(16384)：下行播放 StreamBuffer 容量。
- `SPI_SLAVE_TX_TIMEOUT_MS`(2000)：等待主机读走的超时。

### 音频 / VAD
- `MIC_RING_BYTES`(16384)：上行麦克风 StreamBuffer 容量。
- `VAD_START_RMS`(500) / `VAD_END_RMS`(300)：语音起/止 RMS 阈值。
- `VAD_HANGOVER_MS`(600)：静音持续多久判定说话结束。
- `VAD_PREROLL_MS`(300)：说话前保留多少音频防止掐头。

## 外设使用

| 外设 | 角色 | 说明 |
|------|------|------|
| SPI2 (FSPI) | 从机 | 固定 1280B 定长帧，全双工；DMA。 |
| Wi-Fi (Station) | 客户端 | 连接 AP 后 HTTP POST 到后端。 |
| GPIO4 | 握手输出 | 驱动相位线（Record=低 / Play=高）。 |
| NVS | 持久化 | 存储会话 ID（待实现 `driver/nvs.hpp`）。 |

## 程序流程

```
app_main
  ├─ nvs_init()
  ├─ WiFi.connect()
  └─ while(1): chat_turn()
       ├─ set_phase(Record) + resume()  ← 等 STM 唤醒帧(控制帧)
       ├─ VAD 等待语音起点（维护 pre-roll）
       ├─ 语音起 → chunked 上传(pre-roll + 持续麦克风)
       ├─ VAD 判尾 → 结束上传体
       ├─ set_phase(Play)  ← on_upload_done
       ├─ 读 HTTP 响应 + SPI 播放（write→feeder→MISO→STM）
       ├─ flush() 等待播放排空
       └─ set_phase(Record)   ← 回 RECORD 等下一轮
```

## 云端与本地通信流程

```
STM32 (cat)                    ESP32-C3                    Cloud (service_backend)
──────────                     ───────                     ──────────────────────

[唤醒] ──(控制帧：情绪码)──→
[持续发麦克风帧 16k/PCM16] ──→  VAD 检到语音起点
                              ├─ chunked 上传           ──→ POST /chat
                              │  Content-Type: octet-stream    ├─ 聚合 → pcm_to_wav
                              │  X-Session-Id / X-Emotion      ├─ MiMo STT
                              │                                ├─ DeepSeek LLM
                              │                                ├─ MiMo TTS (stream)
                              │  ← 200 OK + audio/pcm body   ←┘
                              │  X-Session-Id / X-Emotion
                              ├─ write()→feeder→SPI(MISO)
[收播放帧 24k/PCM16]         ←┘
[播放完毕]                    ── 握手拉低 → 回到 RECORD
```

> 详细 SPI 协议见 `PROTOCOL.md`。
