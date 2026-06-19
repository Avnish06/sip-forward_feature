#pragma once

#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <curl/curl.h>

// UploadManager owns a bounded pool of worker threads that perform S3 PUTs.
// Each worker holds a long-lived CURL handle, so curl's connection cache
// reuses TCP+TLS to the S3 host across uploads — saving the ~60ms handshake
// tax that the old "fresh handle per upload" pattern paid every time.
//
// No sharding: uploads have no per-call ordering requirement (mixed and
// user-only tracks are independent S3 keys). A single FIFO queue across
// workers gives full parallelism with simpler bookkeeping.
class UploadManager {
private:
    struct Job {
        std::string call_id;
        std::vector<uint8_t> wav_data;   // moved in; owned by the job
        std::string folder;
        std::shared_ptr<std::promise<bool>> result;
    };

    std::queue<Job> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_;
    std::vector<std::thread> workers_;
    size_t num_workers_;
    thread_local static CURL* curl_handle_;

    void processJobs();
    void initThreadResources();
    void cleanupThreadResources();

public:
    UploadManager();
    ~UploadManager();

    static UploadManager& instance();

    // Submit a WAV blob for upload. Returns a future that resolves to true
    // on success, false on retry exhaustion. The future is non-blocking on
    // destruction (promise-backed) — drop it if you don't care.
    std::future<bool> submit(std::string call_id,
                             std::vector<uint8_t> wav_data,
                             std::string folder);

    void stop();
};
