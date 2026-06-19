#pragma once

#include <string>
#include <pjsua2.hpp>

class LoadBalancer {
public:
    void notifyLoadBalancer(const pj::AccountInfo& ai);
    std::string extractContactUri(const pj::AccountInfo& ai);
    std::string getContainerName();
    std::string getLocalIP();
    bool sendRegistrationRequest(const std::string& payload);
    void notifyLoadBalancerWithResponse(const std::string& sip_response);
    std::string extractContactFromSipResponse(const std::string& sip_response);

    LoadBalancer() = default;
    ~LoadBalancer() = default;
    LoadBalancer(const LoadBalancer&) = delete;
    LoadBalancer& operator=(const LoadBalancer&) = delete;
};