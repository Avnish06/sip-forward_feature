#include "../include/loadbalancer.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>
#include <chrono>

void LoadBalancer::notifyLoadBalancer(const pj::AccountInfo& ai) {
    try {
        const char* sip_client_name = std::getenv("SIP_USERNAME");
        if (!sip_client_name) sip_client_name = "pjsip_client";

        const char* http_port = std::getenv("HTTP_PORT");
        if (!http_port) http_port = "8086";

        const char* public_ip = std::getenv("PUBLIC_IP");
        if (!public_ip) public_ip = "34.21.21.165";
        
        std::string contact_uri = extractContactUri(ai);
        std::string container_name = std::string(sip_client_name) + "-" + std::string(public_ip) + "-" + std::string(http_port);
        std::string host = std::string(public_ip);
        
        if (contact_uri.empty()) {
            std::cerr << "[Account] Could not extract contact URI" << std::endl;
            return;
        }
        
        // Create JSON payload
        nlohmann::json payload = {
            {"name", container_name},
            {"host", host},
            {"port", std::atoi(http_port)},
            {"aor", std::string(sip_client_name)},
            {"contact_uri", contact_uri}
        };
        
        // Send HTTP POST
        bool success = sendRegistrationRequest(payload.dump());
        
        if (success) {
            std::cerr << "[Account] Successfully registered with load balancer" << std::endl;
        } else {
            std::cerr << "[Account] Failed to register with load balancer" << std::endl;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[Account] Error notifying load balancer: " << e.what() << std::endl;
    }
}

std::string LoadBalancer::extractContactUri(const pj::AccountInfo& ai) {
    // Extract from regStatusText - format: "Contact: <sip:pjsip_client@172.19.0.4:46763>"
    std::string status_text = ai.regStatusText;
    std::cout << "[LoadBalancer] " << status_text << std::endl;
    size_t contact_pos = status_text.find("Contact:");
    if (contact_pos != std::string::npos) {
        size_t start = status_text.find("<", contact_pos);
        size_t end = status_text.find(">", start);
        if (start != std::string::npos && end != std::string::npos) {
            return status_text.substr(start + 1, end - start - 1);
        }
    }
    return "";
}

std::string LoadBalancer::getContainerName() {
    // Use environment variable or hostname
    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    return std::string(hostname);
}

std::string LoadBalancer::getLocalIP() {
    const char* public_ip = std::getenv("PUBLIC_IP");
    if (public_ip && strlen(public_ip) > 0) {
        return std::string(public_ip);
    }
    // Return a fallback IP instead of null pointer
    return "127.0.0.1";
}

bool LoadBalancer::sendRegistrationRequest(const std::string& payload) {
    CURL *curl;
    CURLcode res;
    
    curl = curl_easy_init();
    if(curl) {
        const char* loadbalancer_endpoint = std::getenv("LOADBALANCER_ENDPOINT");
        if (!loadbalancer_endpoint) loadbalancer_endpoint = "http://localhost:8080";
        
        std::string url = std::string(loadbalancer_endpoint) + "/register-container";
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        res = curl_easy_perform(curl);
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        
        return res == CURLE_OK;
    }
    return false;
}

void LoadBalancer::notifyLoadBalancerWithResponse(const std::string& sip_response) {
    try {
        const char* sip_client_name = std::getenv("SIP_USERNAME");
        if (!sip_client_name) sip_client_name = "pjsip_client";

        const char* asterisk_host = std::getenv("ASTERISK_IP");
        if (!asterisk_host) asterisk_host = "asterisk";

        const char* asterisk_port = std::getenv("ASTERISK_PORT");
        if (!asterisk_port) asterisk_port = "5060";

        const char* http_port = std::getenv("HTTP_PORT");
        if (!http_port) http_port = "8086";

        const char* public_ip = std::getenv("PUBLIC_IP");
        if (!public_ip) public_ip = "34.21.21.165";

        std::string contact_uri = extractContactFromSipResponse(sip_response);
        std::string container_name = std::string(sip_client_name) + "-" + std::string(public_ip) + "-" + std::string(http_port);
        std::string host = std::string(public_ip);
        
        if (contact_uri.empty()) {
            contact_uri = "sip:" + std::string(sip_client_name) + "@" + std::string(host) + ":" + std::string(http_port);
            std::cout << "[LoadBalancer] contact uri not found. Making default one: " << contact_uri << std::endl;
        }
        
        nlohmann::json payload = {
            {"name", container_name},
            {"host", host},
            {"port", std::atoi(http_port)},
            {"aor", std::string(sip_client_name)},
            {"contact_uri", contact_uri}
        };

        for (auto& it : payload.items()) std::cout << "[LoadBalancer] Sending " << it.key()  << ": " << it.value()  << std::endl;
        
        bool success = sendRegistrationRequest(payload.dump());
        
        if (success) {
            std::cerr << "[LoadBalancer] Successfully registered with load balancer" << std::endl;
        } else {
            std::cerr << "[LoadBalancer] Failed to register with load balancer" << std::endl;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[LoadBalancer] Error notifying load balancer: " << e.what() << std::endl;
    }
}

std::string LoadBalancer::extractContactFromSipResponse(const std::string& sip_response) {
    const char* sip_client_name = std::getenv("SIP_USERNAME");
    if (!sip_client_name) sip_client_name = "pjsip_client";

    const char* sip_port = std::getenv("SIP_PORT");
    if (!sip_port) sip_port = "5060";

    std::string host = getLocalIP();
    
    std::string contact_uri = "sip:" + std::string(sip_client_name) + "@" + host + ":" + sip_port + ";ob";
    return contact_uri;
}