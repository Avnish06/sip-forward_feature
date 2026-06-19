#pragma once

#include <pjsua2.hpp>
#include <pjsip.h>
#include <pjsip/sip_endpoint.h>
#include <pjsip/sip_transaction.h>
#include "websocket_handler.hpp"
#include "audio_recorder.hpp"
#include <memory>
#include <functional>

class MyCall;  // Forward declaration

class MyAudioMediaPort : public pj::AudioMediaPort {
private:
    WebSocketAudioHandler* handler;
    std::string ws_uri;
    bool initialized;
    std::shared_ptr<AudioRecorder> recorder;
    int sample_rate;  // Sample rate in Hz
    MyCall* call;
    bool directCall;

    std::string current_rtp_endpoint;
    pj::AudioMedia* current_audio_media = nullptr;
    
    enum {
        MAX_RETRIES = 3,
        RETRY_DELAY_MS = 5000
    };

public:
    MyAudioMediaPort(const std::string& uri, const std::string& call_id, int sample_rate = 8000, bool directCall = false);
    ~MyAudioMediaPort();

    void cleanup();
    bool initializeWebSocket();
    virtual void onFrameRequested(pj::MediaFrame& frame);
    virtual void onFrameReceived(pj::MediaFrame& frame);
    bool isInitialized() const;
    void startRecording();
    void stopRecording();
    void discardRecordingSoFar();  // drop audio buffered before a human transfer (keep post-transfer only)
    void setCall(MyCall* call_ptr);
    void onWebSocketDisconnected();
    void sendDtmf(char digit);
    void receiveDtmfFromWebsocket(const std::string& text);
    void startHumanNumberTransfer(const std::string& number);  // SIP REFER the human to another number via Asterisk
    void setHangupSource(const std::string& source);  // Add method to set hangup source
    std::string getHangupSource();  // Add method to get hangup source

    void handleRtpEndpointChange(const std::string& new_endpoint);
    void reconnectAudioMedia();
};
