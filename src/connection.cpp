#include "yx/mysql/connection.h"
#include "yx/mysql/result_set.h"
#include <cstring>

namespace yxmysql {

Connection::Connection(const ConnectionConfig& config)
    : config_(config), mysql_(nullptr), connected_(false) {
    mysql_ = mysql_init(nullptr);
    if (!mysql_) {
        throw ConnectionException("Failed to initialize MySQL connection");
    }
    
    connect();
}

Connection::~Connection() {
    close();
}

Connection::Connection(Connection&& other) noexcept
    : config_(std::move(other.config_)), mysql_(other.mysql_), connected_(other.connected_) {
    other.mysql_ = nullptr;
    other.connected_ = false;
}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        close();
        
        config_ = std::move(other.config_);
        mysql_ = other.mysql_;
        connected_ = other.connected_;
        
        other.mysql_ = nullptr;
        other.connected_ = false;
    }
    return *this;
}

void Connection::connect() {
    if (!mysql_) {
        mysql_ = mysql_init(nullptr);
        if (!mysql_) {
            throw ConnectionException("Failed to initialize MySQL connection");
        }
    }
    
    if (connected_) {
        return;
    }
    
    unsigned int timeout = static_cast<unsigned int>(config_.connect_timeout.count());
    mysql_options(mysql_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    
    timeout = static_cast<unsigned int>(config_.read_timeout.count());
    mysql_options(mysql_, MYSQL_OPT_READ_TIMEOUT, &timeout);
    
    timeout = static_cast<unsigned int>(config_.write_timeout.count());
    mysql_options(mysql_, MYSQL_OPT_WRITE_TIMEOUT, &timeout);
    
    bool reconnect = config_.auto_reconnect;
    mysql_options(mysql_, MYSQL_OPT_RECONNECT, &reconnect);
    
    if (!config_.charset.empty()) {
        mysql_options(mysql_, MYSQL_SET_CHARSET_NAME, config_.charset.c_str());
    }
    
    unsigned long client_flags = 0;
    if (config_.multi_statements) {
        client_flags |= CLIENT_MULTI_STATEMENTS;
    }
    
    const char* database = config_.database.empty() ? nullptr : config_.database.c_str();
    
    if (!mysql_real_connect(mysql_, config_.host.c_str(), config_.user.c_str(),
                           config_.password.c_str(), database, config_.port,
                           nullptr, client_flags)) {
        throw_mysql_error("Failed to connect to MySQL server");
    }
    
    connected_ = true;
}

void Connection::close() {
    if (mysql_) {
        mysql_close(mysql_);
        mysql_ = nullptr;
    }
    connected_ = false;
}

bool Connection::is_connected() const noexcept {
    return connected_ && mysql_;
}

void Connection::ping() {
    check_connection();
    if (mysql_ping(mysql_) != 0) {
        connected_ = false;
        throw_mysql_error("Connection lost, ping failed");
    }
}

std::unique_ptr<ResultSet> Connection::query(std::string_view sql) {
    check_connection();
    
    if (mysql_real_query(mysql_, sql.data(), sql.length()) != 0) {
        throw_mysql_error("Query execution failed");
    }
    
    MYSQL_RES* result = mysql_store_result(mysql_);
    if (!result) {
        if (mysql_field_count(mysql_) == 0) {
            throw QueryException("Query returned no result set");
        } else {
            throw_mysql_error("Failed to retrieve result set");
        }
    }
    
    return std::make_unique<ResultSet>(result);
}

void Connection::execute(std::string_view sql) {
    check_connection();
    
    if (mysql_real_query(mysql_, sql.data(), sql.length()) != 0) {
        throw_mysql_error("Query execution failed");
    }
    
    MYSQL_RES* result = mysql_store_result(mysql_);
    if (result) {
        mysql_free_result(result);
    }
}

std::string Connection::escape_string(std::string_view str) {
    check_connection();
    
    std::string escaped;
    escaped.resize(str.length() * 2 + 1);
    
    unsigned long escaped_length = mysql_real_escape_string(
        mysql_, &escaped[0], str.data(), str.length());
    
    escaped.resize(escaped_length);
    return escaped;
}

unsigned long long Connection::affected_rows() const {
    check_connection();
    return mysql_affected_rows(mysql_);
}

unsigned long long Connection::insert_id() const {
    check_connection();
    return mysql_insert_id(mysql_);
}

void Connection::begin_transaction() {
    execute("BEGIN");
}

void Connection::commit() {
    execute("COMMIT");
}

void Connection::rollback() {
    execute("ROLLBACK");
}

void Connection::check_connection() const {
    if (!is_connected()) {
        throw ConnectionException("Not connected to MySQL server");
    }
}

void Connection::throw_mysql_error(const std::string& context) const {
    if (mysql_) {
        unsigned int error_code = mysql_errno(mysql_);
        const char* error_msg = mysql_error(mysql_);
        throw SQLException(context + ": " + error_msg, error_code);
    } else {
        throw SQLException(context + ": MySQL connection not initialized");
    }
}

}  // namespace yxmysql