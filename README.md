# Voice-to-Chat Prototype

Real-time voice-to-text chat for Unreal Engine with cross-language translation, powered by a local Whisper server running entirely on-device.

## Overview

Players speak into their mic inside an Unreal Engine game. Audio is sent to a local C++ server that transcribes it with whisper.cpp and broadcasts the text to all connected clients. When players speak different languages, Whisper's built-in translate mode produces an English translation directly from the audio. The server decides what each listener sees — original text for same-language listeners, English for everyone else.

## Architecture

```
┌─────────────────────────────┐       audio chunks        ┌──────────────────────────┐
│                             │  ───────────────────────►  │                          │
│   Unreal Engine Client      │                            │   Voice Server (C++)     │
│                             │  ◄───────────────────────  │                          │
│  - Captures mic audio       │   JSON over TCP            │  - Whisper STT (GPU)     │
│  - Sends audio to server    │                            │  - Whisper translate     │
│  - Displays text in chat UI │                            │  - Profanity filter      │
│  - GTA V-style language     │                            │  - Broadcast to clients  │
│    wheel (Ctrl+Shift+V)     │                            │                          │
└─────────────────────────────┘                            └──────────────────────────┘
```

## How Translation Works

Whisper handles both transcription and translation in a single model. The server runs two passes on non-English audio:

1. Player speaks in any language (Japanese, Hindi, Korean, etc.)
2. **Pass 1**: Whisper transcribe → original text in speaker's language
3. **Pass 2**: Whisper translate → English translation from the same audio (skipped if already English)
4. Profanity filter applied to original text
5. Server broadcasts to all clients, picking the right text per listener:
   - **Same language** → original text in speaker's language
   - **Different language** → English translation

English speech is always accepted regardless of the client's locale setting, so bilingual players (e.g. Hindi + English) work naturally. Auto language detection means the server figures out what language was spoken — the client locale is only used for broadcast routing.

## Components

### Voice Server (Standalone C++)

- **Speech-to-text** via whisper.cpp with the multilingual `ggml-small` model (99 languages)
- **Cross-language translation** via Whisper's translate mode (any language → English)
- **Auto language detection** — Whisper detects the spoken language, no client hint needed
- **Per-client locale routing** — each client declares their language, server sends the right text
- **English always accepted** — bilingual speakers mixing in English phrases work naturally
- **Profanity filter** — dictionary-based (~2,900 words), applied before broadcast
- **Multi-client support** with thread-per-client model and transcriber pool
- NVIDIA CUDA on Windows, Metal/Accelerate on macOS
- Zero configuration — all defaults built in

### Test Client (Standalone C++)

Standalone client for testing without Unreal Engine. Captures mic audio with automatic voice activity detection (VAD). Supports `--listen` for listen-only mode (receives broadcasts without mic capture).

### Unreal Engine Client (Unreal C++)

Pure C++ integration — no Blueprints. See [docs/unreal-integration.md](docs/unreal-integration.md) for full source.

- **Push-to-talk** via Ctrl+V
- **GTA V-style language wheel** via Ctrl+Shift+V (20 languages)
- **In-game chat widget** — semi-transparent Slate overlay with auto-scrolling
- **UTF-8 support** — composite font with CJK, Devanagari, Arabic, Thai fallbacks

## Wire Protocol

```
Client → Server:  [1-byte locale length][locale string][4-byte BE audio length][raw 16-bit PCM, 16kHz mono]
Server → Client:  [4-byte BE length][JSON response]
```

A zero-length audio message is a locale registration/update (no transcription).

### JSON Response

The server sends a simple message with the appropriate text already chosen per listener:

```json
{"speaker":"Player1","locale":"hi","text":"The enemy is over there."}
```

- `speaker` — player ID
- `locale` — detected language of the speech
- `text` — the display text (original for same-language, English translation for others)

No speech detected or empty audio is silently ignored (no response sent).

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

When the Hindi speaker talks:
- The Hindi listener sees the original Hindi text
- The English and Japanese listeners see the English translation

The Hindi speaker can also say something in English — it gets detected as English and sent to everyone as-is.

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
│   ├── main.cpp                 # Server entry point + broadcast translation logic
│   ├── server.h/cpp             # TCP server, per-client broadcast routing
│   ├── transcriber.h/cpp        # Whisper inference (transcribe + translate passes)
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

Whisper supports 99 languages for transcription and any-to-English translation. The language wheel in the Unreal client shows 20 languages:

en, zh, es, hi, ar, pt, ja, ko, fr, de, ru, it, nl, pl, tr, sv, th, vi, id, cs

## GPU Notes

- **Windows**: NVIDIA CUDA required for real-time performance. CPU-only is too slow.
- **macOS**: Metal/Accelerate — fast enough on Apple Silicon without explicit GPU setup.
- Targets sm_89 (Ada Lovelace) — forward-compatible with RTX 30/40/50 series.
- Avoid deleting `build/` — CUDA kernel compilation is slow. Incremental builds are fast.
