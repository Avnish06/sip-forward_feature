#pragma once

#include "human_ringing.hpp"
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <nlohmann/json.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <pjsua2.hpp>

using json = nlohmann::json;
using DisconnectCallback = std::function<void()>;
using websocketpp::connection_hdl;
typedef websocketpp::client<websocketpp::config::asio_tls_client> tls_client;
using AgentTransferCallback = std::function<void(const std::string& agentId, const std::string& agentName)>;

class MyAudioMediaPort;  // Forward declaration

class WebSocketAudioHandler {
private:
    tls_client ws_client;
    websocketpp::connection_hdl conn_hdl;
    std::string ws_uri;
    std::atomic<bool> connected;
    int sample_rate;  // Sample rate in Hz
    size_t bytes_per_frame;  // sample_rate * 20ms * 2 bytes/sample (320 @ 8k, 640 @ 16k)
    std::queue<std::vector<uint8_t>> audio_queue;
    std::vector<uint8_t> partial_frame;  // carry-over for non-aligned WS chunks, under queue_mutex
    std::mutex queue_mutex;
    bool REAL_TIME_AUDIO = false;
    static const size_t MAX_QUEUE_SIZE_RUPTURE = 15;
    static const size_t MAX_QUEUE_SIZE_DEFAULT = 3000;
    std::thread ws_thread;
    uint64_t sequence_number;
    std::string stream_id;
    bool checkpoint_received;
    bool clear_audio_called;
    bool hangup_received;
    std::atomic<bool> intentional_close;
    static const int MAX_RECONNECT_ATTEMPTS = 3;
    std::atomic<int> reconnect_attempts;
    bool log_misalignment_done;
    std::string pending_stream_name;
    std::string pending_stream_id;
    DisconnectCallback disconnect_callback;
    pj_thread_desc thread_desc;
    pj_thread_t* thread_handle;
    bool thread_registered;
    MyAudioMediaPort* med_port;  // Add pointer to MyAudioMediaPort

    // Agent Transfer
    std::string current_agent_id;
    bool agent_transfer_in_progress;
    AgentTransferCallback agent_transfer_callback;
    void setAgentId();

    // Human Trabsfer Ringing
    std::unique_ptr<RingingAudioProvider> ringing_provider;
    bool ringing_active;
    void stopRinging();

    // Audio resampling method
    std::vector<uint8_t> resampleAudio(const std::vector<uint8_t>& input_data, 
                                      int input_rate, int output_rate);

    void clearAudioQueue();
    void enqueueAudio(std::vector<uint8_t>&& pcm);
    void registerThread();
    void attemptReconnect();
    void resetCheckpointState();
    void sendPlayedStreamEvent();
    std::string base64_encode(const uint8_t* data, size_t size);
    std::vector<uint8_t> base64_decode(const std::string& base64);
    std::string formatTimestamp(long long milliseconds);
    static int16_t mulaw2linear(uint8_t u_val);
    std::vector<uint8_t> convertFromMulaw(const std::vector<uint8_t>& mulaw_data);

public:
    WebSocketAudioHandler(int sample_rate = 8000, const std::string& ws_uri = "");
    ~WebSocketAudioHandler();

    void sendStartEvent();
    bool connect(const std::string& uri);
    void disconnect();
    void sendAudioFrame(const void* data, size_t size);
    void handleReceivedMessage(const std::string& message);
    bool getAudioFrame(std::vector<uint8_t>& frame);
    bool isConnected() const;
    void setDisconnectCallback(DisconnectCallback callback);
    void sendHangupEvent();
    void sendDtmfEvent(char digit);
    void setMediaPort(MyAudioMediaPort* port);

    std::string getCurrentAgentId() const { return current_agent_id; }
    bool isAgentTransferInProgress() const { return agent_transfer_in_progress; }
    void performAgentTransfer(const std::string& new_agent_id);
    void performHumanTransfer(const std::string& human_id);
    void performHumanNumberTransfer(const std::string& number);  // SIP REFER the human to another number via Asterisk

    void adjustVolume(std::vector<uint8_t>& audio_data, float volume);

    bool isRingingActive() const;
    void getRingingFrame(std::vector<uint8_t>& frame, size_t frame_size);
    void startRinging();
};
