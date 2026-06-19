#include "../include/upload_manager.hpp"
#include "../include/audio_recorder.hpp"  // for R2Uploader::uploadWithHandle
#include <cstdlib>
#include <exception>
#include <iostream>

thread_local CURL* UploadManager::curl_handle_ = nullptr;

UploadManager::UploadManager() : running_(true) {
    const char* env_workers = std::getenv("UPLOAD_THREADS");
    num_workers_ = env_workers ? std::atoi(env_workers) : 4;
    if (num_workers_ < 1) num_workers_ = 1;
    if (num_workers_ > 16) num_workers_ = 16;

    workers_.reserve(num_workers_);
    for (size_t i = 0; i < num_workers_; ++i) {
        workers_.emplace_back(&UploadManager::processJobs, this);
    }
    std::cout << "UploadManager started with " << num_workers_ << " workers" << std::endl;
}

UploadManager::~UploadManager() {
    stop();
}

UploadManager& UploadManager::instance() {
    static UploadManager inst;
    return inst;
}

std::future<bool> UploadManager::submit(std::string call_id,
                                        std::vector<uint8_t> wav_data,
                                        std::string folder) {
    auto promise = std::make_shared<std::promise<bool>>();
    auto fut = promise->get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push({std::move(call_id), std::move(wav_data), std::move(folder), promise});
    }
    cv_.notify_one();
    return fut;
}

void UploadManager::stop() {
    running_ = false;
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();
}

void UploadManager::processJobs() {
    initThreadResources();

    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !queue_.empty() || !running_; });

            // Drain remaining jobs on shutdown rather than abandoning recordings.
            if (queue_.empty()) {
                if (!running_) break;
                continue;
            }

            job = std::move(queue_.front());
            queue_.pop();
        }

        bool ok = false;
        try {
            ok = R2Uploader::getInstance().uploadWithHandle(
                curl_handle_, job.call_id, job.wav_data, job.folder, 3);
        } catch (const std::exception& e) {
            std::cerr << "[UploadManager] upload threw for call " << job.call_id
                      << ": " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[UploadManager] upload threw unknown exception for call "
                      << job.call_id << std::endl;
        }

        if (job.result) {
            try {
                job.result->set_value(ok);
            } catch (const std::future_error&) {
                // promise already satisfied/broken — nothing to do
            }
        }
    }

    cleanupThreadResources();
}

void UploadManager::initThreadResources() {
    curl_handle_ = curl_easy_init();
}

void UploadManager::cleanupThreadResources() {
    if (curl_handle_) {
        curl_easy_cleanup(curl_handle_);
        curl_handle_ = nullptr;
    }
}
