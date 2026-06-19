#include "../include/db_manager.hpp"
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <mysql/mysql.h>
// MySQL error codes
#define CR_SERVER_GONE_ERROR 2006
#define CR_SERVER_LOST 2013


DbManager::DbManager() : mysql_conn(nullptr) {
    if (!connectToDatabase()) {
        throw std::runtime_error("Failed to connect to MySQL database");
    }
}

DbManager::~DbManager() {
    disconnectFromDatabase();
}

bool DbManager::connectToDatabase() {
    mysql_conn = mysql_init(nullptr);
    if (!mysql_conn) {
        std::cerr << "[Database] Failed to initialize MySQL connection" << std::endl;
        return false;
    }

    std::string db_host = getEnvString("ASTERISK_IP", "sip-db.vocallabs.ai");
    std::string db_username = getEnvString("MARIA_DB_USERNAME", "asterisk");
    std::string db_password = getEnvString("MARIA_DB_PASS", "1234");
    std::string db_table = getEnvString("MARIA_DB_TABLE", "asterisk");
    int db_port = getEnvInt("MARIA_DB_PORT", 3306);

    std::cout << "DB Host     : " << db_host << std::endl;
    std::cout << "DB Username : " << db_username << std::endl;
    std::cout << "DB Password : " << db_password << std::endl;
    std::cout << "DB Table    : " << db_table << std::endl;
    std::cout << "DB Port     : " << db_port << std::endl;

    if (!mysql_real_connect(mysql_conn, db_host.c_str(), db_username.c_str(), db_password.c_str(),
                      db_table.c_str(), db_port, nullptr, 0)) {
        std::cerr << "[Database] Failed to connect to MySQL: " << mysql_error(mysql_conn) << std::endl;
        mysql_close(mysql_conn);
        mysql_conn = nullptr;
        return false;
    }

    std::cerr << "[Database] Successfully connected to MySQL" << std::endl;
    return true;
}

void DbManager::disconnectFromDatabase() {
    if (mysql_conn) {
        mysql_close(mysql_conn);
        mysql_conn = nullptr;
    }
}

bool DbManager::executeSqlQuery(const std::string& query) {
    // First try to connect if not connected
    if (!mysql_conn) {
        if (!connectToDatabase()) {
            std::cerr << "[Database] Failed to establish database connection" << std::endl;
            return false;
        }
    }

    if (mysql_query(mysql_conn, query.c_str()) != 0) {
        int error_code = mysql_errno(mysql_conn);
        // Check if the error is connection related
        if (error_code == CR_SERVER_GONE_ERROR || error_code == CR_SERVER_LOST) {
            std::cerr << "[Database] Connection lost, attempting to reconnect..." << std::endl;
            // Disconnect and reconnect
            disconnectFromDatabase();
            if (connectToDatabase()) {
                // Retry the query after reconnection
                if (mysql_query(mysql_conn, query.c_str()) == 0) {
                    return true;
                }
            }
            std::cerr << "[Database] Failed to execute query after reconnection" << std::endl;
            return false;
        }
        std::cerr << "[Database] Query failed: " << mysql_error(mysql_conn) << std::endl;
        return false;
    }
    return true;
}

MYSQL_RES* DbManager::executeSelectQuery(const std::string& query) {
    // First try to connect if not connected
    if (!mysql_conn) {
        if (!connectToDatabase()) {
            std::cerr << "[Database] Failed to establish database connection" << std::endl;
            return nullptr;
        }
    }

    if (mysql_query(mysql_conn, query.c_str()) != 0) {
        int error_code = mysql_errno(mysql_conn);
        // Check if the error is connection related
        if (error_code == CR_SERVER_GONE_ERROR || error_code == CR_SERVER_LOST) {
            std::cerr << "[Database] Connection lost, attempting to reconnect..." << std::endl;
            // Disconnect and reconnect
            disconnectFromDatabase();
            if (connectToDatabase()) {
                // Retry the query after reconnection
                if (mysql_query(mysql_conn, query.c_str()) == 0) {
                    return mysql_store_result(mysql_conn);
                }
            }
            std::cerr << "[Database] Failed to execute query after reconnection" << std::endl;
            return nullptr;
        }
        std::cerr << "[Database] Query failed: " << mysql_error(mysql_conn) << std::endl;
        return nullptr;
    }

    return mysql_store_result(mysql_conn);
}

bool DbManager::addProvider(const std::string& provider_name,
                            const std::string& username,
                            const std::string& password,
                            const std::string& domain,
                            const int port,
                            bool with_registration) {

    // Create auth configuration
    std::string auth_realm = "";
    std::string auth_query = "INSERT INTO ps_auths "
        "(id, auth_type, username, password, realm) VALUES "
        "('" + provider_name + "_auth', 'userpass', '" + username + "', '"
        + password + "', '" + auth_realm + "')";

    // Create AOR configuration
    std::string aor_query = "INSERT INTO ps_aors "
        "(id, contact, qualify_frequency, max_contacts, remove_existing) VALUES "
        "('" + provider_name + "_aor', 'sip:" + domain + ":" + std::to_string(port) + "', "
        "60, 1, 'yes')";

    // Create endpoint configuration
    std::string endpoint_query = "INSERT INTO ps_endpoints "
        "(id, transport, aors, context, disallow, allow, direct_media, "
        "force_rport, rewrite_contact, rtp_symmetric, ice_support, from_user, from_domain, outbound_auth, "
        "mailboxes, incoming_mwi_mailbox, mwi_subscribe_replaces_unsolicited, message_context, mwi_from_user, mwi_max_messages, "
        "send_pai, send_rpid, direct_media_method, connected_line_method, timers) VALUES "
        "('" + provider_name + "', 'transport-udp', '" + provider_name + "_aor', "
        "'from-trunk', 'all', 'ulaw', 'yes', 'yes', 'yes', 'yes', 'no', "
        "NULL, '" + domain + "', '" + provider_name + "_auth', '"
        + username + "@" + domain + "', 'sip:" + username + "@" + domain + "', 'yes', "
        "'from-trunk', '" + username + "', 100, "
        "'yes', 'yes', 'reinvite', 'reinvite', 'no')";

    if (!executeSqlQuery(auth_query) ||
        !executeSqlQuery(aor_query) ||
        !executeSqlQuery(endpoint_query)) {
        return false;
    }

    if (with_registration) {
        std::string reg_query = "INSERT INTO ps_registrations "
            "(id, client_uri, contact_user, expiration, retry_interval, "
            "forbidden_retry_interval, max_retries, fatal_retry_interval, server_uri, transport, outbound_auth, "
            "support_path, line, endpoint) VALUES "
            "('" + provider_name + "_registration', "
            "'sip:" + username + "@" + domain + ":" + std::to_string(port) + "', '" + username + "', "
            "3600, 60, 300, 100000, 600, 'sip:" + domain + ":" + std::to_string(port) + "', "
            "'transport-udp', '" + provider_name + "_auth', 'yes', 'yes', '" + provider_name + "')";

        if (!executeSqlQuery(reg_query)) {
            return false;
        }
    }

    return true;
}

bool DbManager::addDid(const std::string& trunk_name,
                       const std::string& did,
                       const std::string& provider_id,
                       const std::string& answer_webhook_url) {
    // trunk_name is the ps_provider_dids.id (client-supplied).
    // provider_id is the FK to ps_endpoints.id.
    std::string did_query =
        "INSERT INTO ps_provider_dids (id, did, provider_id, answer_webhook_url) VALUES ("
        "'" + trunk_name + "', "
        "'" + did + "', "
        "'" + provider_id + "', "
        "'" + answer_webhook_url + "')";

    std::string webhook_query = "INSERT INTO trunk_webhooks "
        "(trunk_id, answer_webhook_url) VALUES "
        "('" + provider_id + "', '" + answer_webhook_url + "') "
        "ON DUPLICATE KEY UPDATE answer_webhook_url = VALUES(answer_webhook_url)";

    if (!executeSqlQuery(did_query) ||
        !executeSqlQuery(webhook_query)) {
        return false;
    }

    return true;
}

bool DbManager::removeProvider(const std::string& trunk_name) {
    // ps_provider_dids has FK to ps_endpoints; delete dependents first.
    std::vector<std::string> queries = {
        "DELETE FROM ps_provider_dids WHERE provider_id = '" + trunk_name + "'",
        "DELETE FROM trunk_webhooks WHERE trunk_id = '" + trunk_name + "'",
        "DELETE FROM ps_registrations WHERE endpoint = '" + trunk_name + "'",
        "DELETE FROM ps_aors WHERE id = '" + trunk_name + "_aor'",
        "DELETE FROM ps_auths WHERE id = '" + trunk_name + "_auth'",
        "DELETE FROM ps_endpoints WHERE id = '" + trunk_name + "'"
    };

    for (const auto& query : queries) {
        if (!executeSqlQuery(query)) {
            return false;
        }
    }

    return true;
}

bool DbManager::removeDid(const std::string& did_id) {
    std::string query =
        "DELETE FROM ps_provider_dids WHERE id = '" + did_id + "'";
    return executeSqlQuery(query);
}

std::vector<SipAccountInfo> DbManager::getRegisteredAccounts() {
    std::vector<SipAccountInfo> accounts;
    
    std::string query =
        "SELECT e.id as trunk_name, e.from_user as username, e.from_domain as domain, "
        "w.answer_webhook_url, "
        "CASE "
        "  WHEN r.id IS NOT NULL AND r.client_uri LIKE CONCAT('sip:', e.from_user, '@', e.from_domain, '%') "
        "  THEN 'registered' "
        "  ELSE 'configured' "
        "END as status "
        "FROM ps_endpoints e "
        "LEFT JOIN trunk_webhooks w ON e.id = w.trunk_id "
        "LEFT JOIN ps_registrations r ON r.endpoint = e.id "
        "WHERE e.id NOT LIKE 'pjsip%'";

    MYSQL_RES* result = executeSelectQuery(query);
    if (!result) {
        return accounts;
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        SipAccountInfo info;
        info.trunk_name = row[0] ? row[0] : "";
        info.username = row[1] ? row[1] : "";
        info.domain = row[2] ? row[2] : "";
        info.answer_webhook_url = row[3] ? row[3] : "";
        info.status = row[4] ? row[4] : "unknown";
        accounts.push_back(info);
    }

    mysql_free_result(result);
    return accounts;
}

std::string DbManager::getWebhookUrl(const std::string& toNumber) {
    std::string query = "SELECT answer_webhook_url FROM ps_provider_dids "
                       "WHERE did = '" + toNumber + "'";
                       
    MYSQL_RES* result = executeSelectQuery(query);
    if (!result) {
        return "";
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    std::string webhook_url = row && row[0] ? row[0] : "";
    
    mysql_free_result(result);
    return webhook_url;
}

std::string DbManager::getEnvVar(const char* key) {
    const char* value = std::getenv(key);
    return value ? std::string(value) : "";
}

std::string DbManager::getEnvString(const char* name, const std::string& defaultValue) {
    const char* value = std::getenv(name);
    return (value && *value != '\0') ? std::string(value) : defaultValue;
}

int DbManager::getEnvInt(const char* name, int defaultValue) {
    const char* value = std::getenv(name);
    return (value && *value != '\0') ? std::stoi(value) : defaultValue;
}
