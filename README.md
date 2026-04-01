# Voice-to-Chat Prototype

Real-time voice-to-text chat for Unreal Engine with live translation, powered by a local Whisper + NLLB transcription server running on a gaming laptop.

## Overview

This prototype captures a player's voice inside an Unreal Engine open world demo, sends the audio to a local transcription server built with whisper.cpp, and displays the transcribed text in an in-game chat widget. When multiple players speak different languages, the server translates each message into every other player's language using NLLB-200 before broadcasting. No cloud services — just local speech-to-text and translation running entirely on-device.

## Architecture

The system is two separate processes communicating over a local TCP socket.

```
┌─────────────────────────────┐       audio chunks        ┌──────────────────────────┐
│                             │  ───────────────────────►  │                          │
│   Unreal Engine Client      │                            │   Voice Server (C++)     │
│                             │  ◄───────────────────────  │                          │
│  - Captures mic audio       │   translated text          │  - Whisper STT           │
│  - Sends audio to server    │   (JSON over TCP)          │  - NLLB-200 translation  │
│  - Displays text in chat UI │                            │  - Profanity filter      │
│  - GTA V-style language     │                            │  - Broadcast to clients  │
│    wheel (Ctrl+Shift+V)     │                            │                          │
└─────────────────────────────┘                            └──────────────────────────┘
```

## Components

### Voice Server (Standalone C++)

A local server built with standard C++ that handles the full voice-to-translated-text pipeline.

- Listens on a local TCP socket for incoming audio data
- **Speech-to-text** via whisper.cpp using the multilingual `ggml-small` model (99 languages)
- **Auto language detection** — Whisper runs with `auto` detect, then validates against English + the client's secondary language. If the detected language doesn't match either, re-runs as English
- **Live translation** via NLLB-200 (CTranslate2 + SentencePiece). When broadcasting to other clients, the server translates the transcription into each receiver's language. If source and target language match, no translation is performed
- **Profanity filter** — dictionary-based, applied before broadcast
- **Multi-client support** with a thread-per-client model and a transcriber pool for parallel inference
- Uses NVIDIA CUDA on Windows (falls back to CPU), Metal/Accelerate on macOS
- All flags have sensible defaults — server runs with zero configuration

### Test Client (Standalone C++)

A standalone client for testing the server without Unreal Engine. Captures live audio from the microphone with automatic voice activity detection (VAD). Detects when you start and stop speaking, then sends the audio chunk to the server automatically. Supports a `--listen` flag for listen-only mode (receives broadcasts without capturing mic).

### Unreal Engine Client (Unreal C++)

A pure C++ integration layer added on top of an existing Unreal Engine project. No Blueprints required.

- **Push-to-talk** via Ctrl+V
- **GTA V-style language wheel** via Ctrl+Shift+V — radial selector with 12 languages, mouse scroll to pick, left-click to confirm, right-click to dismiss
- **TCP connection** to the voice server on port 9090
- **Stateless locale protocol** — sends secondary language with every audio message (English is always on)
- **Broadcast support** — receives translated transcriptions from all connected clients
- **In-game chat widget** — semi-transparent Slate overlay at bottom-left, auto-scrolling, recording indicator
- **Proper UTF-8 handling** — supports CJK and other non-Latin scripts in the chat display

## Language Detection & Translation

The server uses a dual-language approach: **English is always on**, and players set a secondary language.

1. Player speaks → audio sent to server with their secondary language code
2. Server runs Whisper with `auto` language detection
3. If detected language is English or the player's secondary language → accepted
4. If detected language is something else → re-run forced as English
5. On broadcast, the server translates the text into each receiver's secondary language
6. If source language matches the receiver's language → sent as-is (no translation)

This means:
- English speakers who never touch the language wheel get perfect behavior
- Bilingual players set their second language once and speak naturally in either
- A Japanese player speaking Japanese gets their message translated to English for English-speaking players, and vice versa

### Supported Languages (Language Wheel)

| Code | Language | NLLB Code |
|------|----------|-----------|
| `en` | English | `eng_Latn` |
| `zh` | Chinese | `zho_Hans` |
| `es` | Spanish | `spa_Latn` |
| `hi` | Hindi | `hin_Deva` |
| `ar` | Arabic | `arb_Arab` |
| `pt` | Portuguese | `por_Latn` |
| `ja` | Japanese | `jpn_Jpan` |
| `ko` | Korean | `kor_Hang` |
| `fr` | French | `fra_Latn` |
| `de` | German | `deu_Latn` |
| `ru` | Russian | `rus_Cyrl` |
| `it` | Italian | `ita_Latn` |

Whisper supports 99 languages for STT. NLLB-200 supports 200 languages for translation. The language wheel shows the 12 most common; the server can handle any language pair NLLB supports.

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

## Wire Protocol

Stateless, length-prefixed messages over TCP. Each audio message includes the client's secondary language so the server requires no per-client state for STT. The server tracks each client's locale for translation routing.

```
Client → Server:  [1-byte locale length][locale string, e.g. "ja"][4-byte big-endian uint32 audio length][raw 16-bit PCM audio, 16 kHz mono]
Server → Client:  [4-byte big-endian uint32 length][JSON response]
```

The locale represents the client's **secondary language** (English is always on). It is sent with every audio chunk. A zero-length audio message is a registration/locale update — the server updates the client's stored language for translation routing.

JSON responses:
```json
{"speaker":"Player1","locale":"en","original":" anyone see that dragon?","flagged_words":[],"redacted":" anyone see that dragon?"}
{"speaker":"Player2","locale":"ja","original":" あのドラゴン見た？","flagged_words":[],"redacted":" あのドラゴン見た？"}
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
- **NVIDIA driver**: Must support CUDA 13.2+. Run `nvidia-smi` to check — the "CUDA Version" in the top-right must be >= 13.2. Update to the latest Game Ready or Studio driver if needed

### Build and Run

#### Windows (CMake + CUDA)

From a fresh clone:

```bash
# 1. Verify CUDA is available (CUDA Version must be >= 13.2)
nvidia-smi

# 2. Clone the repo
git clone https://github.com/brownking94/voice-unreal-chatbox.git
cd voice-unreal-chatbox

# 3. Configure and build (first build takes 15-20 min — CUDA kernels for whisper + CTranslate2)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
cmake --build build --config RelWithDebInfo --parallel

# 4. Download models (only needed once)
#    Whisper STT model (~466 MB)
curl -L -o models/ggml-small.bin https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin

#    NLLB-200 translation model (~2.4 GB total)
mkdir models\nllb-200-distilled-600M
curl -L -o models/nllb-200-distilled-600M/model.bin https://huggingface.co/entai2965/nllb-200-distilled-600M-ctranslate2/resolve/main/model.bin
curl -L -o models/nllb-200-distilled-600M/config.json https://huggingface.co/entai2965/nllb-200-distilled-600M-ctranslate2/resolve/main/config.json
curl -L -o models/nllb-200-distilled-600M/shared_vocabulary.json https://huggingface.co/entai2965/nllb-200-distilled-600M-ctranslate2/resolve/main/shared_vocabulary.json
curl -L -o models/nllb-200-distilled-600M/sentencepiece.bpe.model https://huggingface.co/entai2965/nllb-200-distilled-600M-ctranslate2/resolve/main/sentencepiece.bpe.model

# 5. Start the server (all defaults — STT + translation + filter)
build\voice-server.exe

# 6. In another terminal — start the mic client
build\test-client.exe -l ja

# 7. Verify GPU is being used (should show ~2-3 GB allocated)
nvidia-smi
```

#### macOS (Make)

```bash
make build
make model
make run-server
# In another terminal:
make run-client
```

#### Server options

All flags are optional — sensible defaults are built in:

```
build\voice-server.exe [options]

  -m <path>    Whisper model (default: models/ggml-small.bin)
  -p <port>    TCP port (default: 9090)
  -w <workers> Parallel transcription instances (default: 2)
  -f <path>    Profanity word list (default: config/profanity.txt)
  -t <path>    NLLB translation model dir (default: models/nllb-200-distilled-600M)
  -s <path>    SentencePiece tokenizer (default: models/nllb-200-distilled-600M/sentencepiece.bpe.model)
```

#### Testing broadcast with multiple clients

```bash
# Terminal 1 — server
build\voice-server.exe

# Terminal 2 — English speaker with Japanese as secondary
build\test-client.exe -l ja

# Terminal 3 — Japanese listener
build\test-client.exe -l ja --listen
```

When you speak English in Terminal 2, Terminal 3 receives the Japanese translation. When you speak Japanese, Terminal 2 receives the English translation.

To build without CUDA (CPU only):
```bash
cmake -B build -DENABLE_CUDA=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

To build without translation:
```bash
cmake -B build -DENABLE_TRANSLATION=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

> **Note:** Avoid deleting the `build/` directory unless necessary — CUDA kernel compilation is slow. Incremental builds only recompile what changed.

### VRAM Budget (RTX 5070 8 GB)

| Component | VRAM |
|-----------|------|
| Whisper small (x2 workers) | ~2.0 GB |
| NLLB-200-distilled-600M | ~0.6 GB |
| **Total** | **~2.6 GB** |

### Project Structure

```
├── CMakeLists.txt               # Build config (fetches whisper.cpp, CTranslate2, SentencePiece, miniaudio)
├── Makefile                     # Convenience targets for build/run (macOS)
├── src/
│   ├── main.cpp                 # Server entry point
│   ├── server.h/cpp             # Cross-platform TCP socket server + broadcast with translation
│   ├── transcriber.h/cpp        # Whisper inference wrapper (returns text + detected language)
│   ├── transcriber_pool.h/cpp   # Thread-safe pool of transcriber instances
│   ├── translator.h/cpp         # NLLB-200 translation via CTranslate2 + SentencePiece
│   ├── filter.h/cpp             # Profanity filter (dictionary-based)
│   └── protocol.h/cpp           # JSON message formatting
├── config/
│   └── profanity.txt            # Bad word list (one per line, editable)
├── scripts/
│   └── update-profanity.sh      # Fetches & merges word lists from multiple repos
├── test_client/
│   └── test_client.cpp          # Test client (live mic with VAD)
├── tests/
│   ├── test_protocol.cpp        # JSON protocol unit tests
│   ├── test_filter.cpp          # Profanity filter unit tests
│   ├── test_wire_protocol.cpp   # Wire format unit tests
│   └── test_translator.cpp      # NLLB locale mapping tests
└── models/                      # Model files (gitignored)
    ├── ggml-small.bin           # Whisper STT model
    └── nllb-200-distilled-600M/ # NLLB translation model
```

## GPU Notes

- **macOS**: Uses Metal/Accelerate automatically. Apple Silicon's unified memory and AMX coprocessor make CPU-only inference fast enough for real-time use even without explicit GPU offload.
- **Windows**: Requires NVIDIA CUDA. Without GPU acceleration, x86 CPU inference is too slow for real-time use (~2x realtime for `small.en`).
- **Consumer Blackwell GPUs** (RTX 5070/5080/5090): The build targets sm_89 (Ada Lovelace) instead of sm_120a. This is because ggml's CUDA backend enables MXFP4 block-scale MMA instructions on sm_120a that only work on data-center Blackwell chips (B200), not consumer cards. sm_89 is forward-compatible and runs correctly on all RTX 30/40/50 series GPUs.
- **Driver version matters**: The NVIDIA driver must support the CUDA version the code was compiled against (13.2). If the driver is too old, the CUDA backend will silently fail and inference falls back to CPU with no error message. Update to the latest Game Ready driver to fix this.

### Verifying GPU usage

Run `nvidia-smi` after starting the server:

```
|   0  NVIDIA GeForce RTX 5070 ...  |   2600MiB /   8151MiB |      0%      Default |
```

If GPU memory shows **0 MiB** with the server running, CUDA is not working. Common causes:
1. **Driver too old** — run `nvidia-smi` and check "CUDA Version" in the top-right is >= 13.2
2. **Missing DLLs** — verify ggml-cuda.dll and ctranslate2.dll exist next to the executable
3. **Wrong CUDA architecture** — rebuild after checking CMakeLists.txt targets a compatible sm version

## Scope and Constraints

- **Language:** 99 languages for speech-to-text, 200 languages for translation
- **Deployment:** Runs locally on a single machine (RTX 3060+ recommended for GPU acceleration)
- **Game project:** Any Unreal Engine project — the chat system is game-agnostic
- **Goal:** Working prototype, not production quality

## Future Considerations

- Speaker identification (currently all transcriptions are labeled "Player1", "Player2", etc.)
- Streaming / partial transcription display (text appears word by word)
- Packaging the voice server as a DLL loaded directly by Unreal instead of a separate process
- Translation quality tuning for game-specific vocabulary
