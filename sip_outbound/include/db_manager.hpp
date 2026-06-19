#pragma once

#include <string>
#include <vector>
#include "common_types.hpp"

// Forward declare MYSQL and MYSQL_RES types
typedef struct st_mysql MYSQL;
typedef struct st_mysql_res MYSQL_RES;

class DbManager {
public:
    static DbManager& instance() {
        static DbManager instance;
        return instance;
    }

    bool connectToDatabase();
    void disconnectFromDatabase();
    bool executeSqlQuery(const std::string& query);
    MYSQL_RES* executeSelectQuery(const std::string& query);
    
    // SIP Account Management
    // provider_name is the ps_endpoints.id.
    bool addProvider(const std::string& provider_name,
                     const std::string& username,
                     const std::string& password,
                     const std::string& domain,
                     const int port,
                     bool with_registration);
    // trunk_name is the ps_provider_dids.id (client-controlled DID-row key).
    // provider_id is ps_endpoints.id (FK).
    bool addDid(const std::string& trunk_name,
                const std::string& did,
                const std::string& provider_id,
                const std::string& answer_webhook_url);
    bool removeProvider(const std::string& trunk_name);
    // did_id matches ps_provider_dids.id (single DID-row deletion).
    bool removeDid(const std::string& did_id);
    std::vector<SipAccountInfo> getRegisteredAccounts();
    std::string getWebhookUrl(const std::string& toNumber);

private:
    DbManager();
    ~DbManager();
    DbManager(const DbManager&) = delete;
    DbManager& operator=(const DbManager&) = delete;

    std::string getEnvString(const char* name, const std::string& defaultValue);
    int getEnvInt(const char* name, int defaultValue);
    std::string getEnvVar(const char* key);

    MYSQL* mysql_conn;
};