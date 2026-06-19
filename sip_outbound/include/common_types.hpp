#pragma once

#include <string>

struct SipAccountInfo {
    std::string trunk_name;
    std::string username;
    std::string domain;
    std::string answer_webhook_url;
    std::string status;
};