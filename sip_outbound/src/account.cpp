#include "../include/account.hpp"
#include "../include/call_manager.hpp"
#include "../include/db_manager.hpp"
#include <iostream>
#include <string>
#include <algorithm>
#include <cctype>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <regex>

using json = nlohmann::json;

inline std::string trim(const std::string& s) {
    auto start = std::find_if_not(s.begin(), s.end(),
        [](unsigned char ch) { return std::isspace(ch); });

    auto end = std::find_if_not(s.rbegin(), s.rend(),
        [](unsigned char ch) { return std::isspace(ch); }).base();

    if (start >= end) return "";
    return std::string(start, end);
}

// Helper function to clean phone numbers
std::string cleanPhoneNumber(const std::string& number) {
    std::string cleaned = number;
    // Remove whitespace
    cleaned.erase(0, cleaned.find_first_not_of(" \t\r\n"));
    cleaned.erase(cleaned.find_last_not_of(" \t\r\n") + 1);
    // Keep only digits and '+' character
    cleaned.erase(std::remove_if(cleaned.begin(), cleaned.end(),
        [](char c) { return !std::isdigit(c) && c != '+'; }), cleaned.end());
    return cleaned;
}

void MyAccount::onRegState(pj::OnRegStateParam& prm) {
    try {
        // Safety check for valid registration parameter
        if (prm.code == 0) {
            std::cerr << "[Account] Invalid registration parameter, skipping" << std::endl;
            return;
        }
        
        pj::AccountInfo ai = getInfo();
        std::cerr << "[Account] Registration state changed for " << ai.uri << std::endl;
        std::cerr << "[Account] Status: " << (ai.regIsActive ? "Registered" : "Unregistered") << std::endl;
        std::cerr << "[Account] Status code: " << prm.code << std::endl;
        
        // Log registration status details
        if (!ai.regIsActive && prm.code != 200) {
            std::cerr << "[Account] Registration failed - Status code: " << prm.code 
                      << ", Reason: " << prm.reason << std::endl;
            
            // Additional registration state info
            std::cerr << "[Account] Registration details:" << std::endl;
            std::cerr << "[Account] Last error: " << ai.regLastErr << std::endl;
            std::cerr << "[Account] Status text: " << ai.regStatusText << std::endl;
        } else if (ai.regIsActive) {
            std::cerr << "[Account] Registration successful" << std::endl;
            std::cerr << "[Account] Status text: " << ai.regStatusText << std::endl;

            if (prm.code == 200 && !prm.rdata.wholeMsg.empty()) {
                loadBalancer.notifyLoadBalancerWithResponse(prm.rdata.wholeMsg);
            }
        }
    } catch (const pj::Error& err) {
        std::cerr << "[Account] Error in onRegState: " << err.info() 
                  << " (code: " << err.status << ")" << std::endl;
    }
}

// Callback function to handle the response data
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

void MyAccount::onIncomingCall(pj::OnIncomingCallParam& prm) {
    // Flag to ensure we don't try to reject a call that was already successfully moved to CallManager
    bool callSuccessfullyCreated = false;

    try {
        std::cerr << "[Account] Incoming call received..." << std::endl;
        pj::SipRxData rx_data = prm.rdata;
        std::string msg = rx_data.wholeMsg;
        std::cerr << "[Account] Whole SIP Message: " << msg << std::endl;
        
        std::string fromNumber, toNumber, trunkName;

        // --- Header Extraction ---
        // Extract trunk name from X-Trunk header
        size_t trunkPos = msg.find("X-Trunk:");
        if (trunkPos != std::string::npos) {
            size_t trunkEnd = msg.find("\r\n", trunkPos);
            trunkName = msg.substr(trunkPos + 8, trunkEnd - (trunkPos + 8));
            trunkName.erase(0, trunkName.find_first_not_of(" \t\r\n"));
            trunkName.erase(trunkName.find_last_not_of(" \t\r\n") + 1);
            trunkName = trim(trunkName);
            if (!trunkName.empty()) {
                std::cout << "[Account] Trunk Name (from X-Trunk): " << trunkName << std::endl;
            } else {
                std::cerr << "[Account] No X-Trunk header found, using empty trunk name" << std::endl;
                trunkName = "";
            }
        }
        
        // Extract destination number from X-To-Number header
        size_t toNumPos = msg.find("X-To-Number:");
        if (toNumPos != std::string::npos) {
            size_t toNumEnd = msg.find("\r\n", toNumPos);
            toNumber = cleanPhoneNumber(msg.substr(toNumPos + 12, toNumEnd - (toNumPos + 12)));
        } else {
            throw std::runtime_error("Missing X-To-Number header");
        }

        // --- Caller ID Extraction (Simplified for clarity) ---
        size_t paiPos = msg.find("P-Asserted-Identity:");
        if (paiPos != std::string::npos) {
            size_t sipPos = msg.find("sip:", paiPos);
            size_t atPos = msg.find("@", sipPos);
            if (sipPos != std::string::npos && atPos != std::string::npos) {
                fromNumber = cleanPhoneNumber(msg.substr(sipPos + 4, atPos - (sipPos + 4)));
            }
        }

        if (fromNumber.empty() || toNumber.empty()) {
            throw std::runtime_error("Failed to get valid phone numbers");
        }

        std::string call_id = CallManager::generateCallId();
        std::string websocket_url, webhook_url;

        if (trunkName == "test_sipp") {
            // websocket_url = "TEST_SIPP_RINGING";
            websocket_url = "wss://call-in.vocallabs.ai/wstest";
            webhook_url = "";
            std::cout << "[Account] test_sipp trunk detected. Bypassing AI core connection." << std::endl;
        } else {
            // --- CURL Logic ---
            CURL* curl = curl_easy_init(); // Note: curl_global_init should be in main()
            if (curl) {
                json payload = {{"call_id", call_id}, {"from_number", fromNumber}, {"to_number", toNumber}, {"trunk_name", trunkName}};
                std::string jsonStr = payload.dump();
                struct curl_slist* headers = curl_slist_append(NULL, "Content-Type: application/json");
                std::string responseData;
                
                std::string ans_webhook_url = DbManager::instance().getWebhookUrl(toNumber);
                if (ans_webhook_url.empty()) ans_webhook_url = "https://api.vocallabs.ai/sip-answer-webhook";
                std::cout << "[Account] Using Answer Webhook URL: " << ans_webhook_url << std::endl;

                curl_easy_setopt(curl, CURLOPT_URL, ans_webhook_url.c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseData);
                
                CURLcode res = curl_easy_perform(curl);
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);

                if (res == CURLE_OK) {
                    json responseJson = json::parse(responseData);
                    websocket_url = responseJson.value("websocket_url", "");
                    webhook_url = responseJson.value("webhook_url", "");
                    
                    if (websocket_url.empty()) throw std::runtime_error("Empty websocket_url in response");
                } else {
                    throw std::runtime_error("Curl request failed");
                }
            } else {
                throw std::runtime_error("Failed to initialize CURL");
            }
        }

        // --- Success: Instantiate and Answer ---
        auto call = std::make_unique<MyCall>(*this, websocket_url, call_id, prm.callId, webhook_url, "", "", "", trunkName, std::chrono::system_clock::now());
        
        {
            std::lock_guard<std::mutex> lock(CallManager::instance().calls_mutex);
            CallManager::instance().active_calls[call_id] = std::move(call);
            CallManager::instance().current_calls++;
            callSuccessfullyCreated = true; 
        }
        
        pj::CallOpParam call_op_param;
        call_op_param.statusCode = PJSIP_SC_OK;
        CallManager::instance().active_calls[call_id]->answer(call_op_param);

    } catch (const std::exception& e) {
        std::cerr << "[Account] Error: " << e.what() << ". Cleaning up sockets..." << std::endl;
        
        if (!callSuccessfullyCreated) {
            // FIX: Create a temporary call object to send a SIP 500 rejection.
            // This is the ONLY way to ensure PJSIP closes the associated FDs.
            try {
                pj::Call tempCall(*this, prm.callId);
                pj::CallOpParam prm_err;
                prm_err.statusCode = PJSIP_SC_INTERNAL_SERVER_ERROR;
                tempCall.answer(prm_err);
            } catch (...) {
                std::cerr << "[Account] Failed to send SIP 500 rejection." << std::endl;
            }
        }
    }
}