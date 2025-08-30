#pragma once

#include "yxmysql/types.h"
#include "yxmysql/exception.h"
#include <mysql/mysql.h>
#include <memory>
#include <string>
#include <string_view>

namespace yxmysql {

class ResultSet;

class Connection {
public:
    explicit Connection(const ConnectionConfig& config);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    void connect();
    void close();
    bool is_connected() const noexcept;
    void ping();

    std::unique_ptr<ResultSet> query(std::string_view sql);
    void execute(std::string_view sql);
    
    std::string escape_string(std::string_view str);
    
    unsigned long long affected_rows() const;
    unsigned long long insert_id() const;
    
    void begin_transaction();
    void commit();
    void rollback();
    
    const ConnectionConfig& config() const noexcept { return config_; }

private:
    void check_connection() const;
    void throw_mysql_error(const std::string& context) const;
    
    ConnectionConfig config_;
    MYSQL* mysql_;
    bool connected_;
};

}  // namespace yxmysql