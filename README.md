# Voice-to-Chat Prototype

Real-time voice-to-text chat for Unreal Engine, powered by a local Whisper transcription server running on a gaming laptop.

## Overview

This prototype captures a player's voice inside an Unreal Engine open world demo, sends the audio to a local transcription server built with whisper.cpp, and displays the transcribed text in an in-game chat widget. No cloud services, no translation for now — just English speech to English text, running entirely on-device.

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
- Runs Whisper inference using the `ggml-medium.en` model (configurable)
- Returns transcribed text as JSON (e.g. `{"speaker": "Player1", "text": "anyone see that dragon?"}`)
- **Multi-client support** with a thread-per-client model and a transcriber pool for parallel inference
- Uses Metal/Accelerate on macOS, CPU (or CUDA) on Windows
- Cross-platform: macOS and Windows

### Test Client (Standalone C++)

A standalone client for testing the server without Unreal Engine. Captures live audio from the microphone with automatic voice activity detection (VAD). Detects when you start and stop speaking, then sends the audio chunk to the server automatically.

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
3. **Trailing silence** — Speech stopped. Waits 1.2 seconds to confirm the speaker is done.
4. **Send** — Chunk is sent to the server for transcription. Returns to idle.

If the speaker talks for longer than 30 seconds, the chunk is force-sent and a new chunk begins recording immediately with no gap — this uses a double-buffer design where the callback writes to one buffer while the main loop drains the other.

## Multi-Client & Transcriber Pool

The server supports multiple simultaneous clients. Each client connection is handled in its own thread, and transcription runs through a **pool of pre-loaded whisper model instances**.

- At startup, the server loads N model instances into memory (default: 2)
- When a client sends audio, a free instance is borrowed from the pool
- Multiple clients talking at the same time get truly parallel transcription (up to N concurrent)
- If all instances are busy, the next request waits until one frees up (~1-2 seconds)
- Memory is fixed at startup — no per-client allocation
- Each client is assigned a unique ID (Player1, Player2, etc.) that appears in logs and JSON responses

| Workers | RAM (medium.en) | Parallel transcriptions |
|---------|-----------------|------------------------|
| 1 | ~2.6 GB | Sequential only |
| 2 (default) | ~5.2 GB | 2 concurrent |
| 4 | ~10.4 GB | 4 concurrent |

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

Length-prefixed messages over TCP:

```
Client → Server:  [4-byte big-endian uint32 length][raw 16-bit PCM audio, 16 kHz mono]
Server → Client:  [4-byte big-endian uint32 length][JSON response]
```

JSON responses (speaker is assigned per client connection):
```json
{"speaker":"Player1","original":" anyone see that dragon?","flagged_words":[],"redacted":" anyone see that dragon?"}
{"speaker":"Player2","original":" what the fuck is that","flagged_words":["fuck"],"redacted":" what the **** is that"}
{"error":"No speech detected"}
```

## Quick Start

### Prerequisites

- CMake 3.20+
- C++17 compiler (clang, MSVC, or gcc)
- macOS or Windows

### Build and Run

```bash
# Build everything (fetches whisper.cpp and miniaudio automatically)
make build

# Download the whisper model (~1.5 GB, only needed once)
make model

# Update profanity word list from all sources
make update-filter

# Start the server (auto-downloads model if missing)
make run-server

# In another terminal — start the mic client
make run-client
```

### Available Whisper Models

| Model | Size | Quality | `MODEL_PATH` |
|-------|------|---------|-------------|
| `tiny.en` | ~75 MB | Basic | `models/ggml-tiny.en.bin` |
| `base.en` | ~142 MB | Good | `models/ggml-base.en.bin` |
| `small.en` | ~466 MB | Great | `models/ggml-small.en.bin` |
| `medium.en` | ~1.5 GB | Excellent (default) | `models/ggml-medium.en.bin` |
| `large-v3` | ~3 GB | Best (multilingual) | `models/ggml-large-v3.bin` |

Override the default model:
```bash
make run-server MODEL_PATH=models/ggml-small.en.bin
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

## Scope and Constraints

- **Language:** English voice to English text only (no translation)
- **Deployment:** Runs locally on a single machine (Apple Silicon or RTX 3060+ recommended)
- **Game project:** Any Unreal Engine open world demo — the chat system is game-agnostic
- **Goal:** Working prototype, not production quality

## Future Considerations

These are out of scope for the prototype but worth noting for later iterations.

- Multi-language translation using a local LLM (e.g. llama.cpp) as a second processing stage
- Speaker identification (currently all transcriptions are labeled "Player1")
- Streaming / partial transcription display (text appears word by word)
- Packaging the Whisper server as a DLL loaded directly by Unreal instead of a separate process
