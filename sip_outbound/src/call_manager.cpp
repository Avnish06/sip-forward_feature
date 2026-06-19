#include "../include/call_manager.hpp"
#include "../include/ami_manager.hpp"
#include "../include/db_manager.hpp"
#include "../include/webhook_manager.hpp"
#include <iostream>
#include <fstream>
#include <regex>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <cstdlib>

CallManager::CallManager()
    : http_endpoint(std::make_shared<Pistache::Http::Endpoint>(
          Pistache::Address(Pistache::Ipv4::any(), 
          Pistache::Port(getEnvInt("HTTP_PORT", 8086)))))
{
    // Get environment variables
    std::string public_ip = getEnvString("PUBLIC_IP", "3.108.51.244");
    std::string private_ip = getEnvString("PRIVATE_IP", "0.0.0.0");
    std::string asterisk_ip = getEnvString("ASTERISK_IP", "100.28.249.26");
    int asterisk_port = getEnvInt("ASTERISK_PORT", 5060);
    int sip_port = getEnvInt("SIP_PORT", 5060);
    int rtp_start = getEnvInt("RTP_PORT_START", 4000);
    int rtp_end = getEnvInt("RTP_PORT_END", 4999);
    
    std::string sip_username = getEnvString("SIP_USERNAME", "india_media");
    std::string sip_password = getEnvString("SIP_PASSWORD", "secret123");
    std::string sip_realm = getEnvString("SIP_REALM", "asterisk");
    
    int max_calls = getEnvInt("MAX_CONCURRENT_CALLS", 25);
    MAX_CONCURRENT_CALLS = max_calls;
    
    try {
        std::cerr << "[CallManager] Initializing PJSIP for direct RTP" << std::endl;
        std::cerr << "[CallManager] Public IP: " << public_ip << std::endl;
        std::cerr << "[CallManager] Private IP: " << private_ip << std::endl;
        std::cerr << "[CallManager] RTP Range: " << rtp_start << "-" << rtp_end << std::endl;
        std::cerr << "[CallManager] Asterisk: " << asterisk_ip << ":" << asterisk_port << std::endl;
        std::cerr << "[CallManager] Max concurrent calls: " << max_calls << std::endl;
        
        // Register main thread
        {
            std::lock_guard<std::mutex> lock(thread_mutex);
            auto thread_id = std::this_thread::get_id();
            std::string thread_name = "MainThread";
            auto desc = std::make_unique<ThreadDescriptor>();
            pj_thread_register(thread_name.c_str(), desc->desc, &desc->thread);
            thread_descriptors[thread_id] = std::move(desc);
        }
        
        // Create PJSIP endpoint
        ep = std::make_unique<pj::Endpoint>();
        ep->libCreate();
        
        // Configure logging
        pj::LogConfig log_cfg;
        log_cfg.level = 4;  
        log_cfg.consoleLevel = 4;
        log_cfg.decor = PJ_LOG_HAS_NEWLINE | PJ_LOG_HAS_TIME | PJ_LOG_HAS_MICRO_SEC |
                       PJ_LOG_HAS_SENDER | PJ_LOG_HAS_THREAD_ID;
        
        // Configure endpoint for direct RTP
        pj::EpConfig ep_cfg;
        ep_cfg.logConfig = log_cfg;
        
        // Media configuration (remove problematic port range config)
        ep_cfg.medConfig.hasIoqueue = true;
        ep_cfg.medConfig.clockRate = getEnvInt("SAMPLE_RATE", 8000);
        ep_cfg.medConfig.audioFramePtime = 20;
        ep_cfg.medConfig.noVad = true;
        
        // Explicitly increase the conference bridge size (4x max_calls)
        ep_cfg.medConfig.maxMediaPorts = 4 * max_calls;

        // Increase media worker threads (default is 1)
        ep_cfg.medConfig.threadCnt = 4;

        // Set max calls for this container
        ep_cfg.uaConfig.maxCalls = max_calls;
        
        // Initialize PJSIP library
        ep->libInit(ep_cfg);
        
        // Create UDP transport with correct binding
        std::cerr << "[CallManager] Creating SIP transport on port " << sip_port << std::endl;
        pj::TransportConfig tcfg;
        tcfg.port = sip_port;
        tcfg.boundAddress = private_ip;      // Bind to all interfaces (0.0.0.0)
        tcfg.publicAddress = public_ip;      // Advertise public IP in SDP
        
        ep->transportCreate(PJSIP_TRANSPORT_UDP, tcfg);
        
        // Set null audio device (we handle media ourselves)
        ep->audDevManager().setNullDev();
        
        // Start PJSIP library
        ep->libStart();
        std::cerr << "[CallManager] PJSIP library started successfully" << std::endl;
        
        // Register with Asterisk (dynamic registration with RTP port control)
        registerWithAsterisk(asterisk_ip, asterisk_port, sip_username, 
                           sip_password, sip_realm, public_ip, sip_port);
        
    } catch (const pj::Error& err) {
        std::cerr << "[CallManager] PJSIP initialization error: " << err.info() 
                  << " (code: " << err.status << ")" << std::endl;
        throw std::runtime_error("Failed to initialize PJSIP: " + std::string(err.info()));
    }

    std::cerr << "[CallManager] Initializing WebhookManager..." << std::endl;
    WebhookManager::instance();

    setupRoutes();
}

bool CallManager::addProvider(const std::string& provider_name,
                              const std::string& username,
                              const std::string& password,
                              const std::string& domain,
                              const int port,
                              bool with_registration) {
    bool success = DbManager::instance().addProvider(
        provider_name, username, password, domain, port, with_registration);

    if (success && with_registration) {
        std::cerr << "[addProvider] Provider added with registration, triggering registration check" << std::endl;
        if (!AmiManager::instance().triggerRegistrationCheck()) {
            std::cerr << "[addProvider] Warning: Failed to trigger registration check via AMI" << std::endl;
        }
    }

    return success;
}

bool CallManager::addDid(const std::string& trunk_name,
                         const std::string& did,
                         const std::string& provider_id,
                         const std::string& answer_webhook_url) {
    return DbManager::instance().addDid(trunk_name, did, provider_id, answer_webhook_url);
}

bool CallManager::removeProvider(const std::string& trunk_name) {
    return DbManager::instance().removeProvider(trunk_name);
}

bool CallManager::removeDid(const std::string& did_id) {
    return DbManager::instance().removeDid(did_id);
}

std::vector<SipAccountInfo> CallManager::getRegisteredAccounts() {
    return DbManager::instance().getRegisteredAccounts();
}

void CallManager::registerThread() {
    // Use PJLIB's real per-thread state, NOT an id-keyed cache. std::thread::id values
    // are recycled after a thread terminates, so a fresh OS thread (e.g. each detached
    // human-transfer thread) can reuse a dead thread's id and falsely "hit" the cache
    // while being unregistered with PJLIB. The next pjlib call (Call::xfer, makeCall,
    // hangup, ...) then aborts: "Calling pjlib from unknown/external thread". Checking
    // pj_thread_is_registered() reflects the actual thread, fixing those sporadic aborts.
    if (pj_thread_is_registered()) return;

    std::lock_guard<std::mutex> lock(thread_mutex);
    auto thread_id = std::this_thread::get_id();
    std::string thread_name = "Thread-" + std::to_string(thread_descriptors.size());
    auto desc = std::make_unique<ThreadDescriptor>();
    pj_thread_register(thread_name.c_str(), desc->desc, &desc->thread);
    // Keep the descriptor alive for the thread's lifetime. Keying by thread_id is safe
    // (ids are unique among *live* threads); a recycled id just overwrites the dead
    // thread's now-unused descriptor.
    thread_descriptors[thread_id] = std::move(desc);
}

void CallManager::handleRequest(const Pistache::Rest::Request& request,
                              Pistache::Http::ResponseWriter response,
                              std::function<void(const Pistache::Rest::Request&,
                                               Pistache::Http::ResponseWriter)> handler) {
    try {
        registerThread();
        handler(request, std::move(response));
    } catch (const std::exception& e) {
        response.send(Pistache::Http::Code::Internal_Server_Error,
                     std::string("Internal error: ") + e.what());
    }
}

void CallManager::setupRoutes() {
    using namespace Pistache;

    // Health checking route for the container
    Rest::Routes::Get(router, "/health",
    [this](const Rest::Request& req, Http::ResponseWriter resp) -> Rest::Route::Result {
        handleRequest(req, std::move(resp),
            std::bind(&CallManager::healthCheck, this,
                std::placeholders::_1, std::placeholders::_2));
        return Rest::Route::Result::Ok;
    });

    // Call management routes
    Rest::Routes::Post(router, "/initiate-call",
        [this](const Rest::Request& req, Http::ResponseWriter resp) -> Rest::Route::Result {
            handleRequest(req, std::move(resp),
                std::bind(&CallManager::initiateCall, this,
                    std::placeholders::_1, std::placeholders::_2));
            return Rest::Route::Result::Ok;
        });

    Rest::Routes::Post(router, "/end-call/:callId",
        [this](const Rest::Request& req, Http::ResponseWriter resp) -> Rest::Route::Result {
            handleRequest(req, std::move(resp),
                std::bind(&CallManager::endCall, this,
                    std::placeholders::_1, std::placeholders::_2));
            return Rest::Route::Result::Ok;
        });
    
    // Simple call count route
    Rest::Routes::Get(router, "/get-call-count",
    [this](const Rest::Request& req, Http::ResponseWriter resp) -> Rest::Route::Result {
        handleRequest(req, std::move(resp),
            std::bind(&CallManager::getCallCount, this,
                std::placeholders::_1, std::placeholders::_2));
        return Rest::Route::Result::Ok;
    });

    // Get all calls route
    Rest::Routes::Get(router, "/get-all-calls",
    [this](const Rest::Request& req, Http::ResponseWriter resp) -> Rest::Route::Result {
        handleRequest(req, std::move(resp),
            std::bind(&CallManager::getAllCalls, this,
                std::placeholders::_1, std::placeholders::_2));
        return Rest::Route::Result::Ok;
    });

    // SIP account management routes
    Rest::Routes::Post(router, "/add-provider",
        [this](const Rest::Request& req, Http::ResponseWriter resp) -> Rest::Route::Result {
            handleRequest(req, std::move(resp),
                std::bind(&CallManager::addProviderEndpoint, this,
                    std::placeholders::_1, std::placeholders::_2));
            return Rest::Route::Result::Ok;
        });

    Rest::Routes::Post(router, "/add-did",
        [this](const Rest::Request& req, Http::ResponseWriter resp) -> Rest::Route::Result {
            handleRequest(req, std::move(resp),
                std::bind(&CallManager::addDidEndpoint, this,
                    std::placeholders::_1, std::placeholders::_2));
            return Rest::Route::Result::Ok;
        });

    Rest::Routes::Post(router, "/remove-provider",
        [this](const Rest::Request& req, Http::ResponseWriter resp) -> Rest::Route::Result {
            handleRequest(req, std::move(resp),
                std::bind(&CallManager::removeProviderEndpoint, this,
                    std::placeholders::_1, std::placeholders::_2));
            return Rest::Route::Result::Ok;
        });

    Rest::Routes::Post(router, "/remove-did",
        [this](const Rest::Request& req, Http::ResponseWriter resp) -> Rest::Route::Result {
            handleRequest(req, std::move(resp),
                std::bind(&CallManager::removeDidEndpoint, this,
                    std::placeholders::_1, std::placeholders::_2));
            return Rest::Route::Result::Ok;
        });

    Rest::Routes::Get(router, "/show-accounts",
        [this](const Rest::Request& req, Http::ResponseWriter resp) -> Rest::Route::Result {
            handleRequest(req, std::move(resp),
                std::bind(&CallManager::showAccountsEndpoint, this,
                    std::placeholders::_1, std::placeholders::_2));
            return Rest::Route::Result::Ok;
        });
    
    // Debugging crash endpoint
    Rest::Routes::Post(router, "/crash",
        [this](const Rest::Request& req, Http::ResponseWriter resp) -> Rest::Route::Result {
            handleRequest(req, std::move(resp),
                std::bind(&CallManager::crashEndpoint, this,
                    std::placeholders::_1, std::placeholders::_2));
            return Rest::Route::Result::Ok;
        });
    
    // 16 threads for handling concurrent call requests, SIP account management and health checks heartbeats
    http_endpoint->init(Http::Endpoint::options().threads(16));
    http_endpoint->setHandler(router.handler());
}

CallManager::~CallManager() {
    WebhookManager::instance().stop();
    
    std::lock_guard<std::mutex> lock(endpoints_mutex);

    // Terminate simulated calls
    for (auto& [id, sim] : simulated_calls) {
        sim->terminate();
    }
    simulated_calls.clear();
        
    // Force hangup all active calls first
    std::cerr << "[CallManager] Force hanging up " << active_calls.size() << " active calls" << std::endl;
    for (auto& [call_id, call] : active_calls) {
        try {
            pj::CallOpParam prm;
            prm.statusCode = PJSIP_SC_REQUEST_TERMINATED;
            call->hangup(prm);
        } catch (...) {}
    }
    
    // Wait for calls to cleanup (media ports to stop)
    if (!active_calls.empty()) {
        std::cerr << "[CallManager] Waiting for calls to cleanup..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(5000));
    }

    active_calls.clear();
    
    // Clean up all accounts
    for (auto& [key, endpoint] : endpoints) {
        try {
            if (endpoint->account) {
                std::cerr << "[CallManager] Shutting down account: " << key << std::endl;
                endpoint->account->shutdown();
            }
        } catch (pj::Error& err) {
            std::cerr << "[CallManager] Error cleaning up account: " << err.info() << std::endl;
        }
    }
    endpoints.clear();

    // Extended delay before library destruction to ensure clock threads stop
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));

    // Clean up PJSIP endpoint
    try {
        if (ep) {
            std::cerr << "[CallManager] Destroying PJSIP library" << std::endl;
            ep->libDestroy();
            std::cerr << "[CallManager] PJSIP library destroyed" << std::endl;
        }
    } catch (pj::Error& err) {
        std::cerr << "[CallManager] Error destroying PJSIP library: " << err.info() << std::endl;
    }
}

void CallManager::initiateCall(const Pistache::Rest::Request& request, 
                              Pistache::Http::ResponseWriter response) {
    try {
        std::cerr << "[initiateCall] Processing new call request" << std::endl;

        // Check concurrent call limit
        if (current_calls >= getEnvInt("MAX_CONCURRENT_CALLS", 25)) {
            std::cerr << "[initiateCall] Maximum concurrent calls reached" << std::endl;
            response.send(Pistache::Http::Code::Service_Unavailable, "Maximum concurrent calls reached");
            return;
        }

        // Parse request body
        auto body = nlohmann::json::parse(request.body());
        std::string phone_number = body["phone_number"];
        std::string websocket_url = body["websocket_url"];
        std::string webhook_url = body["webhook_url"];
        std::string trunk_name = body["trunk_name"];
        int sample_rate = body.value("sample_rate", getEnvInt("SAMPLE_RATE", 8000));
        bool directCall = false;
        
        if (websocket_url.find("rupture") != std::string::npos) {
            directCall = true;
        }
        
        // Validate parameters
        if (!isValidPhoneNumber(phone_number) || trunk_name.empty()) {
            response.send(Pistache::Http::Code::Bad_Request, "Invalid or missing parameters");
            return;
        }
        
        // Validate sample rate
        if (sample_rate != 8000 && sample_rate != 16000) {
            response.send(Pistache::Http::Code::Bad_Request, "Invalid sample rate. Must be 8000 or 16000");
            return;
        }

        // Simulated call bypass — no SIP, just connect to WebSocket and stream ringing
        if (trunk_name == "tata_byebye") {
            std::string call_id = generateCallId();
            std::cerr << "[initiateCall] Simulated call for trunk tata_byebye, call_id: " << call_id << std::endl;
            std::cerr << "[initiateCall] Simulated call websocket_url: " << websocket_url << ", call_id: " << call_id << std::endl;

            auto sim = std::make_unique<SimulatedCall>(call_id, websocket_url, webhook_url, sample_rate, trunk_name);
            if (!sim->start()) {
                response.send(Pistache::Http::Code::Internal_Server_Error, "Failed to start simulated call");
                return;
            }

            {
                std::lock_guard<std::mutex> lock(calls_mutex);
                simulated_calls[call_id] = std::move(sim);
                current_calls++;
            }

            nlohmann::json response_body = {
                {"status", "success"},
                {"call_id", call_id},
                {"message", "Simulated call initiated"},
                {"sample_rate", sample_rate},
                {"webhook_url", webhook_url}
            };
            response.send(Pistache::Http::Code::Ok, response_body.dump());
            return;
        }
        
        // Get environment configuration
        std::string asterisk_ip = getEnvString("ASTERISK_IP", "100.28.249.26");
        std::string sip_username = getEnvString("SIP_USERNAME", "india_media");
        std::string public_ip = getEnvString("PUBLIC_IP", "3.108.51.244");
        int http_port = getEnvInt("HTTP_PORT", 8086);
        
        std::string endpoint_key = sip_username + "@" + asterisk_ip;
        std::string call_id = generateCallId();
        
        {
            std::lock_guard<std::mutex> lock(calls_mutex);
            
            // Get registered endpoint
            auto& endpoint_data = endpoints[endpoint_key];
            if (!endpoint_data || !endpoint_data->initialized) {
                throw std::runtime_error("Asterisk endpoint not initialized");
            }
            
            std::cerr << "[initiateCall] Creating call with direct RTP capability" << std::endl;
            std::cerr << "[initiateCall] Sample rate: " << sample_rate << " Hz" << std::endl;
            std::cerr << "[initiateCall] Using trunk: " << trunk_name << std::endl;
            
            // Create call object
            auto call = std::make_unique<MyCall>(*endpoint_data->account, 
                                               websocket_url, call_id, 
                                               PJSUA_INVALID_ID, webhook_url, 
                                               "", "", "", trunk_name, 
                                               std::chrono::system_clock::now(), 
                                               sample_rate, directCall);
            call->endpoint_key = endpoint_key;
            call->string_call_id = call_id;
            
            // Target URI - route through Asterisk for signaling
            std::string target_uri = "sip:" + phone_number + "@" + asterisk_ip;
            
            std::cerr << "[initiateCall] Target URI: " << target_uri << std::endl;
            
            // Configure call parameters for direct RTP
            pj::CallOpParam call_op_param;
            
            // CRITICAL: Add X-Trunk header for Asterisk routing
            pj::SipHeader trunk_hdr;
            trunk_hdr.hName = "X-Trunk";
            trunk_hdr.hValue = trunk_name;
            call_op_param.txOption.headers.push_back(trunk_hdr);
            std::cerr << "[initiateCall] Added X-Trunk header: " << trunk_name << std::endl;
            
            // Carry our internal UUID across both legs so the provider's
            // post-call webhook can be mapped back to string_call_id.
            pj::SipHeader call_uuid_hdr;
            call_uuid_hdr.hName = "X-Call-UUID";
            call_uuid_hdr.hValue = call_id;
            call_op_param.txOption.headers.push_back(call_uuid_hdr);
            
            // Add custom From header
            std::string trunk_did = "919484952308";
            pj::SipHeader from_hdr;
            from_hdr.hName = "From";
            from_hdr.hValue = "\"+" + trunk_did + "\" <sip:+" + trunk_did + "@" + asterisk_ip + ">";
            call_op_param.txOption.headers.push_back(from_hdr);
            
            // Add Contact header with public IP for direct RTP
            std::string trunk_port = "34222";
            pj::SipHeader contact_hdr;
            contact_hdr.hName = "Contact";
            contact_hdr.hValue = "<sip:+" + trunk_did + "@" + public_ip + ":" + trunk_port + ">";
            call_op_param.txOption.headers.push_back(contact_hdr);

            // // Add custom From header
            // pj::SipHeader from_hdr;
            // from_hdr.hName = "From";
            // from_hdr.hValue = "<sip:" + sip_username + "@" + asterisk_ip + ">";
            // call_op_param.txOption.headers.push_back(from_hdr);
            
            // // Add Contact header with public IP for direct RTP
            // pj::SipHeader contact_hdr;
            // contact_hdr.hName = "Contact";
            // contact_hdr.hValue = "<sip:" + sip_username + "@" + public_ip + ":" + 
            //                    getEnvString("SIP_PORT", "5060") + ">";
            // call_op_param.txOption.headers.push_back(contact_hdr);
            // ----------------------------------
            
            // Add User-Agent header
            pj::SipHeader ua_hdr;
            ua_hdr.hName = "User-Agent";
            ua_hdr.hValue = "DirectRTP-MediaServer/1.0";
            call_op_param.txOption.headers.push_back(ua_hdr);
            
            // Make the call through Asterisk
            try {
                call->makeCall(target_uri, call_op_param);
                std::cerr << "[initiateCall] Call initiated successfully" << std::endl;
            } catch (const pj::Error& err) {
                std::cerr << "[initiateCall] Error making call: " << err.info() 
                         << " (code: " << err.status << ")" << std::endl;
                return;
            }

            // Add to active calls
            active_calls[call_id] = std::move(call);
            current_calls++;
            std::cerr << "[initiateCall] Call added to active calls. Total: " << current_calls << std::endl;
        }

        std::string container_id = std::string(sip_username) + "-" + std::string(public_ip) + "-" + std::to_string(http_port);

        // Send success response
        nlohmann::json response_body = {
            {"status", "success"},
            {"call_id", call_id},
            {"message", "Call initiated with direct RTP"},
            {"rtp_range", getEnvString("RTP_PORT_START", "4000") + "-" + getEnvString("RTP_PORT_END", "4999")},
            {"container_id", container_id},
            {"sample_rate", sample_rate},
            {"webhook_url", webhook_url}
        };

        std::cerr << "[initiateCall] Sending success response" << std::endl;
        response.send(Pistache::Http::Code::Ok, response_body.dump());
        
    } catch (const std::exception& e) {
        std::cerr << "[initiateCall] Exception: " << e.what() << std::endl;
        response.send(Pistache::Http::Code::Internal_Server_Error, 
                     std::string("Error initiating call: ") + e.what());
    }
}

void CallManager::transferToHumanNumber(MyCall* caller, const std::string& number) {
    if (!caller) return;

    // This is invoked from the WebSocket handler's transfer thread, which is not a
    // PJSIP-registered thread. makeCall() requires registration.
    registerThread();

    if (!isValidPhoneNumber(number)) {
        std::cerr << "[transferToHuman] Invalid human number: " << number << std::endl;
        return;
    }

    std::string asterisk_ip = getEnvString("ASTERISK_IP", "100.28.249.26");
    std::string public_ip   = getEnvString("PUBLIC_IP", "3.108.51.244");

    // Snapshot what we need off the caller (CallManager is a friend of MyCall).
    std::string endpoint_key = caller->endpoint_key;
    std::string trunk_name   = caller->trunk_name;
    int sample_rate          = caller->sample_rate;

    std::string human_call_id = generateCallId();
    std::cerr << "[transferToHuman] Dialing human " << number << " for caller "
              << caller->string_call_id << " (trunk=" << trunk_name << ")" << std::endl;

    try {
        std::lock_guard<std::mutex> lock(calls_mutex);

        auto& endpoint_data = endpoints[endpoint_key];
        if (!endpoint_data || !endpoint_data->initialized) {
            std::cerr << "[transferToHuman] Endpoint not initialized: " << endpoint_key << std::endl;
            return;
        }

        // Bridge leg: no AI WebSocket, no webhook. ws_url is intentionally empty.
        auto human = std::make_unique<MyCall>(*endpoint_data->account,
                                              "", human_call_id,
                                              PJSUA_INVALID_ID, "",
                                              "", "", "", trunk_name,
                                              std::chrono::system_clock::now(),
                                              sample_rate, true);
        human->endpoint_key = endpoint_key;
        human->string_call_id = human_call_id;
        human->setupAsBridgeLeg(caller);

        std::string target_uri = "sip:" + number + "@" + asterisk_ip;

        // Same routing headers as a normal outbound call so Asterisk picks the trunk.
        pj::CallOpParam op;

        pj::SipHeader trunk_hdr;
        trunk_hdr.hName = "X-Trunk";
        trunk_hdr.hValue = trunk_name;
        op.txOption.headers.push_back(trunk_hdr);

        pj::SipHeader call_uuid_hdr;
        call_uuid_hdr.hName = "X-Call-UUID";
        call_uuid_hdr.hValue = human_call_id;
        op.txOption.headers.push_back(call_uuid_hdr);

        std::string trunk_did = "919484952308";
        pj::SipHeader from_hdr;
        from_hdr.hName = "From";
        from_hdr.hValue = "\"+" + trunk_did + "\" <sip:+" + trunk_did + "@" + asterisk_ip + ">";
        op.txOption.headers.push_back(from_hdr);

        std::string trunk_port = "34222";
        pj::SipHeader contact_hdr;
        contact_hdr.hName = "Contact";
        contact_hdr.hValue = "<sip:+" + trunk_did + "@" + public_ip + ":" + trunk_port + ">";
        op.txOption.headers.push_back(contact_hdr);

        pj::SipHeader ua_hdr;
        ua_hdr.hName = "User-Agent";
        ua_hdr.hValue = "DirectRTP-MediaServer/1.0";
        op.txOption.headers.push_back(ua_hdr);

        try {
            human->makeCall(target_uri, op);
        } catch (const pj::Error& err) {
            std::cerr << "[transferToHuman] makeCall failed: " << err.info() << std::endl;
            return;
        }

        active_calls[human_call_id] = std::move(human);
        current_calls++;
        std::cerr << "[transferToHuman] Human leg initiated. Total calls: " << current_calls << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[transferToHuman] Exception: " << e.what() << std::endl;
    }
}

void CallManager::referCallerToHuman(MyCall* caller, const std::string& number) {
    if (!caller) return;

    // Invoked from the WebSocket transfer thread, which is not PJSIP-registered.
    // xfer()/hangup() require a registered thread.
    registerThread();

    if (!isValidPhoneNumber(number)) {
        std::cerr << "[refer] Invalid human number: " << number << std::endl;
        return;
    }

    const std::string asterisk_ip = getEnvString("ASTERISK_IP", "127.0.0.1");

    // Blind SIP REFER. We do NOT create a second call here. Asterisk receives the
    // REFER on our dialog, pulls the OTHER party in our bridge (the human on the
    // phone) out, dials the Refer-To target itself, bridges the two humans, and
    // then BYEs our leg. For the human<->human call to survive, our leg must be
    // bridged to a real channel in Asterisk (a real trunk call, or a softphone) —
    // a bare dialplan Answer()+Wait() has no party to transfer.
    //
    // "xfer" prefix marks this as a transfer target so the dialplan (_xfer. in the
    // media endpoint's context) routes it out the trunk and bridges the two humans,
    // distinct from a normal number. The trunk is carried on the transferred leg via
    // the inherited REFERPROVIDER channel var (set in [default]), so it isn't encoded
    // here. IMPORTANT: keep the user-part free of pattern-special chars like '*' —
    // Asterisk's extension matcher won't match them, which silently fails the transfer.
    const std::string refer_to = "sip:xfer" + number + "@" + asterisk_ip;

    std::cerr << "[refer] REFER caller " << caller->string_call_id
              << " (trunk=" << caller->trunk_name << ") -> " << refer_to << std::endl;

    try {
        pj::CallOpParam prm;
        caller->xfer(refer_to, prm);   // sends the in-dialog SIP REFER
        std::cerr << "[refer] REFER sent; awaiting Asterisk transfer NOTIFY + BYE." << std::endl;
    } catch (const pj::Error& err) {
        std::cerr << "[refer] xfer (REFER) failed: " << err.info()
                  << " - hanging up caller leg." << std::endl;
        try {
            caller->setHangupSource("human");
            pj::CallOpParam op;
            caller->hangup(op);
        } catch (...) {}
    }
}

void CallManager::endCall(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
    try {
        auto call_id = request.param(":callId").as<std::string>();

        // Check simulated calls first
        {
            std::lock_guard<std::mutex> lock(calls_mutex);
            auto sim_it = simulated_calls.find(call_id);
            if (sim_it != simulated_calls.end()) {
                auto sim = std::move(sim_it->second);
                simulated_calls.erase(sim_it);
                response.send(Pistache::Http::Code::Ok, "Call end initiated");
                std::thread([s = std::move(sim)]() mutable { s->terminate(); }).detach();
                return;
            }
        }
        
        std::unique_ptr<MyCall> call_to_hangup;
        
        {
            std::lock_guard<std::mutex> lock(calls_mutex);
            auto it = active_calls.find(call_id);
            if (it == active_calls.end()) {
                response.send(Pistache::Http::Code::Not_Found, "Call not found");
                return;
            }
            
            // Move the call out of active_calls
            call_to_hangup = std::move(it->second);
            active_calls.erase(it);
        }
        
        // Send response immediately
        response.send(Pistache::Http::Code::Ok, "Call end initiated");
        
        // Handle call termination in a separate thread
        std::thread([call_ptr = std::move(call_to_hangup)]() {
            try {
                // Register thread with PJSIP
                pj_thread_desc thread_desc;
                pj_thread_t *thread_handle;
                
                if (!pj_thread_is_registered()) {
                    pj_bzero(thread_desc, sizeof(thread_desc));
                    if (pj_thread_register("HangupThread", thread_desc, &thread_handle) != PJ_SUCCESS) {
                        return;
                    }
                }

                // Mark hangup as server-initiated
                call_ptr->hangup_source = "server";
                
                // Check if call is still active before sending BYE
                pj::CallInfo call_info;
                try {
                    call_info = call_ptr->getInfo();
                } catch (const pj::Error& err) {
                    std::cerr << "[endCall] Call already invalid: " << err.info() << std::endl;
                    return;  // Call already gone, nothing to do
                } catch (const std::exception& e) {
                    std::cerr << "[endCall] Exception getting call info: " << e.what() << std::endl;
                    return;
                } catch (...) {
                    std::cerr << "[endCall] Unknown error getting call info" << std::endl;
                    return;
                }

                // Let PJSUA pick CANCEL vs BYE based on current invite state.
                // Do NOT prefix with sendRequest({method:"BYE"}) — on a non-CONFIRMED
                // dialog a manual BYE drops inv->ref_cnt to 0, and a later async
                // 401/200 then asserts in pjsip_inv_add_ref (sip_inv.c:288).
                if (call_info.state != PJSIP_INV_STATE_DISCONNECTED) {
                    try {
                        pj::CallOpParam param;
                        call_ptr->hangup(param);
                    } catch (const std::exception& e) {
                        std::cerr << "[endCall] Failed to hangup: " << e.what() << std::endl;
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[endCall] Error in hangup thread: " << e.what() << std::endl;
            }
        }).detach();
        
    } catch (const std::exception& e) {
        response.send(Pistache::Http::Code::Internal_Server_Error,
                     std::string("Error ending call: ") + e.what());
    }
}

bool CallManager::isValidPhoneNumber(const std::string& number) {
    return std::regex_match(number, std::regex("^[0-9]{10,15}$"));
}

std::string CallManager::generateCallId() {
    auto uuid = std::filesystem::path("/proc/sys/kernel/random/uuid");
    std::ifstream uuid_file(uuid);
    std::string uuid_str;
    std::getline(uuid_file, uuid_str);
    return uuid_str;
}

static bool parseYesNoBool(const nlohmann::json& v, bool default_value) {
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        if (s == "yes" || s == "true" || s == "1") return true;
        if (s == "no" || s == "false" || s == "0") return false;
    }
    return default_value;
}

void CallManager::addProviderEndpoint(const Pistache::Rest::Request& request,
                                      Pistache::Http::ResponseWriter response) {
    try {
        const char* asterisk_port = std::getenv("ASTERISK_PORT");
        if (!asterisk_port) asterisk_port = "5060";

        auto body = nlohmann::json::parse(request.body());

        std::string provider_name = body.value("provider_name", std::string());
        std::string username = body["username"];
        std::string password = body["password"];
        std::string domain = body["domain"];
        int port = body.value("port", std::atoi(asterisk_port));
        bool with_registration = false;
        if (body.contains("registration")) {
            with_registration = parseYesNoBool(body["registration"], false);
        } else if (body.contains("register")) {
            with_registration = parseYesNoBool(body["register"], false);
        }

        if (provider_name.empty() || username.empty() || password.empty() || domain.empty()) {
            response.send(Pistache::Http::Code::Bad_Request, "Missing required fields");
            return;
        }

        if (port <= 0 || port > 65535) {
            response.send(Pistache::Http::Code::Bad_Request, "Invalid port number (must be between 1 and 65535)");
            return;
        }

        if (addProvider(provider_name, username, password, domain, port, with_registration)) {
            nlohmann::json response_body = {
                {"status", "success"},
                {"message", "Provider added successfully"},
                {"registration", with_registration}
            };
            response.send(Pistache::Http::Code::Ok, response_body.dump());
        } else {
            response.send(Pistache::Http::Code::Internal_Server_Error,
                        "Failed to add provider");
        }
    } catch (const std::exception& e) {
        response.send(Pistache::Http::Code::Internal_Server_Error,
                     std::string("Error adding provider: ") + e.what());
    }
}

void CallManager::addDidEndpoint(const Pistache::Rest::Request& request,
                                 Pistache::Http::ResponseWriter response) {
    try {
        auto body = nlohmann::json::parse(request.body());

        std::string trunk_name = body["trunk_name"];
        std::string did = body["did"];
        std::string provider_id = body["provider_id"];
        std::string answer_webhook_url = body["answer_webhook_url"];

        if (trunk_name.empty() || did.empty() || provider_id.empty() || answer_webhook_url.empty()) {
            response.send(Pistache::Http::Code::Bad_Request, "Missing required fields");
            return;
        }

        if (addDid(trunk_name, did, provider_id, answer_webhook_url)) {
            nlohmann::json response_body = {
                {"status", "success"},
                {"message", "DID added successfully"}
            };
            response.send(Pistache::Http::Code::Ok, response_body.dump());
        } else {
            response.send(Pistache::Http::Code::Internal_Server_Error,
                        "Failed to add DID");
        }
    } catch (const std::exception& e) {
        response.send(Pistache::Http::Code::Internal_Server_Error,
                     std::string("Error adding DID: ") + e.what());
    }
}

void CallManager::removeProviderEndpoint(const Pistache::Rest::Request& request,
                                         Pistache::Http::ResponseWriter response) {
    try {
        auto body = nlohmann::json::parse(request.body());
        std::string trunk_name = body["trunk_name"];

        if (trunk_name.empty()) {
            response.send(Pistache::Http::Code::Bad_Request, "Missing trunk_name");
            return;
        }

        if (removeProvider(trunk_name)) {
            nlohmann::json response_body = {
                {"status", "success"},
                {"message", "Provider removed successfully"}
            };
            response.send(Pistache::Http::Code::Ok, response_body.dump());
        } else {
            response.send(Pistache::Http::Code::Internal_Server_Error,
                        "Failed to remove provider");
        }
    } catch (const std::exception& e) {
        response.send(Pistache::Http::Code::Internal_Server_Error,
                     std::string("Error removing provider: ") + e.what());
    }
}

void CallManager::removeDidEndpoint(const Pistache::Rest::Request& request,
                                    Pistache::Http::ResponseWriter response) {
    try {
        auto body = nlohmann::json::parse(request.body());
        // The body field is named "provider_id" per the API spec, but it
        // identifies the DID-row's id (= ps_provider_dids.id).
        std::string did_id = body["provider_id"];

        if (did_id.empty()) {
            response.send(Pistache::Http::Code::Bad_Request, "Missing provider_id");
            return;
        }

        if (removeDid(did_id)) {
            nlohmann::json response_body = {
                {"status", "success"},
                {"message", "DID removed successfully"}
            };
            response.send(Pistache::Http::Code::Ok, response_body.dump());
        } else {
            response.send(Pistache::Http::Code::Internal_Server_Error,
                        "Failed to remove DID");
        }
    } catch (const std::exception& e) {
        response.send(Pistache::Http::Code::Internal_Server_Error,
                     std::string("Error removing DID: ") + e.what());
    }
}

void CallManager::showAccountsEndpoint(const Pistache::Rest::Request&,
                                    Pistache::Http::ResponseWriter response) {
    try {
        auto accounts = getRegisteredAccounts();
        nlohmann::json accounts_json = nlohmann::json::array();

        for (const auto& account : accounts) {
            accounts_json.push_back({
                {"trunk_name", account.trunk_name},
                {"username", account.username},
                {"domain", account.domain},
                {"answer_webhook_url", account.answer_webhook_url},
                {"status", account.status}
            });
        }

        nlohmann::json response_body = {
            {"status", "success"},
            {"accounts", accounts_json}
        };

        response.send(Pistache::Http::Code::Ok, response_body.dump());
    } catch (const std::exception& e) {
        response.send(Pistache::Http::Code::Internal_Server_Error,
                     std::string("Error getting accounts: ") + e.what());
    }
}

void CallManager::start() {
    http_endpoint->serve();
}

void CallManager::decrementCallCounter() {
    std::lock_guard<std::mutex> lock(calls_mutex);
    current_calls--;
    std::cout << "Current calls after decrement: " << current_calls << std::endl;
}

void CallManager::incrementAnsweredCallsCount() {
    answered_calls_count++;
    std::cout << "Answered calls count: " << answered_calls_count << std::endl;
}

int CallManager::getAnsweredCallsCount() {
    return answered_calls_count.load();
}

void CallManager::getCallCount(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
    try {
        nlohmann::json count_response = {
            {"answered_calls", answered_calls_count.load()},
            {"current_calls", current_calls.load()},
            {"timestamp", std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()}
        };
        
        response.send(Pistache::Http::Code::Ok, count_response.dump(), 
                     MIME(Application, Json));
    } catch (const std::exception& e) {
        response.send(Pistache::Http::Code::Internal_Server_Error, 
                     std::string("Error: ") + e.what());
    }
}

void CallManager::getAllCalls(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
    try {
        std::lock_guard<std::mutex> lock(calls_mutex);
        nlohmann::json calls_data;
        
        for (const auto& [call_id, call] : active_calls) {
            calls_data[call_id] = {
                {"call_id", call->string_call_id},
                {"agent_id", call->agent_id},
                {"env", call->env},
                {"webhook_call_id", call->webhook_call_id},
                {"trunk_name", call->trunk_name},
                {"start_time", std::chrono::duration_cast<std::chrono::seconds>(
                    call->start_time.time_since_epoch()).count()}
            };
        }

        for (const auto& [call_id, sim] : simulated_calls) {
            calls_data[call_id] = {
                {"call_id", sim->call_id},
                {"trunk_name", sim->trunk_name},
                {"simulated", true},
                {"start_time", std::chrono::duration_cast<std::chrono::seconds>(
                    sim->start_time.time_since_epoch()).count()}
            };
        }
        
        nlohmann::json all_calls_response = {
            {"active_calls", calls_data},
            {"count", active_calls.size() + simulated_calls.size()},
            {"timestamp", std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()}
        };
        
        response.send(Pistache::Http::Code::Ok, all_calls_response.dump(), 
                     MIME(Application, Json));
    } catch (const std::exception& e) {
        response.send(Pistache::Http::Code::Internal_Server_Error, 
                     std::string("Error: ") + e.what());
    }
}

// void CallManager::cleanupCall(const std::string& call_id) {
//     std::cout << "[cleanupCall] Starting cleanup for call: " << call_id << std::endl;
    
//     std::unique_ptr<MyCall> call_to_destroy;
    
//     {
//         std::lock_guard<std::mutex> lock(calls_mutex);
//         std::cout << "[cleanupCall] Lock acquired for call: " << call_id << std::endl;
//         auto it = active_calls.find(call_id);
//         if (it != active_calls.end()) {
//             std::cerr << "[cleanupCall] Removing call " << call_id << " from active_calls" << std::endl;
//             // Move the unique_ptr out of the map, dont erase it directly, as destructor will call cleanupCall again -> cleanupCall re-entry while holding the lock -> deadlock
//             call_to_destroy = std::move(it->second);  // Extract the unique_ptr
//             active_calls.erase(it);  // Remove from map but don't destroy yet
//             std::cerr << "[cleanupCall] Active calls remaining: " << active_calls.size() << std::endl;
//         } else {
//             std::cerr << "[cleanupCall] Call " << call_id << " not found in active_calls (already cleaned up)" << std::endl;
//         }
//         std::cout << "[cleanupCall] Cleanup completed for call: " << call_id << std::endl;
//     }  // Lock is released here
    
//     // Now the unique_ptr destructor runs outside the locked section
//     // This prevents deadlock when MyCall destructor calls cleanupCall again
// }

void CallManager::healthCheck(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
    try {
        nlohmann::json health_response = {
            {"status", "healthy"},
            {"answered_calls", answered_calls_count.load()},
            {"current_calls", current_calls.load()},
            {"max_calls", MAX_CONCURRENT_CALLS},
            {"available_capacity", MAX_CONCURRENT_CALLS - current_calls.load()},
            {"timestamp", std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()}
        };

        std::cerr << "[healthCheck] Calls made: " << answered_calls_count.load() <<  ", Current calls: " << current_calls.load() 
                  << ", Max calls: " << MAX_CONCURRENT_CALLS << std::endl;
        
        // Set response status based on capacity
        if (current_calls >= MAX_CONCURRENT_CALLS) {
            response.send(Pistache::Http::Code::Service_Unavailable, health_response.dump());
        } else {
            response.send(Pistache::Http::Code::Ok, health_response.dump());
        }
    } catch (const std::exception& e) {
        std::cerr << "[healthCheck] Exception occurred: " << e.what() << std::endl;
        nlohmann::json error_response = {
            {"status", "unhealthy"},
            {"error", e.what()}
        };
        response.send(Pistache::Http::Code::Internal_Server_Error, error_response.dump());
    }
}

std::string CallManager::getEnvVar(const char* key) {
    const char* value = std::getenv(key);
    return value ? std::string(value) : "";
}

std::string CallManager::getEnvString(const char* name, const std::string& defaultValue) {
    const char* value = std::getenv(name);
    return (value && *value != '\0') ? std::string(value) : defaultValue;
}

int CallManager::getEnvInt(const char* name, int defaultValue) {
    const char* value = std::getenv(name);
    return (value && *value != '\0') ? std::stoi(value) : defaultValue;
}

void CallManager::registerWithAsterisk(const std::string& asterisk_ip, 
                                     int asterisk_port,
                                     const std::string& username,
                                     const std::string& password, 
                                     const std::string& realm,
                                     const std::string& public_ip,
                                     int sip_port) {
    
    // Get RTP port range from environment (for logging/reference only)
    int rtp_start = getEnvInt("RTP_PORT_START", 4000);
    int rtp_end = getEnvInt("RTP_PORT_END", 4999);
    
    std::string endpoint_key = username + "@" + asterisk_ip;
    auto endpoint_data = std::make_unique<EndpointData>();
    endpoint_data->account = std::make_unique<MyAccount>("");
    
    pj::AccountConfig acc_cfg;
    
    // SIP identity and registration
    acc_cfg.idUri = "sip:" + username + "@" + asterisk_ip;
    acc_cfg.regConfig.registrarUri = "sip:" + asterisk_ip + ":" + std::to_string(asterisk_port);
    acc_cfg.regConfig.registerOnAdd = true;
    acc_cfg.regConfig.retryIntervalSec = 60;
    
    // CRITICAL: Contact header must advertise your public IP for direct RTP
    std::string contact = "sip:" + username + "@" + public_ip + ":" + std::to_string(sip_port);
    acc_cfg.sipConfig.contactForced = contact;
    acc_cfg.sipConfig.contactParams = ";ob";  // Outbound parameter
    
    std::cerr << "[CallManager] Contact URI: " << contact << std::endl;
    std::cerr << "[RTP] Target RTP range: " << rtp_start << "-" << rtp_end << " (firewall controlled)" << std::endl;
    std::cerr << "[RTP] PJSIP will auto-assign RTP ports within firewall range" << std::endl;
    
    // Media configuration for direct RTP
    acc_cfg.mediaConfig.streamKaEnabled = true;  // Keep-alive for RTP
    acc_cfg.mediaConfig.transportConfig.publicAddress = public_ip;  // Advertise public IP in SDP
    acc_cfg.mediaConfig.transportConfig.boundAddress = "0.0.0.0";   // Bind to all interfaces
    acc_cfg.sipConfig.contactForced = contact;
    acc_cfg.mediaConfig.transportConfig.port = rtp_start;   // Starting RTP port (PJSIP will auto-assign within range)
    acc_cfg.mediaConfig.transportConfig.portRange = static_cast<unsigned>(rtp_end - rtp_start + 1); // RTP port range size
    
    // Force direct media negotiation
    acc_cfg.mediaConfig.rtcpMuxEnabled = true;
    acc_cfg.mediaConfig.rtcpXrEnabled = true;
    acc_cfg.mediaConfig.lockCodecEnabled = false;  // Allow codec negotiation
    
    // NAT traversal settings for direct media
    acc_cfg.natConfig.sipStunUse = PJSUA_STUN_USE_DISABLED;
    acc_cfg.natConfig.mediaStunUse = PJSUA_STUN_USE_DISABLED;
    acc_cfg.natConfig.iceEnabled = false;
    acc_cfg.natConfig.contactRewriteUse = true;  // Allow contact rewriting if needed
    
    // Authentication credentials
    pj::AuthCredInfo cred("digest", realm, username, 0, password);
    acc_cfg.sipConfig.authCreds.push_back(cred);
    
    // Create and add account
    endpoint_data->account->create(acc_cfg);
    endpoint_data->initialized = true;
    endpoints[endpoint_key] = std::move(endpoint_data);
    
    std::cerr << "[CallManager] Registered " << username << " with Asterisk for direct RTP" << std::endl;
    std::cerr << "[CallManager] RTP ports controlled by firewall rules (UDP " << rtp_start << "-" << rtp_end << ")" << std::endl;
    std::cerr << "[CallManager] Direct media will be negotiated via re-INVITE after call establishment" << std::endl;
}

void CallManager::crashEndpoint(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
    try {
        nlohmann::json req_body = nlohmann::json::parse(request.body());
        int type = req_body.value("type", 0);
        
        std::cerr << "[crashEndpoint] Crash type: " << type << std::endl;
        
        switch(type) {
            case 0:
                // Normal exit
                std::cerr << "[crashEndpoint] Normal exit" << std::endl;
                response.send(Pistache::Http::Code::Ok, "{\"status\":\"exiting\"}");
                std::exit(0);
                break;
                
            case 1:
                // PJSIP exception
                std::cerr << "[crashEndpoint] PJSIP exception" << std::endl;
                response.send(Pistache::Http::Code::Ok, "{\"status\":\"pjsip_exception\"}");
                throw pj::Error(PJ_EINVAL, "Debug crash", "crashEndpoint", __FILE__, __LINE__);
                break;
                
            case 2:
                // Segmentation fault
                std::cerr << "[crashEndpoint] Segmentation fault" << std::endl;
                response.send(Pistache::Http::Code::Ok, "{\"status\":\"segfault\"}");
                {
                    int* p = nullptr;
                    *p = 42; // Null pointer dereference
                }
                break;
                
            default:
                response.send(Pistache::Http::Code::Bad_Request, "{\"error\":\"Invalid type\"}");
        }
    } catch (const std::exception& e) {
        std::cerr << "[crashEndpoint] Exception: " << e.what() << std::endl;
        response.send(Pistache::Http::Code::Internal_Server_Error, 
                     "{\"error\":\"" + std::string(e.what()) + "\"}");
    }
}