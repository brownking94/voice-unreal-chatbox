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
- Uses Metal/Accelerate on macOS, CPU (or CUDA) on Windows
- Cross-platform: macOS and Windows

### Test Client (Standalone C++)

A standalone client for testing the server without Unreal Engine. Supports two modes:

- **Mic mode (`--mic`):** Captures live audio from the microphone with automatic voice activity detection (VAD). Detects when you start and stop speaking, then sends the audio chunk to the server automatically.
- **WAV mode:** Sends a pre-recorded WAV file to the server for transcription.

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

## Wire Protocol

Length-prefixed messages over TCP:

```
Client → Server:  [4-byte big-endian uint32 length][raw 16-bit PCM audio, 16 kHz mono]
Server → Client:  [4-byte big-endian uint32 length][JSON response]
```

JSON responses:
```json
{"speaker": "Player1", "text": "anyone see that dragon?"}
{"error": "No speech detected"}
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

# Start the server (auto-downloads model if missing)
make run-server

# In another terminal — live mic with VAD
make run-mic

# Or send a WAV file
make run-client WAV_FILE=path/to/audio.wav
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
├── CMakeLists.txt          # Build config (fetches whisper.cpp + miniaudio)
├── Makefile                # Convenience targets for build/run
├── src/
│   ├── main.cpp            # Server entry point
│   ├── server.h/cpp        # Cross-platform TCP socket server
│   ├── transcriber.h/cpp   # Whisper inference wrapper
│   └── protocol.h/cpp      # JSON message formatting
├── test_client/
│   └── test_client.cpp     # Test client (mic + WAV modes)
└── models/                 # Whisper model files (gitignored)
```

## Scope and Constraints

- **Language:** English voice to English text only (no translation)
- **Deployment:** Runs locally on a single machine (Apple Silicon or RTX 3060+ recommended)
- **Game project:** Any Unreal Engine open world demo — the chat system is game-agnostic
- **Goal:** Working prototype, not production quality

## Future Considerations

These are out of scope for the prototype but worth noting for later iterations.

- Multi-language translation using a local LLM (e.g. llama.cpp) as a second processing stage
- Multi-player support with speaker identification
- Streaming / partial transcription display (text appears word by word)
- Packaging the Whisper server as a DLL loaded directly by Unreal instead of a separate process
