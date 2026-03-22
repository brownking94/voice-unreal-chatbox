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
- Buffers and segments audio chunks from the client
- Runs Whisper inference using an English-only model (e.g. `ggml-base.en` or `ggml-small.en`)
- Returns transcribed text as JSON (e.g. `{"speaker": "Player1", "text": "anyone see that dragon?"}`)
- Runs on GPU for near-real-time performance

### Unreal Engine Client (Unreal C++ / Blueprints)

A thin integration layer added on top of an existing open world demo project. No modifications to the base game.

- Captures microphone input using Unreal's built-in audio capture (e.g. `UAudioCaptureComponent`)
- Streams raw audio chunks to the Whisper server over TCP
- Receives transcribed text back from the server
- Displays text in a UMG chat widget overlaid on the game HUD

## Scope and Constraints

- **Language:** English voice to English text only (no translation)
- **Deployment:** Runs locally on a single gaming laptop (RTX 3060+ recommended, 8GB+ VRAM)
- **Game project:** Any Unreal Engine open world demo — the chat system is game-agnostic
- **Goal:** Working prototype, not production quality

## Development Plan (1 Week)

### Days 1–2: Whisper Server Core
- Compile whisper.cpp and verify transcription works with test audio files
- Build the TCP socket server that accepts audio input and returns text
- Write a simple test client (reads audio file or captures mic) to validate the server without Unreal

### Days 3–4: Unreal Integration
- Set up the open world demo project in Unreal Editor (Visual Studio)
- Create the UMG chat widget (text box overlay on HUD)
- Build the socket client (AActor or subsystem) to connect to the Whisper server
- Wire up audio capture to stream mic input to the server

### Day 5: End-to-End Testing
- Launch both processes and test the full pipeline: speak → capture → transcribe → display
- Tune audio chunking strategy (segment on silence, 2–5 second windows)
- Handle edge cases: partial transcriptions, connection drops, empty audio

### Days 6–7: Polish and Buffer
- Optimize latency (target under 2 seconds from speech to on-screen text)
- Clean up UI (timestamps, scrolling chat history, speaker labels)
- Buffer time for build issues, debugging, and demo prep

## Dev Environment

| Component          | Tool                                  | Platform |
|--------------------|---------------------------------------|----------|
| Whisper Server     | VS Code + C/C++ extension             | macOS → Windows |
| Unreal Client      | Visual Studio Community (free)        | Windows  |
| Unreal Project     | Any open world demo / sample project  | Windows  |
| Whisper Model      | whisper.cpp with `ggml-base.en`       | Cross-platform |

## Future Considerations

These are out of scope for the prototype but worth noting for later iterations.

- Multi-language translation using a local LLM (e.g. llama.cpp) as a second processing stage
- Multi-player support with speaker identification
- Streaming / partial transcription display (text appears word by word)
- Packaging the Whisper server as a DLL loaded directly by Unreal instead of a separate process
- Push-to-talk vs. voice activity detection modes
