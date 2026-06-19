#pragma once
#include <filesystem>
#include <fstream>
#include <pjsua2.hpp>
#include <pistache/endpoint.h>
#include <pistache/http.h>
#include <pistache/router.h>
#include <map>
#include <memory>
#include <atomic>
#include <thread>
#include <mysql/mysql.h>
#include "account.hpp"
#include "call.hpp"
#include "simulated_call.hpp"
#include "common_types.hpp"

struct ThreadDescriptor {
    pj_thread_desc desc;
    pj_thread_t* thread;
    ThreadDescriptor() {
        std::memset(desc, 0, sizeof(desc));
    }
};

// Define EndpointData before CallManager class
struct EndpointData {
    std::unique_ptr<MyAccount> account;
    std::atomic<bool> initialized{false};
    std::string answer_webhook_url;
};

class CallManager {
    friend class MyAccount;  // Allow MyAccount to access private members
public:
    static CallManager& instance() {
        static CallManager instance;
        return instance;
    }
    
    // Delete copy constructor and assignment operator
    CallManager(const CallManager&) = delete;
    CallManager& operator=(const CallManager&) = delete;

private:
    CallManager();  // Make constructor private
    std::unique_ptr<pj::Endpoint> ep;  // PJSIP endpoint instance
    std::map<std::string, std::unique_ptr<EndpointData>> endpoints;  // key: username+domain
    std::map<std::string, std::unique_ptr<MyCall>> active_calls;     // key: call_id
    std::map<std::string, std::unique_ptr<SimulatedCall>> simulated_calls;  // key: call_id
    std::mutex endpoints_mutex;
    std::mutex calls_mutex;
    std::atomic<int> current_calls{0};
    std::atomic<int> answered_calls_count{0};  // Simple counter for answered calls
    int MAX_CONCURRENT_CALLS = 25;
    std::shared_ptr<Pistache::Http::Endpoint> http_endpoint;
    Pistache::Rest::Router router;
    std::map<std::thread::id, std::unique_ptr<ThreadDescriptor>> thread_descriptors;
    std::mutex thread_mutex;
    MYSQL* mysql_conn;

    // Database methods
    bool connectToDatabase();
    void disconnectFromDatabase();
    bool executeSqlQuery(const std::string& query);
    MYSQL_RES* executeSelectQuery(const std::string& query);

    // AMI methods
    bool connectToAMI(int& sockfd);
    bool loginToAMI(int sockfd);
    bool sendAMICommand(int sockfd, const std::string& command);
    bool triggerRegistrationCheck();

    // SIP account management methods
    bool addProvider(const std::string& provider_name,
                     const std::string& username,
                     const std::string& password,
                     const std::string& domain,
                     const int port,
                     bool with_registration);
    bool addDid(const std::string& trunk_name,
                const std::string& did,
                const std::string& provider_id,
                const std::string& answer_webhook_url);
    bool removeProvider(const std::string& trunk_name);
    bool removeDid(const std::string& did_id);
    std::vector<SipAccountInfo> getRegisteredAccounts();
    
    // Environment variable helpers
    std::string getEnvString(const char* name, const std::string& defaultValue);
    int getEnvInt(const char* name, int defaultValue);
    std::string getEnvVar(const char* key);
    
    // Asterisk registration with RTP port control
    void registerWithAsterisk(const std::string& asterisk_ip, 
                             int asterisk_port,
                             const std::string& username,
                             const std::string& password, 
                             const std::string& realm,
                             const std::string& public_ip,
                             int sip_port);

    // Private methods
    void setupRoutes();
    bool isValidPhoneNumber(const std::string& number);
    static std::string generateCallId();
    void registerThread();
    void handleRequest(const Pistache::Rest::Request& request,
                      Pistache::Http::ResponseWriter response,
                      std::function<void(const Pistache::Rest::Request&,
                                       Pistache::Http::ResponseWriter)> handler);

public:
    ~CallManager();
    void start();
    void decrementCallCounter();  // Public method to decrement call counter
    // void cleanupCall(const std::string& call_id);  // Public method to cleanup call resources
    void incrementAnsweredCallsCount();  // Simple method to increment answered calls
    int getAnsweredCallsCount();  // Simple getter for answered calls count
    
    // Health check endpoint
    void healthCheck(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response);

    // API endpoints
    void initiateCall(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response);
    void endCall(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response);

    // Human transfer: place a new outbound leg to `number` via Asterisk and bridge
    // it to the live `caller`. Safe to call from any thread (registers with PJSIP).
    // DEPRECATED for humanNumber transfers — kept for reference. The humanNumber
    // path now uses referCallerToHuman() (SIP REFER) so no second leg is created.
    void transferToHumanNumber(MyCall* caller, const std::string& number);

    // Human transfer via SIP REFER (blind transfer). Creates NO new call leg:
    // sends one in-dialog REFER on `caller`'s dialog so Asterisk moves the human
    // party to `number`, joins the two humans, and BYEs our leg. Safe to call from
    // any thread (registers with PJSIP).
    void referCallerToHuman(MyCall* caller, const std::string& number);
    void getCallCount(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response);
    void getAllCalls(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response);

    // SIP account management endpoints
    void addProviderEndpoint(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response);
    void addDidEndpoint(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response);
    void removeProviderEndpoint(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response);
    void removeDidEndpoint(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response);
    void showAccountsEndpoint(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response);
    
    // Debugging endpoint
    void crashEndpoint(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response);
};
