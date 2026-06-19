#pragma once

#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <curl/curl.h>

struct WebhookData {
    std::string url;
    std::string json;
};

class WebhookManager {
private:
    // One independent queue per worker. Events for the same call_id
    // always hash to the same shard, so a single worker drains them in
    // FIFO order. Each worker also serializes its POSTs (waits for the
    // response before pulling the next item), so events leave sip_outbound
    // in the order they were enqueued for that call. webhook_worker
    // shards on the same key to preserve order end-to-end.
    struct Shard {
        std::queue<WebhookData> queue;
        std::mutex mutex;
        std::condition_variable cv;
    };

    std::vector<std::unique_ptr<Shard>> shards_;
    std::atomic<bool> running_;
    std::vector<std::thread> workers_;
    size_t num_workers_;
    thread_local static CURL* curl_handle_;
    thread_local static struct curl_slist* headers_;

    void processRequests(size_t shard_idx);
    void initThreadResources();
    void cleanupThreadResources();

public:
    WebhookManager();
    ~WebhookManager();

    static WebhookManager& instance();
    // shard_key must be a stable per-call identifier (e.g. SIP call UUID)
    // so all events for one call are processed by the same worker in order.
    void send(const std::string& shard_key, const std::string& url, const std::string& json);
    void stop();
};
