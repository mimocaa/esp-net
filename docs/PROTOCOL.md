# STM32 ↔ ESP32-C3 SPI 音频桥接协议

本文件是 `cat/`（STM32F411，**主机**）与 `esp-net/`（ESP32-C3，SPI2 **从机**）
之间通信规范的唯一源头。两份仓库保持内容一致。

## 接线（共 6 线，两端均为 3.3V，须共地）

| 信号 | 方向 | STM32F411（主机） | ESP32-C3（从机） |
|--------|------|-------------------|------------------|
| SCLK   | STM→ESP | PB13 | GPIO7 (`SPI2_SCLK`) |
| MOSI   | STM→ESP（麦克风上行） | PB15 | GPIO6 (`SPI2_MOSI`) |
| MISO   | ESP→STM（播放下行） | PB14 | GPIO5 (`SPI2_MISO`) |
| CS/NSS | STM→ESP（低有效） | PC7 (`ESP_Bridge_CS`) | GPIO10 (`SPI2_CS`) |
| HANDSHAKE | ESP→STM（高有效） | PC6 (`ESP_Bridge_HS`) | GPIO4 (`SPI2_HANDSHAKE`) |
| GND    | — | GND | GND |

两端均按主机视角命名 → 同名直连。

## SPI 参数
- 模式 0（CPOL=0 / CPHA=0），MSB 先发，8 位字长。
- CS 低有效，必须覆盖整个 1280 字节帧（用 DMA 时在传输完成回调里拉高）。
- 时钟由 STM 主机设定；建议起步 1~4 MHz（ESP32-C3 从机走 GPIO matrix，上限约 10 MHz）。

## 帧格式
- 固定每帧 **1280 字节**，每次 CS 选中对应一帧，无 CRC/校验尾。
- 1280 B = 640 个 int16 样本。
  - 上行 16 kHz：约 40 ms/帧。
  - 下行 24 kHz：约 26.7 ms/帧。

## 音频格式
- **上行（麦克风，MOSI）**：PCM16LE，**16 kHz**，单声道。
  STM 端 I2S3 接收 24-bit→须下采样为 16-bit 后再经 SPI 发送。
- **下行（播放，MISO）**：PCM16LE，**24 kHz**，单声道。
  ESP 直接输出后端流式返回的 PCM16。STM 端播放时需将 I2S3 重配为 24k TX。

## 相位协议（单握手线，ESP 决策）
时分半双工（不支持播放中打断）。握手线电平 = 相位：

- **HANDSHAKE 低 = RECORD**：STM 持续经 MOSI 发送麦克风帧；ESP 接收并运行 VAD。MISO 发静音。
- **HANDSHAKE 高 = ESP 忙 / PLAY**：STM 停止发送麦克风、在主机时钟时经 MISO 读取播放帧；ESP 提供 24k PCM（TTS 到达前为静音）。STM 将 I2S3 切为 24k TX 播放。

一个完整回合的时序（ESP 侧驱动）：
1. RECORD（低）：麦克风流持续；ESP VAD 空闲，维护 pre-roll 环形缓冲。
2. VAD 判定说话起始 → ESP 发起 HTTP 分块上传（先发 pre-roll，再发持续麦克风帧）。**仍为 RECORD/低**（麦克风持续工作）。
3. VAD 判定静音结束（挂起到时） → ESP 结束上传体。
4. ESP 将 HANDSHAKE 拉高 → STM 进入 PLAY（24k）。ESP 读取 HTTP 响应并将其喂为播放帧；STM 播放。
5. 播放排空 → ESP 将 HANDSHAKE 拉低 → 回到 RECORD。

## 控制帧 / 情绪上行
配合"唤醒→开始录制"边界，使能后的**第一帧为控制帧**，承载 STM 当前宠物情绪。
后续帧均为裸 1280B PCM16 音频帧。

- 控制帧载荷（1280B）：
  ```
  [0x01] [emotion_code] [0x00] [0x00] … 1276 字节补零 …
  ```
  其中 `emotion_code` 取值：

| 码 | 语义 |
|----|------|
| 0  | normal |
| 1  | happy  |
| 2  | alert  |
| 3  | angry  |
| 4  | curious |
| 5  | shy    |
| 6  | sad    |

- STM 侧 `EmotionTypeDef` → 协议码映射（Sad 不在其枚举中，其余六类一一对应；未知回退 0）。
- ESP 收到控制帧后存入最新情绪；下次 HTTP 上传时以 `X-Emotion: <ASCII>` 请求头返回给后端。
