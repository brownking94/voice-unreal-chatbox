# Voice-to-Chat Prototype

Real-time voice-to-text chat for Unreal Engine, powered by a local Whisper transcription server running on a gaming laptop.

## Overview

This prototype captures a player's voice inside an Unreal Engine open world demo, sends the audio to a local transcription server built with whisper.cpp, and displays the transcribed text in an in-game chat widget. No cloud services — just local speech-to-text supporting 99 languages, running entirely on-device.

## Architecture

The system is two separate processes communicating over a local TCP socket.

```
┌─────────────────────────────┐       audio chunks        ┌──────────────────────────┐
│                             │  ───────────────────────►  │                          │
│   Unreal Engine Client      │                            │   Whisper Server (C++)   │
│                             │  ◄───────────────────────  │                          │
│  - Captures mic audio       │     transcribed text       │  - Receives audio data   │
│  - Sends audio to server    │      (JSON over TCP)       │  - Runs Whisper inference │
│  - Displays text in chat UI │                            │  - Returns transcription  │
│                             │                            │                          │
└─────────────────────────────┘                            └──────────────────────────┘
```

## Components

### Whisper Transcription Server (Standalone C++)

A lightweight local server built with standard C++ and whisper.cpp. It has no dependency on Unreal Engine.

- Listens on a local TCP socket for incoming audio data
- Runs Whisper inference using the multilingual `ggml-small` model (99 languages, configurable)
- Returns transcribed text as JSON (e.g. `{"speaker": "Player1", "text": "anyone see that dragon?"}`)
- **Multi-client support** with a thread-per-client model and a transcriber pool for parallel inference
- Uses Metal/Accelerate on macOS, NVIDIA CUDA on Windows (falls back to CPU)
- Cross-platform: macOS and Windows

### Test Client (Standalone C++)

A standalone client for testing the server without Unreal Engine. Captures live audio from the microphone with automatic voice activity detection (VAD). Detects when you start and stop speaking, then sends the audio chunk to the server automatically. Supports a `--listen` flag for listen-only mode (receives broadcasts without capturing mic).

### Unreal Engine Client (Unreal C++ / Blueprints) — not yet implemented

A thin integration layer added on top of an existing open world demo project. No modifications to the base game.

- Captures microphone input using Unreal's built-in audio capture (e.g. `UAudioCaptureComponent`)
- Streams raw audio chunks to the Whisper server over TCP
- Receives transcribed text back from the server
- Displays text in a UMG chat widget overlaid on the game HUD

## Voice Activity Detection (VAD)

The test client uses energy-based VAD to automatically detect speech and manage audio chunking:

1. **Idle** — Mic is listening but discarding audio. No memory growth.
2. **Recording** — Speech detected (RMS energy above threshold). Audio is buffered.
3. **Trailing silence** — Speech stopped. Waits 700ms to confirm the speaker is done.
4. **Send** — Chunk is sent to the server for transcription. Returns to idle.

If the speaker talks for longer than 10 seconds, the chunk is force-sent and a new chunk begins recording immediately with no gap — this uses a double-buffer design where the callback writes to one buffer while the main loop drains the other.

## Multi-Client & Transcriber Pool

The server supports multiple simultaneous clients. Each client connection is handled in its own thread, and transcription runs through a **pool of pre-loaded whisper model instances**.

- At startup, the server loads N model instances into memory (default: 2)
- When a client sends audio, a free instance is borrowed from the pool
- Multiple clients talking at the same time get truly parallel transcription (up to N concurrent)
- If all instances are busy, the next request waits until one frees up (~1-2 seconds)
- Memory is fixed at startup — no per-client allocation
- Each client is assigned a unique ID (Player1, Player2, etc.) that appears in logs and JSON responses

| Workers | VRAM (small) | Parallel transcriptions |
|---------|----------------|------------------------|
| 1 | ~1.0 GB | Sequential only |
| 2 (default) | ~2.0 GB | 2 concurrent |
| 4 | ~4.0 GB | 4 concurrent |

Override at launch:
```bash
make run-server WORKERS=4
```

## Profanity Filter

Transcribed text is run through a profanity filter before being returned to the client. The filter uses a dictionary-based approach with a configurable word list.

- Loaded into memory at server startup from `config/profanity.txt` (~2,900 words from 4 open source lists)
- Single words are stored in a hash set (`std::unordered_set`) for O(1) lookups
- Multi-word phrases are stored separately and matched via substring scan
- Case-insensitive matching with word boundary detection
- Flagged words are replaced with asterisks in the redacted output
- The response includes the original text, list of flagged words, and redacted version

### Updating the word list

The word list is aggregated from multiple open source repositories. Run the update script to fetch the latest from all sources and merge with any custom words you've added locally:

```bash
make update-filter
```

Sources:
- [LDNOOBW](https://github.com/LDNOOBW/List-of-Dirty-Naughty-Obscene-and-Otherwise-Bad-Words) — curated list (~400 words)
- [zacanger/profane-words](https://github.com/zacanger/profane-words) — comprehensive list (~2,700 words)
- [web-mech/badwords](https://github.com/web-mech/badwords) — npm bad-words package (~450 words)
- [better_profanity](https://github.com/snguyenthanh/better_profanity) — Python profanity library (~800 words)

To add a new source, edit the `SOURCES` array in `scripts/update-profanity.sh`.

Custom word list path:
```bash
make run-server FILTER_PATH=config/my-custom-list.txt
```

### Moderation pipeline

When `flagged_words` is non-empty, the event can be forwarded to a separate moderation service for further action. This keeps the voice server fast (filter and respond immediately) while offloading enforcement decisions to a dedicated system.

```
audio → transcribe → filter → respond to client (redacted text)
                        │
                        └──► if flagged_words not empty:
                                push to moderation queue
                                    │
                                    ▼
                              moderation service
                              - track per-player flag frequency
                              - issue warnings after N flags
                              - temporary mute after repeated offenses
                              - escalate to human review if needed
                              - log original text for audit trail
```

This is not yet implemented but the response format is designed to support it — the `flagged_words` array gives downstream services exactly what they need to decide on an action without re-parsing the text.

## Wire Protocol

Stateless, length-prefixed messages over TCP. Each audio message includes the client's locale so the server requires no per-client state.

```
Client → Server:  [1-byte locale length][locale string, e.g. "en"][4-byte big-endian uint32 audio length][raw 16-bit PCM audio, 16 kHz mono]
Server → Client:  [4-byte big-endian uint32 length][JSON response]
```

The locale is a whisper language code (e.g. `en`, `ja`, `ko`, `zh`, `auto`). It is sent with every audio chunk, allowing the server to remain completely stateless — no per-client session tracking needed.

JSON responses (speaker is assigned per client connection, locale is the sender's language):
```json
{"speaker":"Player1","locale":"en","original":" anyone see that dragon?","flagged_words":[],"redacted":" anyone see that dragon?"}
{"speaker":"Player2","locale":"en","original":" what the fuck is that","flagged_words":["fuck"],"redacted":" what the **** is that"}
{"error":"No speech detected"}
```

## Quick Start

### Prerequisites

- **Git** (to clone the repo)
- **CMake 3.20+** ([cmake.org/download](https://cmake.org/download/))
- **C++17 compiler**:
  - **Windows**: Visual Studio 2022+ with "Desktop development with C++" workload
  - **macOS**: Xcode command line tools (`xcode-select --install`)
- **NVIDIA CUDA Toolkit 13.2+** (Windows only, optional): [developer.nvidia.com/cuda-downloads](https://developer.nvidia.com/cuda-downloads). Required for GPU-accelerated inference. Without it, falls back to CPU (too slow for real-time use)
- **NVIDIA driver**: Must support CUDA 13.2+. Run `nvidia-smi` to check — the "CUDA Version" in the top-right must be >= 13.2. Update to the latest Game Ready or Studio driver if needed. Both Game Ready and Studio drivers work

### Build and Run

#### macOS (Make)

```bash
# Build everything (fetches whisper.cpp and miniaudio automatically)
make build

# Download the multilingual whisper model (~466 MB, only needed once)
make model

# Start the server (auto-downloads model if missing)
make run-server

# In another terminal — start the mic client
make run-client
```

#### Windows (CMake + CUDA)

From a fresh clone:

```bash
# 1. Verify CUDA is available (CUDA Version must be >= 13.2)
nvidia-smi

# 2. Clone the repo
git clone https://github.com/brownking94/voice-unreal-chatbox.git
cd voice-unreal-chatbox

# 3. Configure — only needed once, or after editing CMakeLists.txt
#    CUDA is enabled by default when NVIDIA CUDA Toolkit is installed
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo

# 4. Build — first build takes 10-15 min (compiling CUDA kernels)
#    Subsequent builds are incremental and only recompile changed files
cmake --build build --config RelWithDebInfo

# 5. Download the multilingual whisper model (~466 MB, only needed once)
mkdir models
curl -L -o models/ggml-small.bin https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin

# 6. Start the server
build\RelWithDebInfo\voice-server.exe -m models/ggml-small.bin -p 9090 -w 2 -f config/profanity.txt

# 7. In another terminal — start the mic client (English)
build\RelWithDebInfo\test-client.exe -p 9090 -l en

# 8. Verify GPU is being used (should show ~1500 MiB allocated)
nvidia-smi
```

#### Testing broadcast with multiple clients

The server broadcasts transcriptions to all connected clients. To test with one mic:

```bash
# Terminal 1 — server
build\RelWithDebInfo\voice-server.exe -m models/ggml-small.bin -p 9090 -w 2 -f config/profanity.txt

# Terminal 2 — speaking client (captures mic, sends audio)
build\RelWithDebInfo\test-client.exe -p 9090 -l en

# Terminal 3 — listen-only client (receives broadcasts, no mic capture)
build\RelWithDebInfo\test-client.exe -p 9090 -l ja --listen
```

When you speak in Terminal 2, both Terminal 2 and Terminal 3 display the transcription. The listen-only client registers its locale with the server (for future translation routing) but never captures audio.

To build without CUDA (CPU only), pass `-DENABLE_CUDA=OFF` during configure:
```bash
cmake -B build -DENABLE_CUDA=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config RelWithDebInfo
```

> **Note:** Avoid deleting the `build/` directory unless necessary — CUDA kernel compilation is slow. Incremental builds only recompile what changed.

Shared libraries (ggml, whisper, ggml-cuda, etc.) are automatically copied next to the executables after each build.

### Available Whisper Models

**Multilingual models** (99 languages — recommended):

| Model | Size | Quality | Languages | `MODEL_PATH` |
|-------|------|---------|-----------|-------------|
| `tiny` | ~75 MB | Basic | 99 | `models/ggml-tiny.bin` |
| `base` | ~142 MB | Good | 99 | `models/ggml-base.bin` |
| **`small`** | **~466 MB** | **Great (default)** | **99** | **`models/ggml-small.bin`** |
| `medium` | ~1.5 GB | Excellent | 99 | `models/ggml-medium.bin` |
| `large-v3` | ~3 GB | Best | 99 | `models/ggml-large-v3.bin` |

**English-only models** (slightly better WER for English):

| Model | Size | Quality | `MODEL_PATH` |
|-------|------|---------|-------------|
| `small.en` | ~466 MB | Great | `models/ggml-small.en.bin` |
| `medium.en` | ~1.5 GB | Excellent | `models/ggml-medium.en.bin` |

**NVIDIA GPU estimates** (for a ~5s audio clip with CUDA):

| Model | RTX 3060 | RTX 3080 | RTX 4090 |
|-------|----------|----------|----------|
| `tiny.en` | ~0.1s | ~0.05s | ~0.03s |
| `base.en` | ~0.2s | ~0.1s | ~0.05s |
| `small.en` | ~0.5s | ~0.3s | ~0.15s |
| `medium.en` | ~1.5s | ~0.8s | ~0.4s |
| `large-v3` | ~4s | ~2s | ~1s |

Total latency = VAD silence timeout (700ms) + inference + network (<1ms localhost) + filter (<1ms).
Inference time scales roughly linearly with audio length. Expect 2-3x slower on CPU-only compared to GPU.

Download any model with:
```bash
curl -L -o models/<model_file> https://huggingface.co/ggerganov/whisper.cpp/resolve/main/<model_file>
```

Override the default model:
```bash
make run-server MODEL_PATH=models/ggml-medium.bin
```

### Project Structure

```
├── CMakeLists.txt               # Build config (fetches whisper.cpp + miniaudio)
├── Makefile                     # Convenience targets for build/run
├── src/
│   ├── main.cpp                 # Server entry point
│   ├── server.h/cpp             # Cross-platform TCP socket server
│   ├── transcriber.h/cpp        # Whisper inference wrapper
│   ├── transcriber_pool.h/cpp   # Thread-safe pool of transcriber instances
│   ├── filter.h/cpp             # Profanity filter (dictionary-based)
│   └── protocol.h/cpp           # JSON message formatting
├── config/
│   └── profanity.txt            # Bad word list (one per line, editable)
├── scripts/
│   └── update-profanity.sh      # Fetches & merges word lists from multiple repos
├── test_client/
│   └── test_client.cpp          # Test client (live mic with VAD)
└── models/                      # Whisper model files (gitignored)
```

## GPU Notes

- **macOS**: Uses Metal/Accelerate automatically. Apple Silicon's unified memory and AMX coprocessor make CPU-only inference fast enough for real-time use even without explicit GPU offload.
- **Windows**: Requires NVIDIA CUDA. Without GPU acceleration, x86 CPU inference is too slow for real-time use (~2x realtime for `small.en`).
- **Consumer Blackwell GPUs** (RTX 5070/5080/5090): The build targets sm_89 (Ada Lovelace) instead of sm_120a. This is because ggml's CUDA backend enables MXFP4 block-scale MMA instructions on sm_120a that only work on data-center Blackwell chips (B200), not consumer cards. sm_89 is forward-compatible and runs correctly on all RTX 30/40/50 series GPUs.
- **Driver version matters**: The NVIDIA driver must support the CUDA version the code was compiled against (13.2). If the driver is too old, the CUDA backend will silently fail and inference falls back to CPU with no error message. Update to the latest Game Ready driver to fix this.

### Verifying GPU usage

Run `nvidia-smi` after starting the server:

```
nvidia-smi
```

You should see GPU memory allocated by the server even before any client connects (the model is loaded at startup):

```
|   0  NVIDIA GeForce RTX 5070 ...  |   1518MiB /   8151MiB |      0%      Default |
```

If GPU memory shows **0 MiB** with the server running, CUDA is not working. Common causes:
1. **Driver too old** — run `nvidia-smi` and check "CUDA Version" in the top-right is >= 13.2
2. **Missing ggml-cuda.dll** — verify it exists next to the executable in `build/RelWithDebInfo/`
3. **Wrong CUDA architecture** — rebuild after checking CMakeLists.txt targets a compatible sm version

## Scope and Constraints

- **Language:** 99 languages for speech-to-text (translation not yet implemented)
- **Deployment:** Runs locally on a single machine (Apple Silicon or RTX 3060+ recommended)
- **Game project:** Any Unreal Engine open world demo — the chat system is game-agnostic
- **Goal:** Working prototype, not production quality

## Future Considerations

These are out of scope for the prototype but worth noting for later iterations.

- Multi-language translation using NLLB-200 (200 languages, ~1.2 GB) as a second processing stage after STT
- Speaker identification (currently all transcriptions are labeled "Player1")
- Streaming / partial transcription display (text appears word by word)
- Packaging the Whisper server as a DLL loaded directly by Unreal instead of a separate process
