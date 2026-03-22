cmake_minimum_required(VERSION 3.20)
project(unreal-voice-server LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── whisper.cpp via FetchContent ─────────────────────────────────────────────
include(FetchContent)

set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    whisper
    GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
    GIT_TAG        v1.7.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(whisper)

# ── miniaudio (single-header audio capture library) ──────────────────────────
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        0.11.21
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(miniaudio)

# ── Voice server executable ──────────────────────────────────────────────────
add_executable(voice-server
    src/main.cpp
    src/server.cpp
    src/transcriber.cpp
    src/protocol.cpp
)

target_include_directories(voice-server PRIVATE src)
target_link_libraries(voice-server PRIVATE whisper)

if(WIN32)
    target_link_libraries(voice-server PRIVATE ws2_32)
endif()

if(APPLE)
    find_library(FOUNDATION_LIBRARY Foundation)
    find_library(ACCELERATE_LIBRARY Accelerate)
    if(FOUNDATION_LIBRARY)
        target_link_libraries(voice-server PRIVATE ${FOUNDATION_LIBRARY})
    endif()
    if(ACCELERATE_LIBRARY)
        target_link_libraries(voice-server PRIVATE ${ACCELERATE_LIBRARY})
    endif()
endif()

# ── Test client executable ───────────────────────────────────────────────────
add_executable(test-client
    test_client/test_client.cpp
)

target_include_directories(test-client PRIVATE src ${miniaudio_SOURCE_DIR})

if(WIN32)
    target_link_libraries(test-client PRIVATE ws2_32)
endif()

if(APPLE)
    find_library(COREAUDIO_LIBRARY CoreAudio)
    find_library(AUDIOTOOLBOX_LIBRARY AudioToolbox)
    if(COREAUDIO_LIBRARY)
        target_link_libraries(test-client PRIVATE ${COREAUDIO_LIBRARY})
    endif()
    if(AUDIOTOOLBOX_LIBRARY)
        target_link_libraries(test-client PRIVATE ${AUDIOTOOLBOX_LIBRARY})
    endif()
endif()
