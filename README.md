# Voice-to-Chat Prototype

Real-time voice-to-text chat for Unreal Engine with cross-language translation, powered by a local Whisper server running entirely on-device.

## Overview

Players speak into their mic inside an Unreal Engine game. Audio is sent to a local C++ server that transcribes it with whisper.cpp and broadcasts the text to all connected clients. When players speak different languages, Whisper's built-in translate mode produces an English translation directly from the audio — no separate translation models needed. The English translation is shown alongside the original text for cross-language listeners.

## Architecture

```
┌─────────────────────────────┐       audio chunks        ┌──────────────────────────┐
│                             │  ───────────────────────►  │                          │
│   Unreal Engine Client      │                            │   Voice Server (C++)     │
│                             │  ◄───────────────────────  │                          │
│  - Captures mic audio       │   JSON over TCP            │  - Whisper STT (GPU)     │
│  - Sends audio to server    │                            │  - Whisper translate      │
│  - Displays text in chat UI │                            │  - Profanity filter      │
│  - GTA V-style language     │                            │  - Broadcast to clients  │
│    wheel (Ctrl+Shift+V)     │                            │                          │
└─────────────────────────────┘                            └──────────────────────────┘
```

## How Translation Works

Whisper has a built-in `translate` mode that converts speech in any supported language directly to English text. The server uses this instead of separate translation models:

1. Player speaks in any language (Japanese, Hindi, Korean, etc.)
2. Server runs Whisper **transcribe** → original text in speaker's language
3. Server runs Whisper **translate** → English translation from the same audio
4. Sender gets their original text back (profanity filtered)
5. Same-language listeners get the original text
6. Different-language listeners get the original + an English translation

This means any language → English works out of the box with high quality. English is used as the lingua franca for cross-language communication — no per-pair translation models needed.

## Components

### Voice Server (Standalone C++)

- **Speech-to-text** via whisper.cpp with the multilingual `ggml-small` model (99 languages)
- **Cross-language translation** via Whisper's translate mode (any language → English)
- **Per-client locale tracking** — each client declares their language, server only translates when needed
- **Language validation** — silently drops audio when detected language doesn't match client's locale
- **Profanity filter** — dictionary-based (~2,900 words), applied before broadcast
- **Multi-client support** with thread-per-client model and transcriber pool
- NVIDIA CUDA on Windows, Metal/Accelerate on macOS
- Zero configuration — all defaults built in

### Test Client (Standalone C++)

Standalone client for testing without Unreal Engine. Captures mic audio with automatic voice activity detection (VAD). Supports `--listen` for listen-only mode.

### Unreal Engine Client (Unreal C++)

Pure C++ integration — no Blueprints. See [docs/unreal-integration.md](docs/unreal-integration.md) for full source.

- **Push-to-talk** via Ctrl+V
- **GTA V-style language wheel** via Ctrl+Shift+V
- **In-game chat widget** — semi-transparent Slate overlay with auto-scrolling
- **Translation display** — original text in white, English translation indented below in gray
- **UTF-8 support** — composite font with CJK, Devanagari, Arabic fallbacks

## Wire Protocol

```
Client → Server:  [1-byte locale length][locale string][4-byte BE audio length][raw 16-bit PCM, 16kHz mono]
Server → Client:  [4-byte BE length][JSON response]
```

A zero-length audio message is a locale update (no transcription).

### JSON Responses

Sender receives (own language, profanity filtered):
```json
{"speaker":"Player1","locale":"ja","original":"こんにちは","flagged_words":[],"redacted":"こんにちは"}
```

Cross-language listener receives (with English translation):
```json
{"speaker":"Player1","locale":"ja","original":"こんにちは","flagged_words":[],"redacted":"こんにちは","translated":"Hello."}
```

Same-language listener receives (no translation):
```json
{"speaker":"Player1","locale":"ja","original":"こんにちは","flagged_words":[],"redacted":"こんにちは"}
```

Error:
```json
{"error":"No speech detected"}
```

## Quick Start

### Prerequisites

- **CMake 3.20+**
- **C++17 compiler** (Visual Studio 2022+ on Windows, Xcode CLI tools on macOS)
- **NVIDIA CUDA Toolkit 13.2+** (Windows, optional but recommended)

### Build and Run (Windows)

```bash
# 1. Clone
git clone https://github.com/brownking94/voice-unreal-chatbox.git
cd voice-unreal-chatbox

# 2. Build (first build takes 10-15 min for CUDA kernels)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
cmake --build build --parallel

# 3. Download Whisper model (~466 MB, one time)
powershell scripts/download_model.ps1

# 4. Run the server
build\voice-server.exe

# 5. In another terminal — test client
build\test-client.exe -l ja
```

### Build and Run (macOS)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
curl -L -o models/ggml-small.bin https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin
./build/voice-server
```

### Server Options

```
build\voice-server.exe [options]

  -m <path>    Whisper model (default: models/ggml-small.bin)
  -p <port>    TCP port (default: 9090)
  -w <workers> Parallel transcription instances (default: 1)
  -f <path>    Profanity word list (default: config/profanity.txt)
```

### Testing with Multiple Clients

```bash
# Terminal 1 — server
build\voice-server.exe

# Terminal 2 — Hindi speaker
build\test-client.exe -l hi

# Terminal 3 — English listener
build\test-client.exe -l en --listen

# Terminal 4 — Japanese listener
build\test-client.exe -l ja --listen
```

When the Hindi speaker talks, both listeners see the original Hindi text plus an English translation.

To build without CUDA (CPU only):
```bash
cmake -B build -DENABLE_CUDA=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

## Resource Budget (RTX 5070 8 GB)

| Component | VRAM | RAM |
|-----------|------|-----|
| Whisper small (1 worker) | ~1.0 GB | — |
| Profanity filter | — | ~10 MB |
| **Total server** | **~1.0 GB** | **~10 MB** |
| Unreal Engine game | 7 GB free | — |

## Project Structure

```
├── CMakeLists.txt               # Build config (fetches whisper.cpp + miniaudio)
├── src/
│   ├── main.cpp                 # Server entry point + translation handler
│   ├── server.h/cpp             # TCP server, broadcast with per-client translation
│   ├── transcriber.h/cpp        # Whisper inference (transcribe + translate modes)
│   ├── transcriber_pool.h/cpp   # Thread-safe pool of transcriber instances
│   ├── filter.h/cpp             # Profanity filter
│   └── protocol.h/cpp           # JSON message formatting
├── config/
│   └── profanity.txt            # Bad word list
├── scripts/
│   ├── setup.ps1                # Windows build script (sources vcvars64 + cmake)
│   ├── download_model.ps1       # Downloads whisper ggml-small.bin
│   └── update-profanity.sh      # Fetches & merges word lists from multiple repos
├── test_client/
│   └── test_client.cpp          # Test client with VAD
├── tests/
│   ├── test_protocol.cpp        # JSON protocol tests
│   ├── test_filter.cpp          # Profanity filter tests
│   └── test_wire_protocol.cpp   # Wire format tests
├── docs/
│   └── unreal-integration.md    # Full Unreal Engine client source code
└── models/
    └── ggml-small.bin           # Whisper model (gitignored)
```

## Supported Languages

Whisper supports 99 languages for transcription and any-to-English translation. The language wheel in the Unreal client shows 12 common languages:

en, zh, es, hi, ar, pt, ja, ko, fr, de, ru, it

## GPU Notes

- **Windows**: NVIDIA CUDA required for real-time performance. CPU-only is too slow.
- **macOS**: Metal/Accelerate — fast enough on Apple Silicon without explicit GPU setup.
- Targets sm_89 (Ada Lovelace) — forward-compatible with RTX 30/40/50 series.
- Avoid deleting `build/` — CUDA kernel compilation is slow. Incremental builds are fast.
