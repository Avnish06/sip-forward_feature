#include "../include/ami_manager.hpp"
#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <netdb.h>

bool AmiManager::connectToAMI(int& sockfd) {
    struct sockaddr_in serv_addr;
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        std::cerr << "[AMI] Error creating socket: " << strerror(errno) << std::endl;
        return false;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(5038);
    
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int status;
    if ((status = getaddrinfo("asterisk", "5038", &hints, &result)) != 0) {
        std::cerr << "[AMI] Failed to resolve hostname: " << gai_strerror(status) << std::endl;
        close(sockfd);
        return false;
    }

    memcpy(&serv_addr, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);

    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags < 0) {
        std::cerr << "[AMI] Failed to get socket flags: " << strerror(errno) << std::endl;
        close(sockfd);
        return false;
    }
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::cerr << "[AMI] Failed to set non-blocking mode: " << strerror(errno) << std::endl;
        close(sockfd);
        return false;
    }

    std::cerr << "[AMI] Attempting to connect to Asterisk..." << std::endl;
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        if (errno != EINPROGRESS) {
            std::cerr << "[AMI] Connection failed: " << strerror(errno) << std::endl;
            close(sockfd);
            return false;
        }

        struct pollfd pfd;
        pfd.fd = sockfd;
        pfd.events = POLLOUT;

        int result = poll(&pfd, 1, 5000);  // 5000ms timeout
        if (result <= 0) {
            std::cerr << "[AMI] Connection timeout or error: " <<
                (result == 0 ? "timeout" : strerror(errno)) << std::endl;
            close(sockfd);
            return false;
        }

        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
            std::cerr << "[AMI] Connection failed after select: " <<
                (error != 0 ? strerror(error) : strerror(errno)) << std::endl;
            close(sockfd);
            return false;
        }
    }

    if (fcntl(sockfd, F_SETFL, flags) < 0) {
        std::cerr << "[AMI] Failed to restore blocking mode: " << strerror(errno) << std::endl;
        close(sockfd);
        return false;
    }

    std::cerr << "[AMI] Connected successfully" << std::endl;
    return true;
}

bool AmiManager::loginToAMI(int sockfd) {
    char buffer[1024] = {0};
    ssize_t bytes_received = recv(sockfd, buffer, sizeof(buffer), 0);
    if (bytes_received <= 0) {
        std::cerr << "[AMI] Failed to receive welcome message: " <<
            (bytes_received == 0 ? "Connection closed" : strerror(errno)) << std::endl;
        return false;
    }
    std::cerr << "[AMI] Welcome message: " << buffer;

    usleep(100000);

    std::string login_cmd = "Action: Login\r\nUsername: admin\r\nSecret: admin\r\n\r\n";
    std::cerr << "[AMI] Sending login command..." << std::endl;
    
    if (send(sockfd, login_cmd.c_str(), login_cmd.length(), 0) != login_cmd.length()) {
        std::cerr << "[AMI] Failed to send login command: " << strerror(errno) << std::endl;
        return false;
    }

    usleep(100000);

    memset(buffer, 0, sizeof(buffer));
    bytes_received = recv(sockfd, buffer, sizeof(buffer), 0);
    if (bytes_received <= 0) {
        std::cerr << "[AMI] Failed to receive login response: " <<
            (bytes_received == 0 ? "Connection closed" : strerror(errno)) << std::endl;
        return false;
    }

    std::string response(buffer);
    std::cerr << "[AMI] Login response: " << response << std::endl;
    
    return response.find("Authentication accepted") != std::string::npos;
}

bool AmiManager::sendAMICommand(int sockfd, const std::string& command) {
    std::string cmd = "Action: Command\r\nCommand: " + command + "\r\n\r\n";
    std::cerr << "[AMI] Sending command: " << command << std::endl;
    
    if (send(sockfd, cmd.c_str(), cmd.length(), 0) != cmd.length()) {
        std::cerr << "[AMI] Failed to send command: " << strerror(errno) << std::endl;
        return false;
    }

    usleep(100000);

    char buffer[4096] = {0};
    std::string full_response;
    int retries = 3;

    while (retries > 0) {
        std::cerr << "[AMI] Waiting for command response (retries left: " << retries << ")..." << std::endl;
        
        ssize_t bytes_received = recv(sockfd, buffer, sizeof(buffer), 0);
        if (bytes_received <= 0) {
            if (bytes_received == 0) {
                std::cerr << "[AMI] Connection closed by peer" << std::endl;
                return false;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::cerr << "[AMI] No more data available" << std::endl;
                break;
            }
            std::cerr << "[AMI] Error receiving data: " << strerror(errno) << std::endl;
            return false;
        }

        full_response.append(buffer, bytes_received);
        
        if (full_response.find("Response: Success") != std::string::npos &&
            full_response.find("Output: Objects found:") != std::string::npos) {
            std::cerr << "[AMI] Received complete response" << std::endl;
            return true;
        }

        memset(buffer, 0, sizeof(buffer));
        retries--;
        usleep(100000);
    }

    std::cerr << "[AMI] Full response received: " << full_response << std::endl;
    return full_response.find("Response: Success") != std::string::npos;
}

bool AmiManager::triggerRegistrationCheck() {
    int sockfd;
    if (!connectToAMI(sockfd)) {
        return false;
    }

    if (!loginToAMI(sockfd)) {
        close(sockfd);
        return false;
    }

    bool reg_result = sendAMICommand(sockfd, "pjsip show registrations");
    
    std::string logoff_cmd = "Action: Logoff\r\n\r\n";
    send(sockfd, logoff_cmd.c_str(), logoff_cmd.length(), 0);
    
    close(sockfd);
    return reg_result;
}

std::string AmiManager::getCallDetailsFromAMI(const std::string& call_id) {
    int sockfd;
    if (!connectToAMI(sockfd)) {
        return "";
    }

    if (!loginToAMI(sockfd)) {
        close(sockfd);
        return "";
    }

    std::string filter_cmd = "Action: Filter\r\n"
                          "Operation: Add\r\n"
                          "Filter: Call-ID = " + call_id + "\r\n\r\n"
                          "Action: Status\r\n"
                          "ActionID: getcalldetails\r\n\r\n";
    send(sockfd, filter_cmd.c_str(), filter_cmd.length(), 0);
    
    char buffer[1024] = {0};
    std::string full_response;
    while (recv(sockfd, buffer, sizeof(buffer), 0) > 0) {
        full_response += buffer;
        if (full_response.find("Event: StatusComplete") != std::string::npos) {
            break;
        }
        memset(buffer, 0, sizeof(buffer));
    }

    std::string logoff_cmd = "Action: Logoff\r\n\r\n";
    send(sockfd, logoff_cmd.c_str(), logoff_cmd.length(), 0);
    
    close(sockfd);
    return full_response;
}