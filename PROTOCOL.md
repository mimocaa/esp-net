# STM32 ⇄ ESP32-C3 SPI audio bridge protocol

Source of truth for the SPI link between `cat/` (STM32F411, **master**) and
`esp-net/` (ESP32-C3, SPI2 **slave**). Keep this file identical in both repos.

## Wiring (6 lines, 3.3V both sides, common GND)

| Signal | Dir | STM32F411 (master) | ESP32-C3 (slave) |
|--------|-----|--------------------|------------------|
| SCLK   | STM→ESP | PB13 | GPIO7 (`SPI2_SCLK`) |
| MOSI   | STM→ESP (mic uplink) | PB15 | GPIO6 (`SPI2_MOSI`) |
| MISO   | ESP→STM (playback downlink) | PB14 | GPIO5 (`SPI2_MISO`) |
| CS/NSS | STM→ESP (active LOW) | soft GPIO | GPIO10 (`SPI2_CS`) |
| HANDSHAKE | ESP→STM (active HIGH) | input GPIO | GPIO4 (`SPI2_HANDSHAKE`) |
| GND    | — | GND | GND |

Names are master-referenced on both ends → connect same-name to same-name.

## SPI parameters
- Mode 0 (CPOL=0, CPHA=0), MSB-first, 8-bit words.
- CS active low, must stay asserted for the **whole** 1280-byte frame (bracket the DMA).
- Clock is set by the STM master; keep it low enough for the C3 GPIO-matrix slave
  (~1–4 MHz to start). The ESP does NOT set the clock.

## Framing
- Fixed **1280-byte** frames, bare PCM (no header/CRC), one frame per CS assertion.
- 1280 B = 640 int16 samples = 40 ms @16kHz (uplink) / 26.7 ms @24kHz (downlink).

## Audio formats
- **Uplink (mic, MOSI)**: PCM16LE, **16 kHz**, mono. STM captures I2S3 @ 24-bit/16k
  and must down-convert 24→16-bit before sending.
- **Downlink (playback, MISO)**: PCM16LE, **24 kHz**, mono. STM must (re)configure
  I2S to 24 kHz TX during playback.

## Phase protocol (single handshake line, ESP decides)
Time-division half-duplex (no barge-in). The handshake line = phase:

- **HANDSHAKE LOW = RECORD**: STM continuously streams mic frames on MOSI; ESP
  receives them, runs VAD. MISO carries silence.
- **HANDSHAKE HIGH = ESP BUSY/PLAY**: STM stops mic and, when it clocks, reads
  playback frames on MISO; ESP feeds 24k PCM (silence until TTS arrives). STM
  switches I2S to 24 kHz TX.

Turn timeline (ESP-driven):
1. RECORD (LOW): mic flows; ESP VAD idle, keeps a pre-roll ring.
2. VAD detects speech start → ESP opens the HTTP upload and streams pre-roll +
   ongoing mic frames. **Still RECORD/LOW** (mic keeps flowing).
3. VAD detects silence (hangover elapsed) → ESP finishes the upload body.
4. ESP raises HANDSHAKE HIGH → STM enters PLAY (24k). ESP reads the HTTP response
   and feeds playback frames; STM plays.
5. Playback drained → ESP lowers HANDSHAKE → back to RECORD.

Notes:
- Each side keeps a transaction queued; the master paces cadence per phase.
- Brief silence may play at step 4 before TTS bytes arrive — acceptable.
- Both sides use ring buffers to decouple SPI from HTTP / I2S.
