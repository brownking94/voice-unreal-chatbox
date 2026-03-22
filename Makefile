BUILD_DIR   := build
BUILD_TYPE  := Release
MODEL_PATH  := models/ggml-base.en.bin
PORT        := 9090
WAV_FILE    ?= test.wav

.PHONY: all build configure clean run run-server run-client model

all: build

configure:
	cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	cmake --build $(BUILD_DIR) --config $(BUILD_TYPE) -j$$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

run: run-server

run-server: build
	./$(BUILD_DIR)/voice-server -m $(MODEL_PATH) -p $(PORT)

run-client: build
	./$(BUILD_DIR)/test-client $(WAV_FILE) -p $(PORT)

model:
	@mkdir -p models
	curl -L -o $(MODEL_PATH) https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin

clean:
	rm -rf $(BUILD_DIR)
