#include "transcriber_pool.h"

#include <iostream>

TranscriberPool::TranscriberPool(const std::string& model_path, int pool_size) {
    std::cout << "[pool] Loading " << pool_size << " model instances..." << std::endl;
    for (int i = 0; i < pool_size; i++) {
        auto t = std::make_unique<Transcriber>(model_path);
        available_.push(t.get());
        all_.push_back(std::move(t));
        std::cout << "[pool] Instance " << (i + 1) << "/" << pool_size << " ready" << std::endl;
    }
    std::cout << "[pool] All instances loaded" << std::endl;
}

std::string TranscriberPool::transcribe(const std::vector<uint8_t>& pcm16_bytes, const std::string& language) {
    Transcriber* t = nullptr;

    // Borrow an available instance (blocks if all are busy)
    {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return !available_.empty(); });
        t = available_.front();
        available_.pop();
    }

    // Transcribe outside the lock — multiple threads can run in parallel
    std::string result = t->transcribe(pcm16_bytes, language);

    // Return instance to the pool
    {
        std::lock_guard<std::mutex> lock(mtx_);
        available_.push(t);
    }
    cv_.notify_one();

    return result;
}
