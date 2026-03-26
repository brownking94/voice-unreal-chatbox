BUILD_DIR   := build
BUILD_TYPE  := RelWithDebInfo
MODEL_PATH  := models/ggml-small.en.bin
PORT        := 9090
WORKERS     := 2
FILTER_PATH := config/profanity.txt

# Detect OS for platform-specific paths
ifeq ($(OS),Windows_NT)
    NPROC   := $(or $(NUMBER_OF_PROCESSORS),4)
    SERVER  := $(BUILD_DIR)\$(BUILD_TYPE)\voice-server.exe
    CLIENT  := $(BUILD_DIR)\$(BUILD_TYPE)\test-client.exe
else
    NPROC   := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
    SERVER  := ./$(BUILD_DIR)/voice-server
    CLIENT  := ./$(BUILD_DIR)/test-client
endif

.PHONY: all build configure clean run run-server run-client model ensure-model update-filter

all: build

configure:
	cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build:
	@if [ ! -d $(BUILD_DIR) ]; then $(MAKE) configure; fi
	cmake --build $(BUILD_DIR) --config $(BUILD_TYPE) -j$(NPROC)

run: run-server

run-server: build ensure-model
	$(SERVER) -m $(MODEL_PATH) -p $(PORT) -w $(WORKERS) -f $(FILTER_PATH)

ensure-model:
	@if [ ! -f $(MODEL_PATH) ]; then \
		echo "Model not found at $(MODEL_PATH), downloading..."; \
		mkdir -p models; \
		curl -L -o $(MODEL_PATH) https://huggingface.co/ggerganov/whisper.cpp/resolve/main/$$(basename $(MODEL_PATH)); \
	fi

run-client: build
	$(CLIENT) -p $(PORT)

model:
	@mkdir -p models
	curl -L -o $(MODEL_PATH) https://huggingface.co/ggerganov/whisper.cpp/resolve/main/$$(basename $(MODEL_PATH))

update-filter:
	./scripts/update-profanity.sh

clean:
	rm -rf $(BUILD_DIR)
