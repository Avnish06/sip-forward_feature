#pragma once
#include <pjsua2.hpp>
#include "audio_port.hpp"
#include <ctime>
#include <chrono>
#include <regex> // Include regex

class MyCall : public pj::Call {
    friend class CallManager;  // Add this to allow CallManager to access endpoint_key
    friend class MyAccount;    // Add this to allow MyAccount to access private members

private:
    MyAudioMediaPort* med_port;
    int sample_rate;  // Sample rate in Hz
    bool media_connected;
    bool call_answered;
    std::string ws_url;
    std::string endpoint_key;  // Add this for endpoint tracking
    std::string call_recording_id;
    std::string webhook_url;
    std::string string_call_id;
    std::string agent_id;  // Add agent_id
    std::string env;  // Add env
    int call_id;  
    std::string webhook_call_id;
    std::string trunk_name;  // Add trunk_name member
    std::string hangup_source;  // Track who initiated the hangup
    std::chrono::system_clock::time_point start_time;
    std::string current_remote_addr;
    void bindMedia(pj::AudioMedia &aud_med);
    bool should_cleanup = false;  // Flag to indicate if cleanup is needed (default false)
    bool directCall;

    // --- Human transfer (phone-number, bridged via Asterisk) ---
    // A "bridge leg" is the outbound call we place to the human's phone. When it
    // answers we splice the caller's audio to it in PJSIP's conference bridge and
    // drop the AI WebSocket. The original caller leg keeps a pointer to its bridge
    // leg (bridged_leg) and vice-versa (bridge_peer) so a hangup on either side
    // tears down the other.
    bool is_bridge_leg = false;     // true on the leg dialed to the human
    bool bridge_done = false;       // guard so we splice audio only once
    MyCall* bridge_peer = nullptr;  // bridge leg -> original caller
    MyCall* bridged_leg = nullptr;  // original caller -> bridge leg

    int  getActiveAudioMediaIndex();  // index of first ACTIVE audio media, -1 if none
    void detachWsMediaForBridge();    // unlink caller <-> WS media port (stops ringback)
    void bridgeWithPeer();            // splice caller <-> human audio

public:
    MyCall(pj::Account& acc, const std::string& websocket_url, const std::string& string_call_id, int call_id = PJSUA_INVALID_ID, const std::string& webhook_url = "", const std::string& agent_id = "", const std::string& env = "", const std::string webhook_call_id = "", const std::string& trunk_name = "", std::chrono::system_clock::time_point start_time = std::chrono::system_clock::now(), int sample_rate = 8000, bool directCall = false);
    void startRecording();
    void stopRecording();
    ~MyCall();
    virtual void onCallState(pj::OnCallStateParam& /*prm*/);
    void setupMedia();
    virtual void onCallMediaState(pj::OnCallMediaStateParam& /*prm*/);
    virtual void onDtmfDigit(pj::OnDtmfDigitParam &prm) override;
    std::string getHangupSource() const { return hangup_source; }  // Add public getter for hangup_source
    void setHangupSource(const std::string& source) { hangup_source = source; }  // Add public setter for hangup_source

    // Configure this call as the bridge leg dialed to a human; `peer` is the original caller.
    void setupAsBridgeLeg(MyCall* peer) { is_bridge_leg = true; bridge_peer = peer; }
};
