#pragma once

#include <string>
#include <memory>

class AmiManager {
public:
    static AmiManager& instance() {
        static AmiManager instance;
        return instance;
    }

    bool connectToAMI(int& sockfd);
    bool loginToAMI(int sockfd);
    bool sendAMICommand(int sockfd, const std::string& command);
    bool triggerRegistrationCheck();
    std::string getCallDetailsFromAMI(const std::string& call_id);

private:
    AmiManager() = default;
    ~AmiManager() = default;
    AmiManager(const AmiManager&) = delete;
    AmiManager& operator=(const AmiManager&) = delete;
};