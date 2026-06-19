#pragma once

#include <pjsua2.hpp>
#include "loadbalancer.hpp"
#include <string>
#include <iostream>

// Forward declare CallManager to avoid circular dependency
class CallManager;

class MyAccount : public pj::Account {
public:
    MyAccount()
        : pj::Account()
        , answer_webhook_url{}
        , loadBalancer() {
        std::cerr << "[Account] Creating new account instance" << std::endl;
        std::cerr << "[Account] Creating loadbalancing context" << std::endl;
    }
    MyAccount(const std::string& answer_webhook)
        : pj::Account()
        , answer_webhook_url(answer_webhook)
        , loadBalancer() {
        std::cerr << "[Account] Creating new account instance" << std::endl;
        std::cerr << "[Account] Creating loadbalancing context" << std::endl;
    }

    void create(const pj::AccountConfig &cfg) {
        // Use the initialized endpoint instance
        Account::create(cfg);
    }

    virtual ~MyAccount() {
        std::cerr << "[Account] Destroying account instance" << std::endl;
        try {
            if (isValid()) {
                std::cerr << "[Account] Account is valid, shutting down..." << std::endl;
                shutdown();
                std::cerr << "[Account] Account shutdown complete" << std::endl;
            }
        } catch (const pj::Error& err) {
            std::cerr << "[Account] Error during account cleanup: " << err.info() 
                      << " (code: " << err.status << ")" << std::endl;
        }
    }

    virtual void onRegState(pj::OnRegStateParam& /*prm*/);
    virtual void onIncomingCall(pj::OnIncomingCallParam& prm);

    std::string answer_webhook_url;
    LoadBalancer loadBalancer;
};
