#include "../include/audio_port.hpp"
#include "../include/call.hpp"
#include "../include/call_manager.hpp"
#include <iostream>
#include <thread>
#include <chrono>

MyAudioMediaPort::MyAudioMediaPort(const std::string& uri, const std::string& call_id, int sample_rate, bool directCall)
    : handler(nullptr), ws_uri(uri), initialized(false), call(nullptr), sample_rate(sample_rate), directCall(directCall) {
    recorder = std::make_shared<AudioRecorder>(call_id, sample_rate);
}

MyAudioMediaPort::~MyAudioMediaPort() {
    cleanup();
}

void MyAudioMediaPort::cleanup() {
    if (!handler) return;

    // Register this thread with PJSIP before doing anything
    static pj_thread_desc thread_desc;
    static pj_thread_t *thread_handle;
    
    if (!pj_thread_is_registered()) {
        pj_bzero(thread_desc, sizeof(thread_desc));
        if (pj_thread_register("CleanupThread", thread_desc, &thread_handle) != PJ_SUCCESS) {
            std::cerr << "Failed to register cleanup thread with PJSIP" << std::endl;
            return;
        }
    }

    try {
        // First disconnect WebSocket to stop audio transmission
        handler->disconnect();
        std::cout << "WebSocket disconnected during cleanup" << std::endl;

        // Critical: Wait for any ongoing PJMEDIA clock operations to finish
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));

        try {
            // Now unregister the media port when audio operations have settled
            unregisterMediaPort();
            std::cout << "Media port unregistered successfully" << std::endl;
        } catch (...) {
            // Ignore all cleanup errors - port may already be cleaned up by PJSIP
            std::cout << "Media port cleanup completed (may have been auto-cleaned)" << std::endl;
        }
        
        // Extended pause to ensure all PJMEDIA clock threads release references
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

        // Clean up the handler
        try {
            delete handler;
            handler = nullptr;
        } catch (...) {
            std::cerr << "Media clean up the handler failed: setting handler to nullptr" << std::endl;
            handler = nullptr;
        }
        initialized = false;
        std::cout << "Cleanup completed successfully" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error during cleanup: " << e.what() << std::endl;
    }
}

void MyAudioMediaPort::setCall(MyCall* call_ptr) {
    call = call_ptr;
}

void MyAudioMediaPort::onWebSocketDisconnected() {
    if (!call) return;

    // Register this thread with PJSIP before doing anything
    static pj_thread_desc thread_desc;
    static pj_thread_t *thread_handle;
    
    if (!pj_thread_is_registered()) {
        pj_bzero(thread_desc, sizeof(thread_desc));
        if (pj_thread_register("WebSocketThread", thread_desc, &thread_handle) != PJ_SUCCESS) {
            std::cerr << "Failed to register thread with PJSIP" << std::endl;
            return;
        }
    }

    // Now that the thread is registered, we can use PJSIP functions
    std::cout << "WebSocket disconnected, checking call state..." << std::endl;

    try {
        pjsua_call_id call_id = call->getId();

        if (call_id < 0 || call_id >= (int)pjsua_call_get_max_count()) {
            std::cout << "Call ID is invalid (" << call_id << "), call already cleaned up" << std::endl;
            return;
        }
        
        if (!pjsua_call_is_active(call_id)) {
            std::cout << "Call is no longer active, skipping hangup" << std::endl;
            return;
        }

        // Try to get call info and handle any errors
        pj::CallInfo ci;
        try {
            ci = call->getInfo();
        } catch (const pj::Error& err) {
            if (err.status == PJSIP_ESESSIONTERMINATED) {
                std::cout << "Call session already terminated, no hangup needed" << std::endl;
                return;
            }
            throw; // Re-throw unexpected errors
        }

        // If call is already disconnected, no need to hang up
        if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
            std::cout << "Call already disconnected, no hangup needed" << std::endl;
            return;
        }

        // Call is still active, attempt hangup
        // Do NOT prefix this with sendRequest({method:"BYE"}) — on a non-CONFIRMED
        // dialog a manual BYE drops inv->ref_cnt to 0, and a later async 401/200
        // then asserts in pjsip_inv_add_ref. PJSUA's hangup() picks CANCEL or BYE
        // correctly based on the current invite-session state.
        std::cout << "Call is active, initiating hangup..." << std::endl;
        pj::CallOpParam param;
        call->hangup(param);
        std::cout << "Call hangup initiated successfully" << std::endl;

    } catch (const pj::Error& err) {
        if (err.status == PJSIP_ESESSIONTERMINATED) {
            std::cout << "Call session was terminated during hangup process" << std::endl;
        } else {
            std::cerr << "Error during call hangup: " << err.info() << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception during call hangup process: " << e.what() << std::endl;
    }
}

bool MyAudioMediaPort::initializeWebSocket() {
    if (!initialized) {
        std::cout << "Initializing WebSocket with URI: " << ws_uri << std::endl;
        handler = new WebSocketAudioHandler(sample_rate, ws_uri);
        handler->setDisconnectCallback([this]() {
            onWebSocketDisconnected();
        });
        handler->setMediaPort(this);
        
        if (ws_uri.empty()) {
            std::cerr << "WebSocket URI is empty" << std::endl;
            return false;
        }
        
        for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
            std::cout << "Attempting WebSocket connection (attempt " << attempt << " of " << MAX_RETRIES << ")..." << std::endl;
            
            if (handler->connect(ws_uri)) {
                initialized = true;
                std::cout << "WebSocket connection established on attempt " << attempt << std::endl;
                // start ringing for direct dashboard calls
                if (ws_uri.find("://rupture") != std::string::npos && !directCall) {
                    handler->startRinging();
                }
                return true;
            }
            
            if (attempt < MAX_RETRIES) {
                std::cout << "Connection failed, retrying in " << RETRY_DELAY_MS << "ms..." << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_DELAY_MS));
            }
        }
        
        std::cout << "Failed to establish WebSocket connection after " << MAX_RETRIES << " attempts" << std::endl;
        delete handler;
        handler = nullptr;
        return false;
    }
    return true;
}

void MyAudioMediaPort::onFrameRequested(pj::MediaFrame& frame) {
    // PJSUA2's get_frame memcpys frame.size bytes into a fixed-size pjmedia buffer;
    // returning more than entry-size corrupts the heap. Force every path to exactly `expected`.
    const size_t expected = frame.size;

    auto fit = [expected](std::vector<uint8_t>& data) {
        if (data.size() & 1u) data.pop_back();
        if (data.size() > expected) data.resize(expected);
        else if (data.size() < expected) data.resize(expected, 0);
    };

    auto emit_silence = [&]() {
        frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
        frame.buf.assign(expected, 0);
        frame.size = expected;
    };

    try {
        if (!initialized || !handler) {
            emit_silence();
            return;
        }

        if (handler->isRingingActive()) {
            std::vector<uint8_t> ringing_data;
            handler->getRingingFrame(ringing_data, expected);
            fit(ringing_data);
            frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
            frame.buf = std::move(ringing_data);
            frame.size = expected;
            return;
        }

        std::vector<uint8_t> audio_data;
        if (handler->getAudioFrame(audio_data)) {
            fit(audio_data);
            frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
            frame.buf = std::move(audio_data);
            frame.size = expected;
            
            // Record agent's audio
            if (recorder) {
                try {
                    recorder->writeFrame(frame, true);  // true indicates agent audio
                } catch (...) {
                    // Ignore recording errors
                }
            }
        } else {
            emit_silence();
        }
    } catch (...) {
        emit_silence();
    }
}

void MyAudioMediaPort::onFrameReceived(pj::MediaFrame& frame) {
    if (initialized && handler && !frame.buf.empty()) {
        handler->sendAudioFrame(frame.buf.data(), frame.buf.size());

        // MANUALLY UPDATE PJSIP STATISTICS
        if (call) {
            try {
                pjsua_call_info ci;
                pjsua_call_get_info(call->getId(), &ci);
                // Force statistics update - this requires accessing internal PJSIP structures
            } catch (...) {
                // Ignore errors
            }
        }

        if (recorder) {
            recorder->writeFrame(frame, false);  // false indicates user audio
        }
    }
}

bool MyAudioMediaPort::isInitialized() const {
    return initialized;
}

void MyAudioMediaPort::startRecording() {
    if (recorder) {
        recorder->startRecording();
    }
}

void MyAudioMediaPort::stopRecording() {
    if (recorder) {
        recorder->stopRecording();
    }
}

void MyAudioMediaPort::discardRecordingSoFar() {
    if (recorder) {
        recorder->discardBufferedAudio();
    }
}

void MyAudioMediaPort::sendDtmf(char digit) {
    if (initialized && handler) {
        handler->sendDtmfEvent(digit);
    } else {
        std::cerr << "Cannot send DTMF: WebSocket not initialized" << std::endl;
    }
}

void MyAudioMediaPort::setHangupSource(const std::string& source) {
    if (call) {
        try {
            call->setHangupSource(source);
        } catch (const std::exception& e) {
            std::cerr << "[MyAudioMediaPort] Error setting hangup source: " << e.what() << std::endl;
            // Clear the call pointer as it might be invalid
            call = nullptr;
        }
    }
}

std::string MyAudioMediaPort::getHangupSource() {
    if (call) {
        try {
            return call->getHangupSource();
        } catch (const std::exception& e) {
            std::cerr << "[MyAudioMediaPort] Error getting hangup source: " << e.what() << std::endl;
            // Clear the call pointer as it might be invalid
            call = nullptr;
        }
    }
    return "none";
}

void MyAudioMediaPort::receiveDtmfFromWebsocket(const std::string& text) {
    if (!call) {
        std::cerr << "Cannot send DTMF to SIP: Call not set" << std::endl;
        return;
    }

    try {
        for (char digit : text) {
            if (std::isdigit(digit) || digit == '*' || digit == '#') {
                pj::CallSendDtmfParam param;
                param.digits = std::string(1, digit);
                call->sendDtmf(param);
                std::cout << "Sent DTMF digit to SIP provider: " << digit << std::endl;
                
                // Add a small delay between digits
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            } else {
                std::cerr << "Invalid DTMF digit received from websocket: " << digit << std::endl;
            }
        }
    } catch (const pj::Error& err) {
        std::cerr << "Error sending DTMF to SIP provider: " << err.info() << std::endl;
    }
}

void MyAudioMediaPort::startHumanNumberTransfer(const std::string& number) {
    if (!call) {
        std::cerr << "[HumanTransfer] Cannot transfer: call not set" << std::endl;
        return;
    }
    // Blind SIP REFER on the existing dialog — Asterisk joins the two humans and
    // BYEs our leg. No second call leg is created here (unlike the old
    // transferToHumanNumber bridge path).
    CallManager::instance().referCallerToHuman(call, number);
}

void MyAudioMediaPort::handleRtpEndpointChange(const std::string& new_endpoint) {
    if (current_rtp_endpoint != new_endpoint) {
        std::cout << "Switching RTP endpoint: " << current_rtp_endpoint 
                 << " -> " << new_endpoint << std::endl;
        current_rtp_endpoint = new_endpoint;
        
        // Reconnect audio media
        reconnectAudioMedia();
    }
}

void MyAudioMediaPort::reconnectAudioMedia() {
    if (call && current_audio_media) {
        try {
            // Disconnect old connection - protect each operation
            try {
                current_audio_media->stopTransmit(*this);
            } catch (...) {
                std::cerr << "Warning: Failed to stop media transmission" << std::endl;
            }
            
            try {
                this->stopTransmit(*current_audio_media);
            } catch (...) {
                std::cerr << "Warning: Failed to stop port transmission" << std::endl;
            }
            
            // Get new audio media
            pj::CallInfo ci = call->getInfo();
            for (unsigned i = 0; i < ci.media.size(); ++i) {
                if (ci.media[i].type == PJMEDIA_TYPE_AUDIO) {
                    try {
                        pj::AudioMedia new_aud_med = call->getAudioMedia(i);
                        
                        // Reconnect - protect each operation
                        try {
                            this->startTransmit(new_aud_med);
                        } catch (...) {
                            std::cerr << "Warning: Failed to start port transmission" << std::endl;
                            continue;
                        }
                        
                        try {
                            new_aud_med.startTransmit(*this);
                        } catch (...) {
                            std::cerr << "Warning: Failed to start media transmission" << std::endl;
                            continue;
                        }
                        
                        current_audio_media = &new_aud_med;
                        std::cout << "Audio media reconnected to new endpoint" << std::endl;
                        break;
                    } catch (...) {
                        std::cerr << "Warning: Failed to get audio media for index " << i << std::endl;
                        continue;
                    }
                }
            }
        } catch (pj::Error& err) {
            std::cerr << "Error reconnecting audio media: " << err.info() << std::endl;
        } catch (...) {
            std::cerr << "Unexpected error during media reconnection" << std::endl;
        }
    }
}