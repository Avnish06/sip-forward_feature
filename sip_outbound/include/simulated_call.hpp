#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>

class WebSocketAudioHandler;  // Forward declaration
class AudioRecorder;          // Forward declaration

class SimulatedCall {
public:
    std::string call_id;
    std::string ws_url;
    std::string webhook_url;
    std::string trunk_name;
    int sample_rate;
    std::chrono::system_clock::time_point start_time;

    SimulatedCall(const std::string& call_id, const std::string& ws_url,
                  const std::string& webhook_url, int sample_rate,
                  const std::string& trunk_name);
    ~SimulatedCall();

    bool start();
    void terminate();
    bool isRunning() const;

private:
    WebSocketAudioHandler* handler = nullptr;
    std::unique_ptr<AudioRecorder> recorder;  // captures caller + human audio to a local wav
    std::thread audio_pump_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> terminated{false};
    std::string hangup_source = "user";

    void audioPumpLoop();
    std::string buildWebhookJson(const std::string& status, int duration_sec);
};
