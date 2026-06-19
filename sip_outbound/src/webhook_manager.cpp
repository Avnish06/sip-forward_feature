#include "webhook_manager.hpp"
#include <cstdlib>
#include <curl/curl.h>
#include <functional>
#include <iostream>

thread_local CURL* WebhookManager::curl_handle_ = nullptr;
thread_local struct curl_slist* WebhookManager::headers_ = nullptr;

WebhookManager::WebhookManager() : running_(true) {
    const char* env_workers = std::getenv("WEBHOOK_THREADS");
    num_workers_ = env_workers ? std::atoi(env_workers) : 4;
    if (num_workers_ < 1) num_workers_ = 1;
    if (num_workers_ > 16) num_workers_ = 16;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Build all shards before launching any worker so every worker can
    // safely reference its shard by index.
    shards_.reserve(num_workers_);
    for (size_t i = 0; i < num_workers_; ++i) {
        shards_.emplace_back(std::make_unique<Shard>());
    }

    workers_.reserve(num_workers_);
    for (size_t i = 0; i < num_workers_; ++i) {
        workers_.emplace_back(&WebhookManager::processRequests, this, i);
    }
    std::cout << "WebhookManager started with " << num_workers_
              << " workers (sharded by call_id)" << std::endl;
}

WebhookManager::~WebhookManager() {
    stop();
}

WebhookManager& WebhookManager::instance() {
    static WebhookManager inst;
    return inst;
}

void WebhookManager::send(const std::string& shard_key,
                          const std::string& url,
                          const std::string& json) {
    size_t idx = std::hash<std::string>{}(shard_key) % num_workers_;
    Shard& shard = *shards_[idx];
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.queue.push({url, json});
    }
    shard.cv.notify_one();
}

void WebhookManager::stop() {
    running_ = false;
    for (auto& shard : shards_) {
        shard->cv.notify_all();
    }
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    curl_global_cleanup();
}

void WebhookManager::processRequests(size_t shard_idx) {
    initThreadResources();

    Shard& shard = *shards_[shard_idx];

    while (running_) {
        WebhookData data;

        {
            std::unique_lock<std::mutex> lock(shard.mutex);
            shard.cv.wait(lock, [&] { return !shard.queue.empty() || !running_; });

            if (!running_ && shard.queue.empty()) break;

            if (!shard.queue.empty()) {
                data = shard.queue.front();
                shard.queue.pop();
            }
        }

        if (!data.url.empty()) {
            bool success = false;
            int attempts = 0;
            const int max_retries = 3;

            while (!success && attempts < max_retries && running_) {
                attempts++;

                curl_easy_reset(curl_handle_);

                curl_easy_setopt(curl_handle_, CURLOPT_URL, data.url.c_str());
                curl_easy_setopt(curl_handle_, CURLOPT_POSTFIELDS, data.json.c_str());
                curl_easy_setopt(curl_handle_, CURLOPT_HTTPHEADER, headers_);
                curl_easy_setopt(curl_handle_, CURLOPT_TIMEOUT, 10L);
                curl_easy_setopt(curl_handle_, CURLOPT_WRITEFUNCTION, +[](void*, size_t size, size_t nmemb, void*) -> size_t {
                    return size * nmemb;
                });

                CURLcode res = curl_easy_perform(curl_handle_);
                long response_code = 0;
                curl_easy_getinfo(curl_handle_, CURLINFO_RESPONSE_CODE, &response_code);

                success = (res == CURLE_OK && response_code == 200);

                if (!success && attempts < max_retries && running_) {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    std::cerr << "Webhook request failed, retrying... (" << attempts << "/" << max_retries << ")" << std::endl;
                } else if (!success) {
                    std::cerr << "Webhook request failed after " << attempts << " attempts: " << curl_easy_strerror(res) << std::endl;
                } else {
                    std::cout << "Webhook request succeeded: " << data.url << std::endl;
                }
            }
        }
    }

    cleanupThreadResources();
}

void WebhookManager::initThreadResources() {
    curl_handle_ = curl_easy_init();
    headers_ = curl_slist_append(nullptr, "Content-Type: application/json");
}

void WebhookManager::cleanupThreadResources() {
    if (curl_handle_) {
        curl_easy_cleanup(curl_handle_);
        curl_handle_ = nullptr;
    }
    if (headers_) {
        curl_slist_free_all(headers_);
        headers_ = nullptr;
    }
}
