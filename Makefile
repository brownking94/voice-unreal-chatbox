BUILD_DIR   := build
BUILD_TYPE  := Release
MODEL_PATH  := models/ggml-medium.en.bin
PORT        := 9090
WAV_FILE    ?= test.wav

.PHONY: all build configure clean run run-server run-client run-mic model ensure-model

all: build

configure:
	cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	cmake --build $(BUILD_DIR) --config $(BUILD_TYPE) -j$$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

run: run-server

run-server: build ensure-model
	./$(BUILD_DIR)/voice-server -m $(MODEL_PATH) -p $(PORT)

ensure-model:
	@if [ ! -f $(MODEL_PATH) ]; then \
		echo "Model not found at $(MODEL_PATH), downloading..."; \
		mkdir -p models; \
		curl -L -o $(MODEL_PATH) https://huggingface.co/ggerganov/whisper.cpp/resolve/main/$$(basename $(MODEL_PATH)); \
	fi

run-client: build
	./$(BUILD_DIR)/test-client $(WAV_FILE) -p $(PORT)

run-mic: build
	./$(BUILD_DIR)/test-client --mic -p $(PORT)

model:
	@mkdir -p models
	curl -L -o $(MODEL_PATH) https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.en.bin

clean:
	rm -rf $(BUILD_DIR)
