#include "simulated_call.hpp"
#include "call_manager.hpp"
#include "websocket_handler.hpp"
#include "webhook_manager.hpp"
#include "audio_recorder.hpp"
#include <iostream>
#include <vector>
#include <regex>
#include <ctime>
#include <cmath>

std::string SimulatedCall::buildWebhookJson(const std::string& status, int duration_sec) {
    auto end_time = std::chrono::system_clock::now();
    int bill_duration = std::ceil(duration_sec / 60.0) * 60;
    double bill_rate = 0.005;
    double total_cost = bill_duration * bill_rate;

    std::time_t start_t = std::chrono::system_clock::to_time_t(start_time);
    std::time_t end_t = std::chrono::system_clock::to_time_t(end_time);
    char start_str[100], end_str[100];
    std::strftime(start_str, sizeof(start_str), "%Y-%m-%d %H:%M:%S", std::localtime(&start_t));
    std::strftime(end_str, sizeof(end_str), "%Y-%m-%d %H:%M:%S", std::localtime(&end_t));

    // Extract agent_id, call_id, env from webhook_url (same as call.cpp)
    std::string agent_id = "unknown", wh_call_id = "unknown", env = "unknown";
    std::smatch m;
    std::regex agent_re("agent_id=([^&]+)");
    if (std::regex_search(webhook_url, m, agent_re)) agent_id = m[1];
    std::regex call_re("call_id=([^&]+)");
    if (std::regex_search(webhook_url, m, call_re)) wh_call_id = m[1];
    std::regex env_re("env=([^&]+)");
    if (std::regex_search(webhook_url, m, env_re)) env = m[1];

    return "{\"CallUUID\": \"" + call_id +
           "\", \"CallStatus\": \"" + status +
           "\", \"BillDuration\": " + std::to_string(bill_duration) +
           ", \"BillRate\": " + std::to_string(bill_rate) +
           ", \"TotalCost\": " + std::to_string(total_cost) +
           ", \"Direction\": \"outbound\"" +
           ", \"Duration\": " + std::to_string(duration_sec) +
           ", \"StartTime\": \"" + std::string(start_str) +
           "\", \"EndTime\": \"" + std::string(end_str) +
           "\", \"From\": \"simulated\"" +
           ", \"To\": \"simulated\"" +
           ", \"agent_id\": \"" + agent_id +
           "\", \"call_id\": \"" + wh_call_id +
           "\", \"env\": \"" + env +
           "\", \"trunk_name\": \"" + trunk_name +
           "\", \"HangupSource\": \"" + hangup_source + "\"}";
}

SimulatedCall::SimulatedCall(const std::string& call_id, const std::string& ws_url,
                             const std::string& webhook_url, int sample_rate,
                             const std::string& trunk_name)
    : call_id(call_id), ws_url(ws_url), webhook_url(webhook_url),
      trunk_name(trunk_name), sample_rate(sample_rate),
      start_time(std::chrono::system_clock::now()) {}

SimulatedCall::~SimulatedCall() {
    terminate();
}

bool SimulatedCall::start() {
    handler = new WebSocketAudioHandler(sample_rate, ws_url);
    handler->setDisconnectCallback([this]() {
        std::cout << "[SimulatedCall] WebSocket disconnected for call: " << call_id << std::endl;
        running = false;
    });

    if (!handler->connect(ws_url)) {
        std::cerr << "[SimulatedCall] Failed to connect WebSocket for call: " << call_id << std::endl;
        delete handler;
        handler = nullptr;
        // Webhook: fail
        WebhookManager::instance().send(call_id, webhook_url, buildWebhookJson("fail", 0));
        return false;
    }

    std::cout << "[SimulatedCall] WebSocket connected, starting ringing for call: " << call_id << std::endl;
    handler->startRinging();

    // Record both legs to a local wav: caller audio (the ringing we send) as the
    // user stream, and whatever the far end plays back (e.g. the human after a
    // rupture transfer) as the agent stream.
    recorder = std::make_unique<AudioRecorder>(call_id, sample_rate);
    recorder->startRecording();

    running = true;
    audio_pump_thread = std::thread(&SimulatedCall::audioPumpLoop, this);

    // Webhook: ringing
    WebhookManager::instance().send(call_id, webhook_url, buildWebhookJson("ringing", 0));
    return true;
}

void SimulatedCall::terminate() {
    if (terminated.exchange(true)) return;

    running = false;
    if (audio_pump_thread.joinable()) {
        audio_pump_thread.join();
    }

    if (handler) {
        handler->disconnect();
        delete handler;
        handler = nullptr;
    }

    // Webhook: completed
    auto duration_sec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now() - start_time).count();
    WebhookManager::instance().send(call_id, webhook_url, buildWebhookJson("completed", static_cast<int>(duration_sec)));

    CallManager::instance().decrementCallCounter();
    std::cout << "[SimulatedCall] Terminated call: " << call_id << std::endl;
}

bool SimulatedCall::isRunning() const {
    return running.load() && !terminated.load();
}

void SimulatedCall::audioPumpLoop() {
    // 20ms of 16-bit mono PCM
    size_t frame_size = (sample_rate / 1000) * 20 * 2;
    auto next_tick = std::chrono::steady_clock::now();
    auto start = std::chrono::steady_clock::now();

    while (running) {
        // Auto-disconnect after 60 seconds
        if (std::chrono::steady_clock::now() - start >= std::chrono::seconds(60)) {
            std::cout << "[SimulatedCall] 60s timeout reached, disconnecting call: " << call_id << std::endl;
            hangup_source = "timeout";
            running = false;
            break;
        }
        next_tick += std::chrono::milliseconds(20);

        // Build a pj::MediaFrame from raw PCM bytes and hand it to the recorder.
        auto record = [this](const std::vector<uint8_t>& data, bool is_agent) {
            if (!recorder || data.empty()) return;
            pj::MediaFrame mf;
            mf.type = PJMEDIA_FRAME_TYPE_AUDIO;
            mf.buf.assign(data.begin(), data.end());
            mf.size = data.size();
            recorder->writeFrame(mf, is_agent);
        };

        if (handler && handler->isRingingActive()) {
            std::vector<uint8_t> ringing_data;
            handler->getRingingFrame(ringing_data, frame_size);
            handler->sendAudioFrame(ringing_data.data(), ringing_data.size());
            record(ringing_data, /*is_agent=*/false);  // caller leg
        }

        // Pull incoming audio (e.g. the human's voice after transfer) and record it.
        std::vector<uint8_t> rx;
        if (handler) {
            while (handler->getAudioFrame(rx)) {
                record(rx, /*is_agent=*/true);  // far-end / human leg
            }
        }

        std::this_thread::sleep_until(next_tick);
    }

    if (recorder) {
        recorder->stopRecording();  // flush + write local wav (idempotent)
    }

    // Audio pump finished — clean up directly on this thread
    if (!terminated.exchange(true)) {
        if (handler) {
            handler->disconnect();
            delete handler;
            handler = nullptr;
        }

        auto duration_sec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now() - start_time).count();
        WebhookManager::instance().send(call_id, webhook_url, buildWebhookJson("completed", static_cast<int>(duration_sec)));

        CallManager::instance().decrementCallCounter();
        std::cout << "[SimulatedCall] Call finished: " << call_id << std::endl;
    }
}
