#include "../include/websocket_handler.hpp"
#include "../include/audio_port.hpp"
#include <iostream>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <spandsp.h>

WebSocketAudioHandler::WebSocketAudioHandler(int sample_rate, const std::string& ws_uri)
    : connected(false)
    , sequence_number(0)
    , stream_id("MZ" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()))
    , checkpoint_received(false)
    , clear_audio_called(false)
    , thread_registered(false)
    , thread_handle(nullptr)
    , hangup_received(false)
    , intentional_close(false)
    , reconnect_attempts(0)
    , med_port(nullptr)
    , sample_rate(sample_rate)
    , ws_uri(ws_uri)
    , current_agent_id("")
    , agent_transfer_in_progress(false)
    , ringing_active(false)
{
    // PJMEDIA frame size for our port format: 20ms * 2 bytes/sample * sample_rate / 1000ms.
    bytes_per_frame = (static_cast<size_t>(sample_rate) * 20 * 2) / 1000;
    log_misalignment_done = false;

    ringing_provider = std::make_unique<RingingAudioProvider>(sample_rate);

    ws_client.init_asio();
    ws_client.clear_access_channels(websocketpp::log::alevel::all);
    ws_client.clear_error_channels(websocketpp::log::elevel::all);
    ws_client.set_access_channels(websocketpp::log::alevel::connect);
    ws_client.set_access_channels(websocketpp::log::alevel::disconnect);
    ws_client.set_error_channels(websocketpp::log::elevel::fatal);
    ws_client.set_tls_init_handler([](websocketpp::connection_hdl) {
        return websocketpp::lib::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::sslv23);
    });

    if (ws_uri.find("rupture") != std::string::npos) {
        REAL_TIME_AUDIO = true;
    }

    // Set agent_id from ws_uri here
    setAgentId();
    
    ws_client.set_message_handler([this](connection_hdl, tls_client::message_ptr msg) {
        if (msg->get_opcode() == websocketpp::frame::opcode::text) {
            try {
                auto j = json::parse(msg->get_payload());

                // LOG EVERY FIELD IN THE MESSAGE
                // std::cout << "📥 PARSED JSON SUCCESSFULLY:" << std::endl;
                // for (auto it = j.begin(); it != j.end(); ++it) {
                //     std::cout << "  " << it.key() << " = " << it.value() << std::endl;
                // }

                std::string event = j["event"];
                if (event == "playAudio") {

                    auto media = j["media"];
                    // for (auto it = media.begin(); it != media.end(); ++it) {
                    //     if (it.key() == "payload") {
                    //         std::cout << "    " << it.key() << " = [" << it.value().get<std::string>().length() << " chars]" << std::endl;
                    //     } else {
                    //         std::cout << "    " << it.key() << " = " << it.value() << std::endl;
                    //     }
                    // }

                    // stop ringing when incoming starts, but not for simulated calls (media port is null since not fully set up)
                    if (ringing_active && med_port) {
                        stopRinging();
                        clearAudioQueue();
                        std::cout << "[Ringing] Cleared ringing audio upon human answering." << std::endl;
                    }

                    std::string payload = j["media"]["payload"];
                    std::string contentType = j["media"]["contentType"];
                    auto decoded = base64_decode(payload);

                    if (contentType == "audio/x-mulaw") {
                        decoded = convertFromMulaw(decoded);
                    }

                    if (j["media"].contains("volume")) {
                        try {
                            float volume = j["media"]["volume"].get<float>();
                            adjustVolume(decoded, volume);
                        } catch (const std::exception& e) {
                            std::cerr << "Error parsing volume: " << e.what() << std::endl;
                        }
                    }
                    
                    enqueueAudio(std::move(decoded));
                } else if (event == "clearAudio") {
                    clearAudioQueue();
                    std::cout << "Audio queue cleared" << std::endl;
                } else if (event == "hangup") {
                    std::cout << "Received hangup event" << std::endl;
                    hangup_received = true;  // Mark as hangup before triggering disconnect
                    if (med_port) {
                        const std::string& hangup_reason = j["hangupType"];
                        const std::string current_hangup_source = med_port->getHangupSource();
                        std::cout << "Setting hangup source to: " << hangup_reason << std::endl;
                        if (!current_hangup_source.empty() && current_hangup_source == "none") {
                            med_port->setHangupSource(hangup_reason);
                        }
                    }
                    if (disconnect_callback) {
                        disconnect_callback();  // This will trigger call termination
                    }
                } else if (event == "sendDtmf") {
                    if (j.contains("text")) {
                        std::string dtmf_text = j["text"];
                        std::cout << "Received sendDtmf event with text: " << dtmf_text << std::endl;
                        if (med_port) {
                            med_port->receiveDtmfFromWebsocket(dtmf_text);
                        }
                    }
                } else if (event == "checkpoint") {
                    checkpoint_received = true;
                    if (j.contains("streamId")) {
                        pending_stream_id = j["streamId"];
                    }
                    if (j.contains("name")) {
                        pending_stream_name = j["name"];
                    }
                    std::cout << "Checkpoint received for stream: " << pending_stream_id << std::endl;
                } else if (event == "agentTransfer") {
                    /*
                    Key: agentId, Value: "61d45fe5-39cf-42ba-bcc2-b4cd2f164b72"
                    Key: event, Value: "agentTransfer"
                    Key: name, Value: "bot"
                    */ 

                    std::cout << "Received agent Transfer event" << std::endl;
                    // for (auto it = j.begin(); it != j.end(); ++it) {
                    //     std::cout << "Key: " << it.key() << ", Value: " << it.value() << std::endl;
                    // }
                    
                    agent_transfer_in_progress = true;

                    if (j.contains("agentId")) {
                        std::string new_agent_id = j["agentId"];
                        // std::string new_agent_name = "";
                        // if (j.contains("name")) {
                        //     new_agent_name = j["name"];
                        // }

                        std::cout << "Call being transferred from agent: " << current_agent_id << " to Agent with agentId: " << new_agent_id << std::endl;

                        // perfortm agent transfer
                        performAgentTransfer(new_agent_id);
                    } else {
                        // Transfer failed due to incomplete details parsed
                        agent_transfer_in_progress = false;
                        std::cout << "Call agent transfer failed since agentId not parsed" << std::endl;
                    }
                } else if (event == "humanTransfer") {
                    std::cout << "Received human Transfer event" << std::endl;
                    // for (auto it = j.begin(); it != j.end(); ++it) {
                    //     std::cout << "Key: " << it.key() << ", Value: " << it.value() << std::endl;
                    // }

                    agent_transfer_in_progress = true;

                    if (j.contains("humanNumber")) {
                        // Phone-number transfer: dial a real human via Asterisk and
                        // bridge the caller to them (no rupture WebSocket involved).
                        std::string human_number = j["humanNumber"];
                        std::cout << "Call being transferred to human phone number: " << human_number << std::endl;
                        performHumanNumberTransfer(human_number);
                    } else if (j.contains("humanId")) {
                        std::string new_human_id = j["humanId"];
                        // std::string new_human_name = "";
                        // if (j.contains("name")) {
                        //     new_human_name = j["name"];
                        // }

                        std::cout << "Call being transferred to human with humanId: " << new_human_id << std::endl;

                        performHumanTransfer(new_human_id);
                    } else {
                        // Transfer failed due to incomplete details parsed
                        agent_transfer_in_progress = false;
                        std::cout << "Call human transfer failed since humanId/humanNumber not parsed" << std::endl;
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "Error processing message: " << e.what() << std::endl;
            }
        }
    });

    ws_client.set_open_handler([this](connection_hdl hdl) {
        conn_hdl = hdl;
        connected = true;
        std::cout << "WebSocket connected" << std::endl;
        
        sendStartEvent();
    });

    ws_client.set_close_handler([this](connection_hdl hdl) {
        try {
            auto con = ws_client.get_con_from_hdl(hdl);
            auto status = con->get_remote_close_code();
            auto reason = con->get_remote_close_reason();

            std::cout << "WebSocket closed with status: " << status
                    << " reason: " << reason << std::endl;

            connected = false;

            if (agent_transfer_in_progress) {
                std::cout << "WebSocket closed during agent transfer - not triggering hangup" << std::endl;
                return;
            }

            // WS-emitted hangup or SIP-side teardown: tear the call down (idempotent).
            if (hangup_received || intentional_close.load()) {
                if (disconnect_callback) disconnect_callback();
                hangup_received = false;
                return;
            }

            // Unintentional drop. Reconnect only if URL isn't "rupture2" and we haven't
            // exhausted our MAX_RECONNECT_ATTEMPTS budget for this handler's lifetime.
            // (`this->` because the constructor parameter `ws_uri` shadows the member inside this lambda.)
            bool no_rupture2 = (this->ws_uri.find("rupture2") == std::string::npos);
            if (no_rupture2 && reconnect_attempts.load() < MAX_RECONNECT_ATTEMPTS) {
                int attempt = ++reconnect_attempts;
                std::cout << "Unexpected WebSocket drop - scheduling reconnect attempt "
                          << attempt << " of " << MAX_RECONNECT_ATTEMPTS << "." << std::endl;
                attemptReconnect();
                return;
            }

            // No reconnect possible (rupture2 URL or attempts exhausted) - terminate the SIP leg.
            std::cout << "No reconnect path available - terminating SIP call." << std::endl;
            if (med_port) {
                const std::string current_hangup_source = med_port->getHangupSource();
                if (!current_hangup_source.empty() && current_hangup_source == "none") {
                    std::cout << "Setting hangup source to: Websocket due to failsafe" << std::endl;
                    med_port->setHangupSource("Websocket");
                }
            }
            if (disconnect_callback) disconnect_callback();
        } catch (const std::exception& e) {
            std::cerr << "Error in close handler: " << e.what() << std::endl;
        }
    });
}

WebSocketAudioHandler::~WebSocketAudioHandler() {
    disconnect();
}

void WebSocketAudioHandler::clearAudioQueue() {
    std::lock_guard<std::mutex> lock(queue_mutex);
    std::queue<std::vector<uint8_t>> empty;
    std::swap(audio_queue, empty);
    partial_frame.clear();
    log_misalignment_done = false;
    clear_audio_called = true;
    resetCheckpointState();
}

void WebSocketAudioHandler::enqueueAudio(std::vector<uint8_t>&& pcm) {
    if (bytes_per_frame == 0) return;
    if (pcm.size() & 1u) pcm.pop_back();  // 16-bit PCM must be even-length

    std::lock_guard<std::mutex> lock(queue_mutex);

    if (!log_misalignment_done && (pcm.size() % bytes_per_frame) != 0) {
        std::cout << "[Audio] WS chunk not aligned to " << bytes_per_frame
                  << "B frames (chunk=" << pcm.size() << "B, carry="
                  << partial_frame.size() << "B); chunking with carry-over."
                  << std::endl;
        log_misalignment_done = true;
    }

    partial_frame.insert(partial_frame.end(),
                         std::make_move_iterator(pcm.begin()),
                         std::make_move_iterator(pcm.end()));

    size_t offset = 0;
    while (partial_frame.size() - offset >= bytes_per_frame) {
        std::vector<uint8_t> frame(partial_frame.begin() + offset,
                                   partial_frame.begin() + offset + bytes_per_frame);
        offset += bytes_per_frame;

        if (REAL_TIME_AUDIO) {
            if (audio_queue.size() >= MAX_QUEUE_SIZE_RUPTURE) audio_queue.pop();
            audio_queue.push(std::move(frame));
        } else {
            if (audio_queue.size() < MAX_QUEUE_SIZE_DEFAULT) {
                audio_queue.push(std::move(frame));
            } else {
                break;
            }
        }
    }
    if (offset > 0) {
        partial_frame.erase(partial_frame.begin(), partial_frame.begin() + offset);
    }
}

void WebSocketAudioHandler::resetCheckpointState() {
    checkpoint_received = false;
    clear_audio_called = false;
    pending_stream_name = "";
    pending_stream_id = "";
}

void WebSocketAudioHandler::sendPlayedStreamEvent() {
    if (!connected || pending_stream_id.empty()) return;

    try {
        json message = {
            {"event", "playedStream"},
            {"streamId", pending_stream_id},
            {"name", pending_stream_name}
        };

        websocketpp::lib::error_code ec;
        ws_client.send(conn_hdl, message.dump(), websocketpp::frame::opcode::text, ec);
        if (ec) {
            std::cerr << "Error sending playedStream event: " << ec.message() << std::endl;
        } else {
            std::cout << "PlayedStream event sent successfully" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error creating playedStream event message: " << e.what() << std::endl;
    }

    resetCheckpointState();
}

std::string WebSocketAudioHandler::base64_encode(const uint8_t* data, size_t size) {
    std::string base64;
    base64.resize(boost::beast::detail::base64::encoded_size(size));
    base64.resize(boost::beast::detail::base64::encode(
        &base64[0], data, size));
    return base64;
}

std::vector<uint8_t> WebSocketAudioHandler::base64_decode(const std::string& base64) {
    std::vector<uint8_t> decoded;
    decoded.resize(boost::beast::detail::base64::decoded_size(base64.size()));
    auto result = boost::beast::detail::base64::decode(&decoded[0], base64.data(), base64.size());
    size_t written = result.first;
    decoded.resize(written);
    return decoded;
}

std::vector<uint8_t> WebSocketAudioHandler::convertFromMulaw(const std::vector<uint8_t>& mulaw_data) {
    std::vector<uint8_t> pcm_data;
    pcm_data.reserve(mulaw_data.size() * 2);

    for (uint8_t mulaw_sample : mulaw_data) {
        int16_t pcm_sample = ulaw_to_linear(mulaw_sample);
        pcm_data.push_back(pcm_sample & 0xFF);
        pcm_data.push_back((pcm_sample >> 8) & 0xFF);
    }

    return pcm_data;
}

void WebSocketAudioHandler::sendStartEvent() {
    if (!connected) return;

    try {
        json message = {
            {"event", "start"},
            {"start", {
                {"streamId", stream_id},
                {"mediaFormat", {
                    {"encoding", "audio/x-l16"},
                    {"sampleRate", sample_rate}
                }}
            }}
        };

        websocketpp::lib::error_code ec;
        ws_client.send(conn_hdl, message.dump(), websocketpp::frame::opcode::text, ec);
        if (ec) {
            std::cerr << "Error sending start event: " << ec.message() << std::endl;
        } else {
            std::cout << "Start event sent successfully" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error creating start event message: " << e.what() << std::endl;
    }
}

void WebSocketAudioHandler::registerThread() {
    if (!thread_registered) {
        pj_bzero(thread_desc, sizeof(thread_desc));
        if (pj_thread_register("WebSocketThread", thread_desc, &thread_handle) == PJ_SUCCESS) {
            thread_registered = true;
            std::cout << "WebSocket thread registered with PJSIP" << std::endl;
        } else {
            std::cerr << "Failed to register WebSocket thread with PJSIP" << std::endl;
        }
    }
}

bool WebSocketAudioHandler::connect(const std::string& uri) {
    if (uri == "TEST_SIPP_RINGING") {
        std::cout << "TEST_SIPP_RINGING detected. Bypassing WebSocket connection and starting ringing audio." << std::endl;
        startRinging();
        connected = true;
        return true;
    }

    try {
        // Reset client state
        if (ws_thread.joinable()) {
            ws_thread.join();
        }
        
        // Reset the client
        ws_client.reset();
        
        thread_registered = false;
        websocketpp::lib::error_code ec;
        std::cout << "Creating WebSocket connection to " << uri << std::endl;
        
        auto conn = ws_client.get_connection(uri, ec);
        if (ec) {
            std::cerr << "Error creating connection: " << ec.message() << std::endl;
            return false;
        }

        ws_client.connect(conn);
        
        std::cout << "Starting WebSocket client thread..." << std::endl;
        ws_thread = std::thread([this]() {
            try {
                registerThread();
                std::cout << "WebSocket client thread started" << std::endl;
                ws_client.run();
            } catch (const std::exception& e) {
                std::cerr << "WebSocket thread error: " << e.what() << std::endl;
            }
        });

        std::cout << "Waiting for WebSocket connection..." << std::endl;
        int timeout = 50;
        while (!connected && timeout > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            timeout--;
        }

        return connected;
    } catch (const std::exception& e) {
        std::cerr << "Error connecting to WebSocket: " << e.what() << std::endl;
        return false;
    }
}

void WebSocketAudioHandler::disconnect() {
    if (connected) {
        try {
            // Mark this close as caller-initiated so the close handler doesn't try to reconnect.
            // The only callers are cleanup()/destructor, both of which run when the SIP leg is going down.
            intentional_close = true;
            websocketpp::lib::error_code ec;
            ws_client.close(conn_hdl, websocketpp::close::status::normal, "Call ended", ec);
            connected = false;
        } catch (const std::exception& e) {
            std::cerr << "Error disconnecting WebSocket: " << e.what() << std::endl;
        }
    }

    if (ws_thread.joinable()) {
        ws_thread.join();
    }
}

void WebSocketAudioHandler::attemptReconnect() {
    std::thread reconnect_thread([this]() {
        try {
            std::string new_uri = ws_uri;
            if (new_uri.find("reconnect=") == std::string::npos) {
                new_uri += (new_uri.find('?') == std::string::npos) ? "?reconnect=true" : "&reconnect=true";
            }

            std::cout << "Reconnecting WebSocket to: " << new_uri << std::endl;

            // Let the old close handler return and ws_client.run() unwind before we join+reset.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            // If SIP started tearing down while we were waiting, abort the reconnect.
            if (this->intentional_close.load()) {
                std::cout << "SIP teardown raced reconnect - aborting." << std::endl;
                return;
            }

            // Fresh stream id and sequence; the server sees a new session with the reconnect flag.
            sequence_number = 0;
            stream_id = "MZ" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
            ws_uri = new_uri;

            if (connect(new_uri)) {
                std::cout << "WebSocket reconnect succeeded." << std::endl;
                return;
            }

            // connect() never established. Retry if we still have budget left;
            // otherwise fall through and terminate the SIP leg.
            if (!this->intentional_close.load() && reconnect_attempts.load() < MAX_RECONNECT_ATTEMPTS) {
                int attempt = ++reconnect_attempts;
                std::cout << "WebSocket reconnect failed - retrying, attempt "
                          << attempt << " of " << MAX_RECONNECT_ATTEMPTS << "." << std::endl;
                attemptReconnect();
                return;
            }

            std::cerr << "WebSocket reconnect failed after " << MAX_RECONNECT_ATTEMPTS
                      << " attempts - terminating SIP call." << std::endl;
            if (med_port) {
                const std::string current_hangup_source = med_port->getHangupSource();
                if (!current_hangup_source.empty() && current_hangup_source == "none") {
                    med_port->setHangupSource("Websocket");
                }
            }
            if (disconnect_callback) disconnect_callback();
        } catch (const std::exception& e) {
            std::cerr << "Exception during WebSocket reconnect: " << e.what() << std::endl;
            if (disconnect_callback) disconnect_callback();
        }
    });
    reconnect_thread.detach();
}

std::string WebSocketAudioHandler::formatTimestamp(long long milliseconds) {
    // Convert milliseconds to seconds
    std::time_t timeInSeconds = milliseconds / 1000;

    // Convert to tm structure for formatting
    std::tm tm_time = *std::localtime(&timeInSeconds);

    // Format the output string
    std::ostringstream oss;
    oss << std::put_time(&tm_time, "%Y-%m-%d %H:%M:%S"); 
    return oss.str();
}

void WebSocketAudioHandler::sendAudioFrame(const void* data, size_t size) {
    if (!connected) return;

    try {
        json message = {
            {"event", "media"},
            {"sequenceNumber", std::to_string(++sequence_number)},
            {"media", {
                {"track", "inbound"},
                {"chunk", "1"},
                {"contentType", "audio/x-l16"},
                {"timestamp", formatTimestamp(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count())},
                {"payload", base64_encode(static_cast<const uint8_t*>(data), size)}
            }},
            {"streamId", stream_id}
        };

        websocketpp::lib::error_code ec;
        ws_client.send(conn_hdl, message.dump(), websocketpp::frame::opcode::text, ec);
        if (ec) {
            // Connection is in closing/closed state — stop sending immediately.
            // Without this, the PJSIP media clock fires every 20ms and floods logs
            // for the entire window between the remote close frame arriving and the
            // close handler finally setting connected=false.
            connected = false;
            std::cerr << "Error sending audio frame: " << ec.message() << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error creating audio message: " << e.what() << std::endl;
    }
}

// OBSOL
void WebSocketAudioHandler::handleReceivedMessage(const std::string& message) {
    try {
        auto j = json::parse(message);

        for (auto it = j.begin(); it != j.end(); ++it) {
            std::cout << "Key: " << it.key() << ", Value: " << it.value() << std::endl;
        }

        if (j["event"] == "playAudio") {
            std::string payload = j["media"]["payload"];
            std::string contentType = j["media"]["contentType"];
            int received_sample_rate = j["media"]["sampleRate"];
            auto decoded = base64_decode(payload);

            std::vector<uint8_t> audio_data;
            if (contentType == "audio/x-mulaw") {
                std::cout << "Received mu-law audio" << std::endl;
                if (decoded.size() != 160) {
                    std::cout << "Unexpected mu-law data size: " << decoded.size() << std::endl;
                }
                audio_data = convertFromMulaw(decoded);
            } else if (contentType == "audio/x-l16") {
                std::cout << "Received PCM audio" << std::endl;
                if (decoded.size() != 320) {
                    std::cout << "Unexpected PCM data size: " << decoded.size() << std::endl;
                }
                audio_data = decoded;
            } else {
                std::cerr << "Unknown content type: " << contentType << std::endl;
                return;
            }

            std::cout << "Received audio data of size: " << audio_data.size() 
                      << " bytes with sample rate: " << received_sample_rate << "Hz" << std::endl;

            // Resample if needed
            if (received_sample_rate != sample_rate) {
                std::cout << "Resampling from " << received_sample_rate 
                         << "Hz to " << sample_rate << "Hz" << std::endl;
                audio_data = resampleAudio(audio_data, received_sample_rate, sample_rate);
            }

            std::lock_guard<std::mutex> lock(queue_mutex);
            if (REAL_TIME_AUDIO) {
                if (audio_queue.size() >= MAX_QUEUE_SIZE_RUPTURE) audio_queue.pop();
                audio_queue.push(std::move(decoded));
            } else {
                if (audio_queue.size() < MAX_QUEUE_SIZE_DEFAULT) {
                    audio_queue.push(std::move(decoded));
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error processing message: " << e.what() << std::endl;
    }
}

bool WebSocketAudioHandler::getAudioFrame(std::vector<uint8_t>& frame) {
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (!audio_queue.empty()) {
        frame = std::move(audio_queue.front());
        audio_queue.pop();
        if (checkpoint_received && audio_queue.empty() && !clear_audio_called) {
            sendPlayedStreamEvent();
        }
        return true;
    }
    return false;
}

bool WebSocketAudioHandler::isConnected() const {
    return connected;
}

void WebSocketAudioHandler::setDisconnectCallback(DisconnectCallback callback) {
    disconnect_callback = callback;
}

void WebSocketAudioHandler::sendDtmfEvent(char digit) {
    if (!connected) return;

    try {
        json message = {
            {"event", "dtmf"},
            {"dtmf", {
                {"digit", std::string(1, digit)},
                {"streamId", stream_id}
            }}
        };

        websocketpp::lib::error_code ec;
        ws_client.send(conn_hdl, message.dump(), websocketpp::frame::opcode::text, ec);
        if (ec) {
            std::cerr << "Error sending DTMF event: " << ec.message() << std::endl;
        } else {
            std::cout << "DTMF event sent successfully for digit: " << digit << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error creating DTMF event message: " << e.what() << std::endl;
    }
}

void WebSocketAudioHandler::setMediaPort(MyAudioMediaPort* port) {
    med_port = port;
}

void WebSocketAudioHandler::setAgentId() {
    if (ws_uri.empty()) {
        current_agent_id = "";
        return;
    }

    size_t agent_pos = ws_uri.find("agent=");
    if (agent_pos == std::string::npos) {
        current_agent_id = "";
        std::cout << "No agentId found in WebSocket URI" << std::endl;
        return;
    }

    agent_pos += 6; // Move past "agent="
    
    std::string agent_value = ws_uri.substr(agent_pos);
    size_t underscore_pos = agent_value.find('_');
    if (underscore_pos == std::string::npos) {
        std::cerr << "agent_id delimiter not found." << std::endl;
        return;
    }
    
    std::string agent_id = agent_value.substr(0, underscore_pos);
    std::cout << "Agent ID: " << agent_id << std::endl;
    current_agent_id = agent_id;

    return;
}

void WebSocketAudioHandler::performAgentTransfer(const std::string& new_agent_id) {
    std::thread transfer_thread([this, new_agent_id]() {
        try {
            std::cout << "Starting agent transfer to: " << new_agent_id << std::endl;
            
            // Clear audio queue before transfer
            clearAudioQueue();

            // START RINGING HERE
            // startRinging();      // remove ringing for AI-agent transfers
            
            // Extract call_id and sample_rate from current ws_uri
            std::string call_id_part = ws_uri.substr(ws_uri.find(current_agent_id) + current_agent_id.length() + 1);
            
            // Construct new WebSocket URL with new agent ID
            std::string base_url = ws_uri.substr(0, ws_uri.find("agent=") + 6);
            std::string new_ws_uri = base_url + new_agent_id + "_" + call_id_part;
            
            std::cout << "Transferring to new WebSocket: " << new_ws_uri << std::endl;
            
            // Disconnect from old connection
            if (connected) {
                websocketpp::lib::error_code ec;
                ws_client.close(conn_hdl, websocketpp::close::status::normal, "Agent transfer", ec);
                connected = false;
            }
            
            // Wait for clean disconnect
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            
            // Update URI and agent
            ws_uri = new_ws_uri;
            if (ws_uri.find("rupture") != std::string::npos) {
                REAL_TIME_AUDIO = true;
            }
            current_agent_id = new_agent_id;
            
            // Reset sequence number for new agent
            sequence_number = 0;
            stream_id = "MZ" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
            
            // Connect to new agent
            if (connect(new_ws_uri)) {
                std::cout << "Successfully connected to new agent: " << new_agent_id << std::endl;
                agent_transfer_in_progress = false;  // Reset only after successful connection
                std::cout << "Agent transfer completed successfully" << std::endl;
            } else {
                std::cerr << "Failed to connect to new agent WebSocket" << std::endl;
                stopRinging();  // Stop ringing on failure
                agent_transfer_in_progress = false;
                // Trigger call termination if transfer fails
                if (disconnect_callback) {
                    disconnect_callback();
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error during agent transfer: " << e.what() << std::endl;
            agent_transfer_in_progress = false;
            if (disconnect_callback) {
                disconnect_callback();
            }
        }
    });
    transfer_thread.detach();
}

void WebSocketAudioHandler::performHumanTransfer(const std::string& human_id) {
    std::thread transfer_thread([this, human_id]() {
        try {
            std::cout << "Starting human transfer to: " << human_id << std::endl;
            
            // Clear audio queue before transfer
            clearAudioQueue();

            // START RINGING HERE
            startRinging();

            // Pick the human-transfer target. Default is the legacy rupture server;
            // HUMAN_TRANSFER_WS_URL overrides it (e.g. a local wss:// stub for testing).
            // Must be wss:// — the WebSocket client is TLS-only (asio_tls_client).
            const char* human_env = std::getenv("HUMAN_TRANSFER_WS_URL");
            const bool is_override = (human_env != nullptr && human_env[0] != '\0');
            std::string human_base = is_override ? std::string(human_env)
                                                 : "wss://rupture2.chatwoot.store/ws";

            // Extract call_id from current ws_uri (best effort). The production URI
            // carries it as ...agent=<agentId>_<callId>_... ; a local test URI may not,
            // so when overridden we fall back to a placeholder instead of aborting.
            std::string call_id = is_override ? "local" : "";
            size_t agent_pos = ws_uri.find("agent=");
            if (agent_pos != std::string::npos) {
                std::string agent_value = ws_uri.substr(agent_pos + 6);
                size_t underscore_pos = agent_value.find('_');
                if (underscore_pos != std::string::npos) {
                    // call_id is the second underscore-delimited part after agent_id
                    std::string remaining = agent_value.substr(underscore_pos + 1);
                    size_t next_underscore = remaining.find('_');
                    call_id = remaining.substr(0, next_underscore);
                } else if (!is_override) {
                    std::cerr << "Could not parse call_id from URI for human transfer" << std::endl;
                    agent_transfer_in_progress = false;
                    return;
                }
            } else if (!is_override) {
                std::cerr << "Could not parse agent from URI for human transfer" << std::endl;
                agent_transfer_in_progress = false;
                return;
            }

            // Construct human transfer WebSocket URL (append to any existing query string).
            std::string sep = (human_base.find('?') != std::string::npos) ? "&" : "?";
            std::string human_ws_uri = human_base + sep + "callId=" + call_id +
                                       "&sampleRate=" + std::to_string(sample_rate);

            std::cout << "Transferring to human server: " << human_ws_uri << " with sampleRate: " << sample_rate << std::endl;
            
            // Disconnect from old connection
            if (connected) {
                websocketpp::lib::error_code ec;
                ws_client.close(conn_hdl, websocketpp::close::status::normal, "Human transfer", ec);
                connected = false;
            }
            
            // Wait for clean disconnect
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            // Update URI and agent info
            ws_uri = human_ws_uri;
            // A human transfer is always realtime audio (a local stub URL won't
            // contain "rupture", so set it unconditionally here).
            REAL_TIME_AUDIO = true;
            current_agent_id = "HUMAN_" + human_id;
            
            // Reset sequence number for human transfer
            sequence_number = 0;
            stream_id = "HT" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
            
            // Connect to human transfer server
            if (connect(human_ws_uri)) {
                std::cout << "Successfully connected to human transfer server for: " << human_id << std::endl;
                // Drop everything recorded before this point so the recording
                // contains only the post-transfer (human) audio.
                if (med_port) med_port->discardRecordingSoFar();
                agent_transfer_in_progress = false;  // Reset only after successful connection
                std::cout << "Human transfer completed successfully" << std::endl;
            } else {
                stopRinging();  // Stop ringing on failure
                std::cerr << "Failed to connect to human transfer WebSocket" << std::endl;
                agent_transfer_in_progress = false;
                // Trigger call termination if transfer fails
                if (disconnect_callback) {
                    disconnect_callback();
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error during human transfer: " << e.what() << std::endl;
            agent_transfer_in_progress = false;
            if (disconnect_callback) {
                disconnect_callback();
            }
        }
    });
    transfer_thread.detach();
}

void WebSocketAudioHandler::performHumanNumberTransfer(const std::string& number) {
    std::thread transfer_thread([this, number]() {
        try {
            std::cout << "Starting human (phone) transfer to: " << number << std::endl;

            // Silence the AI and play ringback to the caller while the human's
            // phone rings. The media port keeps pulling these ringing frames until
            // the human leg answers and the bridge detaches this port.
            clearAudioQueue();
            startRinging();

            // Close the AI WebSocket. agent_transfer_in_progress is already true, so
            // the close handler won't tear the SIP call down or try to reconnect.
            if (connected) {
                websocketpp::lib::error_code ec;
                ws_client.close(conn_hdl, websocketpp::close::status::normal, "Human phone transfer", ec);
                connected = false;
            }

            // Hand off to the media port -> CallManager, which dials the human leg
            // through Asterisk and bridges it to this caller once it answers.
            if (med_port) {
                med_port->startHumanNumberTransfer(number);
            } else {
                std::cerr << "Human phone transfer failed: media port not set" << std::endl;
                stopRinging();
                agent_transfer_in_progress = false;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error during human phone transfer: " << e.what() << std::endl;
            stopRinging();
            agent_transfer_in_progress = false;
        }
    });
    transfer_thread.detach();
}

std::vector<uint8_t> WebSocketAudioHandler::resampleAudio(const std::vector<uint8_t>& input_data,
                                                          int input_rate, int output_rate) {
    if (input_rate == output_rate) {
        return input_data;  // No resampling needed
    }

    // Basic input validation 
    if (input_data.empty() || input_data.size() % 2 != 0 || output_rate <= 0) {
        return input_data;  // Return original on invalid input
    }
    
    // Convert input data to int16 samples
    std::vector<int16_t> input_samples(input_data.size() / 2);
    std::memcpy(input_samples.data(), input_data.data(), input_data.size());
    
    // Calculate resampling ratio
    double ratio = static_cast<double>(input_rate) / output_rate;
    size_t output_length = static_cast<size_t>(input_samples.size() / ratio);

    // Prevent zero-length output
    if (output_length == 0) {
        return input_data;
    }
    
    std::vector<int16_t> output_samples(output_length);
    
    // Simple linear interpolation resampling
    for (size_t i = 0; i < output_length; i++) {
        double input_index = i * ratio;
        size_t index_floor = static_cast<size_t>(input_index);

        if (index_floor >= input_samples.size()) {
            std::cerr << "Warning: Resampling index out of bounds: " << index_floor 
                      << " for input size: " << input_samples.size() << std::endl;
            output_samples[i] = 0;  // Out of bounds, fill with zero
            continue;
        }

        size_t index_ceil = std::min(index_floor + 1, input_samples.size() - 1);
        double fraction = input_index - index_floor;
        
        int16_t sample1 = (index_floor < input_samples.size()) ? input_samples[index_floor] : 0;
        int16_t sample2 = (index_ceil < input_samples.size()) ? input_samples[index_ceil] : 0;
        
        output_samples[i] = static_cast<int16_t>(sample1 + (sample2 - sample1) * fraction);
    }
    
    // Convert back to uint8_t vector
    std::vector<uint8_t> output_data(output_samples.size() * 2);
    std::memcpy(output_data.data(), output_samples.data(), output_data.size());
    
    return output_data;
}

void WebSocketAudioHandler::adjustVolume(std::vector<uint8_t>& audio_data, float volume) {
    if (audio_data.empty()) return;

    // Treat volume < 0 as mute (0)
    if (volume < 0.0f) volume = 0.0f;
    
    // If volume is 1.0 (normal), no need to process
    if (std::abs(volume - 1.0f) < 0.001f) return;

    // audio_data contains 16-bit PCM samples (little-endian)
    // We can reinterpret the vector as int16_t array
    int16_t* samples = reinterpret_cast<int16_t*>(audio_data.data());
    size_t num_samples = audio_data.size() / 2;

    for (size_t i = 0; i < num_samples; ++i) {
        float sample_val = static_cast<float>(samples[i]);
        sample_val *= volume;

        // Clamp to int16 range
        if (sample_val > 32767.0f) sample_val = 32767.0f;
        else if (sample_val < -32768.0f) sample_val = -32768.0f;

        samples[i] = static_cast<int16_t>(sample_val);
    }
}

void WebSocketAudioHandler::startRinging() {
    if (ringing_provider->loadRingingFile(sample_rate)) {
        ringing_active = true;
        std::cout << "Started ringing during human transfer" << std::endl;
    } else {
        std::cerr << "Failed to load ringing file" << std::endl;
    }
}

void WebSocketAudioHandler::stopRinging() {
    ringing_active = false;
    std::cout << "Stopped ringing audio" << std::endl;
}

bool WebSocketAudioHandler::isRingingActive() const {
    return ringing_active;
}

void WebSocketAudioHandler::getRingingFrame(std::vector<uint8_t>& frame, size_t frame_size) {
    ringing_provider->getRingingFrame(frame, frame_size);
}
