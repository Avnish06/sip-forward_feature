#include "../include/call.hpp"
#include "../include/call_manager.hpp"
#include "../include/webhook_manager.hpp"
#include <iostream>
#include <sstream>
#include <ctime>
#include <curl/curl.h>
#include <chrono>
#include <cmath> // Include cmath for std::ceil
#include <regex> // Include regex for extracting mobile numbers

MyCall::MyCall(pj::Account& acc, const std::string& websocket_url, const std::string& string_call_id, int call_id, const std::string& webhook_url, const std::string& agent_id, const std::string& env, const std::string webhook_call_id, const std::string& trunk_name, std::chrono::system_clock::time_point start_time, int sample_rate, bool directCall)
    : Call(acc, call_id), med_port(nullptr), media_connected(false), call_answered(false), ws_url(websocket_url), webhook_url(webhook_url), string_call_id(string_call_id), agent_id(agent_id), env(env), call_id(call_id), webhook_call_id(webhook_call_id), trunk_name(trunk_name), hangup_source("none"), start_time(start_time), sample_rate(sample_rate), directCall(directCall) {
        // Use the UUID directly as the recording ID
        call_recording_id = string_call_id;
        
        std::cout << "Creating call with WebSocket URL: " << ws_url << std::endl;

        start_time = std::chrono::system_clock::now();
}

MyCall::~MyCall() {
    std::cout << "[Call Destructor] MyCall destructor starting for call: " << string_call_id << std::endl;
    
    // Stop recording first to ensure R2 upload starts
    if (med_port) {
        try {
            // Stop all media transmission immediately - must be done safely
            try {
                pj::CallInfo ci = getInfo();
                for (unsigned i = 0; i < ci.media.size(); ++i) {
                    if (ci.media[i].type == PJMEDIA_TYPE_AUDIO) {
                        try {
                            pj::AudioMedia aud_med = getAudioMedia(i);
                            aud_med.stopTransmit(*med_port);
                            med_port->stopTransmit(aud_med);
                        } catch (...) {}
                    }
                }
            } catch (...) {
                // Call may already be terminated
                std::cerr << "[Call Destructor] Error occurred while stopping media transmission: Call may already be terminated" << std::endl;
            }
            
            med_port->stopRecording();
            
            // Critical: Wait longer for media threads to fully stop
            std::this_thread::sleep_for(std::chrono::milliseconds(3000));
            
            med_port->cleanup();
            
            // Additional wait after cleanup before deletion
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        } catch (...) {
            std::cerr << "[Call Destructor] Error occurred while stopping media transmission in call destructor" << std::endl;
        }

        try {
            delete med_port;
            med_port = nullptr;
            std::cout << "[Call Destructor] Media port deleted successfully in call destructor" << std::endl;
        } catch(const std::exception& e) {
            std::cerr << "[Call Destructor] Error occurred while deleting media port: " << e.what() << '\n';
            med_port = nullptr;
        }
    }

    std::cout << "[Call Destructor] MyCall destructor completed for call: " << string_call_id << std::endl;
}

void MyCall::onCallState(pj::OnCallStateParam& /*prm*/) {
    pj::CallInfo ci = getInfo();
    std::cout << "Call state: " << ci.stateText << std::endl;

    // Bridge leg (the call we placed to the human's phone) has a much simpler
    // lifecycle than a normal AI call: no WebSocket, no AI CDR webhook. The audio
    // splice happens in onCallMediaState; here we only react to it going down so
    // we can tear down the original caller leg with it.
    if (is_bridge_leg) {
        if (ci.state == PJSIP_INV_STATE_CONFIRMED) {
            std::cout << "[HumanTransfer] Human answered, waiting for media to bridge..." << std::endl;
        } else if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
            std::cout << "[HumanTransfer] Human leg disconnected (code " << ci.lastStatusCode
                      << "). Tearing down caller leg." << std::endl;
            if (bridge_peer) {
                try {
                    if (bridge_peer->med_port) bridge_peer->med_port->setHangupSource("human");
                    pj::CallOpParam op;
                    bridge_peer->hangup(op);
                } catch (...) {}
                bridge_peer->bridged_leg = nullptr;  // we're going away; don't let caller double-hang us
            }
            CallManager::instance().decrementCallCounter();
        }
        return;  // skip the AI/CDR path below
    }

    // Log the call status
    std::string status;
    switch (ci.state) {
        case PJSIP_INV_STATE_NULL:
            status = "initiated";
            break;
        case PJSIP_INV_STATE_CALLING:
            status = "ringing";
            break;
        case PJSIP_INV_STATE_INCOMING:
            status = "incoming";
            break;
        case PJSIP_INV_STATE_EARLY:
            status = "early";
            break;
        case PJSIP_INV_STATE_CONNECTING:
            status = "connecting";
            break;
        case PJSIP_INV_STATE_CONFIRMED:
            status = "in-progress";
            call_answered = true;
            CallManager::instance().incrementAnsweredCallsCount();
            break;
        case PJSIP_INV_STATE_DISCONNECTED:
            switch (ci.lastStatusCode) {
                case PJSIP_SC_OK:
                    status = "completed";
                    break;
                case PJSIP_SC_REQUEST_TERMINATED:  // 487 - WebSocket disconnect during call
                    if (ci.connectDuration.sec > 0) {
                        status = "completed";  // Call had some duration, consider it completed
                    } else {
                        status = "fail";  // Call never really started
                    }
                    break;
                case PJSIP_SC_TEMPORARILY_UNAVAILABLE:
                    if (ci.totalDuration.sec < 34) {
                        status = "busy";
                    } else {
                        status = "no-answer";
                    }
                    break;
                case PJSIP_SC_FORBIDDEN:
                    if (ci.totalDuration.sec <= 20){
                        status = "fail";                // ELISION SPECIFIC: ACTUAL FAIL
                    } else {
                        // status = "unavailable";
                        // status = "fail";
                        status = "no-answer";
                    }
                    break;
                case PJSIP_SC_GONE:
                    if (ci.connectDuration.sec > 0) {
                        status = "completed";
                    } else {
                        status = "busy";
                    }
                    break;
                case PJSIP_SC_DECLINE:
                    status = "busy";
                    break;
                case PJSIP_SC_BUSY_HERE:
                    status = "busy";
                    break;
                case PJSIP_SC_SERVICE_UNAVAILABLE:
                    status = "busy";
                case PJSIP_SC_NOT_FOUND:
                    if (ci.totalDuration.sec >= 5){ // Instant fail with 404 likely means endpint not found, or the prefix is wrong
                        status = "invalid-number";
                    } else {
                        status = "fail";
                    }
                    // status = "fail";
                    break;
                case PJSIP_SC_REQUEST_TIMEOUT:
                    status = "fail";
                    break;
                case PJSIP_SC_INTERNAL_SERVER_ERROR:
                    status = "fail";
                    break;
                default:
                    status = "unknown";
                    break;
            }
            break;
        default:
            status = "unknown";
            break;
    }

    
    std::cout <<"log call Status: " << status << std::endl;
    std::cout <<"log call lastStatusCode: " << ci.lastStatusCode << std::endl;
    std::cout <<"log call lastReason: " << ci.lastReason << std::endl;
    std::cout <<"call Total duration: " << ci.totalDuration.sec << std::endl;

    // Handle state-specific actions
    if (ci.state == PJSIP_INV_STATE_CONFIRMED) {
        call_answered = true;
        std::cout << "Call answered, setting up media immediately..." << std::endl;
        setupMedia();
        startRecording();
    } else if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
        std::cout << "Call disconnected, performing cleanup..." << std::endl;
        should_cleanup = true;  // Mark for cleanup from active_calls in call manager at end of this method

        // If this caller was bridged to a human leg, hang that leg up too.
        if (bridged_leg) {
            std::cout << "[HumanTransfer] Caller leg down, hanging up bridged human leg." << std::endl;
            try {
                bridged_leg->bridge_peer = nullptr;  // prevent the human leg from re-hanging us
                pj::CallOpParam op;
                bridged_leg->hangup(op);
            } catch (...) {}
            bridged_leg = nullptr;
        }
        
        // Set hangup source if not already set
        if (hangup_source == "none") {
            std::cout << "Setting hangup source to: user" << std::endl;
            hangup_source = "user";
        }
        
        // Stop recording first to trigger R2 upload
        stopRecording();
        
        // Delay media port cleanup
        if (med_port) {
            auto* med_port_ptr = med_port;  // Capture the pointer value
            std::thread cleanup_thread([med_port_ptr]() {
                // std::this_thread::sleep_for(std::chrono::milliseconds(3000));
                // med_port_ptr is guaranteed to be valid here
                med_port_ptr->cleanup();
                delete med_port_ptr;
            });
            cleanup_thread.detach();
            // Set to nullptr to prevent double delete in destructor
            med_port = nullptr;
        }

        // Finally decrement call counter
        CallManager::instance().decrementCallCounter();
    }

    // Calculate call duration and billing duration
    auto end_time = std::chrono::system_clock::now();
    std::chrono::duration<double> duration = std::chrono::seconds(ci.connectDuration.sec);
    int bill_duration = std::ceil(static_cast<int>(duration.count()) / 60.0) * 60; // Assuming billing duration is in seconds
    double bill_rate = 0.005; // Example billing rate per second
    double total_cost = bill_duration * bill_rate; // Calculate total cost

    // Format start and end times
    std::time_t start_time_t = std::chrono::system_clock::to_time_t(start_time);
    std::time_t end_time_t = std::chrono::system_clock::to_time_t(end_time);
    char start_time_str[100];
    char end_time_str[100];
    std::strftime(start_time_str, sizeof(start_time_str), "%Y-%m-%d %H:%M:%S", std::localtime(&start_time_t));
    std::strftime(end_time_str, sizeof(end_time_str), "%Y-%m-%d %H:%M:%S", std::localtime(&end_time_t));

    // Extract mobile numbers from URIs
    std::regex phone_regex("\\+?[1-9]\\d{1,14}");
    std::smatch from_match, to_match;
    std::string from_number = ci.remoteUri;
    std::string to_number = ci.localUri;
    if (std::regex_search(ci.remoteUri, from_match, phone_regex)) {
        from_number = from_match.str(0);
    }
    if (std::regex_search(ci.localUri, to_match, phone_regex)) {
        to_number = to_match.str(0);
    }
    // Extract agent_id, call_id, and env from Webhook URL
    std::regex agent_regex("agent_id=([^&]+)");
    std::smatch agent_match;
    if (std::regex_search(webhook_url, agent_match, agent_regex)) {
        this->agent_id = agent_match[1];
    } else {
        this->agent_id = "unknown";
    }

    std::regex call_id_regex("call_id=([^&]+)");
    std::smatch call_id_match;
    if (std::regex_search(webhook_url, call_id_match, call_id_regex)) {
        this->webhook_call_id = call_id_match[1];
    } else {
        this->webhook_call_id = "unknown";
    }

    std::regex env_regex("env=([^&]+)");
    std::smatch env_match;
    if (std::regex_search(webhook_url, env_match, env_regex)) {
        this->env = env_match[1];
    } else {
        this->env = "unknown";
    }

    if (status != "early") {
        std::string json_data = "{\"CallUUID\": \"" + string_call_id + 
                                "\", \"CallStatus\": \"" + status + 
                                "\", \"BillDuration\": " + std::to_string(bill_duration) + 
                                ", \"BillRate\": " + std::to_string(bill_rate) + 
                                ", \"TotalCost\": " + std::to_string(total_cost) + 
                                ", \"Direction\": \"outbound\"" +
                                ", \"Duration\": " + std::to_string(static_cast<int>(duration.count())) + 
                                ", \"StartTime\": \"" + std::string(start_time_str) + 
                                "\", \"EndTime\": \"" + std::string(end_time_str) + 
                                "\", \"From\": \"" + from_number + 
                                "\", \"To\": \"" + to_number + 
                                "\", \"agent_id\": \"" + agent_id + 
                                "\", \"call_id\": \"" + webhook_call_id + 
                                "\", \"env\": \"" + env + 
                                "\", \"trunk_name\": \"" + trunk_name + 
                                "\", \"HangupSource\": \"" + hangup_source + "\"}";
        
        if ((status == "fail" || status == "unknown" || status == "busy" || status == "invalid-number" || status == "no-answer") && ci.totalDuration.sec <= 10) {
            std::cout << "Call " << status << " with no duration, adding delay to ensure proper status update: " << ci.totalDuration.sec << " seconds" << std::endl;
            // prevent race condition between status updatef in 'ringing' and 'unknown/failed
            std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        }

        if (status == "completed" && ci.connectDuration.sec <= 3) {
            std::cout << "Call completed with very short duration, adding delay to ensure proper status update: " << ci.connectDuration.sec << " seconds" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(3000));
             // prevent misalined status update at control.vocallabs.ai
        }
        
        // Shard by call_id so every event for this call hits the same worker in order.
        WebhookManager::instance().send(string_call_id, webhook_url, json_data);
    }

    curl_global_cleanup();
    
    // CRITICAL: Remove from active_calls - this triggers DESTRUCTION of 'this' object
    // Do NOT access any member variables after this line!
    // All roads lead to 'DISCONNECTED' state when call is cut off, so cleanup is triggered here
    // if (should_cleanup) {
    //     try {
    //         std::string call_id_copy = string_call_id; // Copy ID just in case
    //         std::cout << "Call " << call_id_copy << " cleanup triggered at end of callback" << std::endl;
    //         CallManager::instance().cleanupCall(call_id_copy);
    //     } catch (const std::exception& e) {
    //         std::cerr << "Error cleaning up call: " << e.what() << std::endl;
    //     }
    // }
}

void MyCall::setupMedia() {
    if (!call_answered) return;

    try {
        pj::CallInfo ci = getInfo();
        for (unsigned i = 0; i < ci.media.size(); ++i) {
            if (ci.media[i].type == PJMEDIA_TYPE_AUDIO) {
                pj::AudioMedia aud_med = getAudioMedia(i);
                
                if (!med_port) {
                    std::cout << "Initializing WebSocket connection..." << std::endl;
                    med_port = new MyAudioMediaPort(ws_url, call_recording_id, sample_rate, directCall);
                    med_port->setCall(this);

                    if (!med_port->initializeWebSocket()) {
                        std::cout << "Failed to initialize WebSocket connection" << std::endl;
                        return;
                    }

                    pj::MediaFormatAudio fmt;
                    fmt.init(PJMEDIA_FORMAT_PCM, sample_rate, 1, 20000, 16);
                    med_port->createPort("med_port", fmt);

                    // Connect media
                    med_port->startTransmit(aud_med);
                    aud_med.startTransmit(*med_port);
                    
                    media_connected = true;
                    std::cout << "Media successfully connected" << std::endl;
                }
            }
        }
    } catch (pj::Error& err) {
        std::cout << "Error setting up media: " << err.info() << std::endl;
    }
}

void MyCall::onCallMediaState(pj::OnCallMediaStateParam& /*prm*/) {
    std::cout << "Media state changed, ensuring media is setup..." << std::endl;

    // Human bridge leg: once its media is active, splice it to the caller (once).
    if (is_bridge_leg) {
        if (!bridge_done) {
            bridgeWithPeer();
        }
        return;
    }

    if (!media_connected) {
        setupMedia();  // Initial setup
        return;
    }
    
    // Handle reconnection for re-INVITEs
    try {
        pj::CallInfo ci = getInfo();
        for (unsigned i = 0; i < ci.media.size(); ++i) {
            if (ci.media[i].type == PJMEDIA_TYPE_AUDIO) {
                pj::AudioMedia aud_med = getAudioMedia(i);
                std::cout << "Rebinding media due to re-INVITE..." << std::endl;
                bindMedia(aud_med);  // Reconnection only
                break;
            }
        }
    } catch (pj::Error& err) {
        std::cout << "Error rebinding media: " << err.info() << std::endl;
    }
}

void MyCall::startRecording() {
    if (med_port) {
        std::cout << "Starting call recording for ID: " << call_recording_id << std::endl;
        med_port->startRecording();
    }
}

void MyCall::onDtmfDigit(pj::OnDtmfDigitParam &prm) {
    std::cout << "DTMF digit detected: " << prm.digit << std::endl;
    
    if (med_port && media_connected && !prm.digit.empty()) {
        med_port->sendDtmf(prm.digit[0]);
    } else {
        std::cerr << "Cannot send DTMF: Media not connected or invalid digit" << std::endl;
    }
}

void MyCall::stopRecording() {
    if (med_port) {
        std::cout << "Stopping call recording for ID: " << call_recording_id << std::endl;
        med_port->stopRecording();
    }
}

void MyCall::bindMedia(pj::AudioMedia &aud_med) {
    if (!med_port) {
        med_port = new MyAudioMediaPort(ws_url, call_recording_id, sample_rate);
        med_port->setCall(this);

        if (!med_port->initializeWebSocket()) {
            std::cerr << "WS init failed\n";
            return;
        }

        pj::MediaFormatAudio fmt;
        fmt.init(PJMEDIA_FORMAT_PCM, sample_rate, 1, 20000, 16);
        med_port->createPort("med_port", fmt);
    }

    // Safe unlink first
    try { aud_med.stopTransmit(*med_port); } catch(...) {}
    try { med_port->stopTransmit(aud_med); } catch(...) {}

    // Link both ways
    med_port->startTransmit(aud_med);
    aud_med.startTransmit(*med_port);

    media_connected = true;
    std::cout << "Media (re)connected\n";
}

// --- Human transfer (phone-number) bridging -------------------------------

int MyCall::getActiveAudioMediaIndex() {
    try {
        pj::CallInfo ci = getInfo();
        for (unsigned i = 0; i < ci.media.size(); ++i) {
            if (ci.media[i].type == PJMEDIA_TYPE_AUDIO &&
                ci.media[i].status == PJSUA_CALL_MEDIA_ACTIVE) {
                return static_cast<int>(i);
            }
        }
    } catch (const pj::Error& err) {
        std::cerr << "[HumanTransfer] getActiveAudioMediaIndex error: " << err.info() << std::endl;
    }
    return -1;
}

// Caller leg: stop the WebSocket media port from pushing AI / ringback audio
// toward the caller (so it can be wired straight to the human leg), but KEEP the
// caller -> med_port direction so med_port's AudioRecorder keeps capturing the
// caller after the bridge. bridgeWithPeer() additionally feeds the human leg into
// this same med_port, giving a recording of the full bridged conversation.
void MyCall::detachWsMediaForBridge() {
    if (!med_port) return;
    try {
        int idx = getActiveAudioMediaIndex();
        if (idx >= 0) {
            pj::AudioMedia callerMed = getAudioMedia(idx);
            // Only stop med_port -> caller (AI/ringback). Leave caller -> med_port
            // connected so the recorder still hears the caller.
            try { med_port->stopTransmit(callerMed); } catch (...) {}
        }
    } catch (...) {}
    media_connected = false;
    std::cout << "[HumanTransfer] Stopped AI audio toward caller; kept caller->med_port for recording." << std::endl;
}

// Bridge leg (human): splice caller <-> human audio in the conference bridge.
void MyCall::bridgeWithPeer() {
    if (bridge_done || !bridge_peer) return;
    try {
        int hidx = getActiveAudioMediaIndex();
        if (hidx < 0) {
            std::cerr << "[HumanTransfer] No active audio on human leg yet." << std::endl;
            return;
        }
        int cidx = bridge_peer->getActiveAudioMediaIndex();
        if (cidx < 0) {
            // Caller probably hung up while the human was ringing; abandon the leg.
            std::cerr << "[HumanTransfer] Caller leg has no active audio; aborting bridge." << std::endl;
            pj::CallOpParam op;
            try { hangup(op); } catch (...) {}
            return;
        }

        pj::AudioMedia humanMed  = getAudioMedia(hidx);
        pj::AudioMedia callerMed = bridge_peer->getAudioMedia(cidx);

        // Stop ringback / AI audio toward the caller, then cross-connect both legs.
        bridge_peer->detachWsMediaForBridge();
        callerMed.startTransmit(humanMed);
        humanMed.startTransmit(callerMed);

        // Record the bridged conversation: route the human's audio into the
        // caller-leg's media port too (caller's audio still feeds it via
        // detachWsMediaForBridge), so its AudioRecorder captures caller + human.
        // med_port no longer transmits to anyone, so this adds no echo.
        if (bridge_peer->med_port) {
            try { humanMed.startTransmit(*bridge_peer->med_port); } catch (...) {}
            // Drop everything recorded before the bridge (AI phase + ringback) so the
            // recording contains only the post-transfer caller<->human conversation.
            bridge_peer->med_port->discardRecordingSoFar();
            std::cout << "[HumanTransfer] Human leg routed into caller med_port; pre-transfer audio discarded." << std::endl;
        }

        bridge_peer->bridged_leg = this;  // caller -> human, for mutual teardown
        bridge_done = true;
        std::cout << "[HumanTransfer] Caller bridged to human. Audio now flows caller <-> human." << std::endl;
    } catch (const pj::Error& err) {
        std::cerr << "[HumanTransfer] bridgeWithPeer error: " << err.info() << std::endl;
    }
}