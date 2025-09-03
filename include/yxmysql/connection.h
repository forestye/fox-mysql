#pragma once

#include "yxmysql/types.h"
#include "yxmysql/exception.h"
#include <mysql/mysql.h>
#include <memory>
#include <string>
#include <string_view>
#include <deque>
#include <vector>

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
    
    // 预编译SQL接口
    template <typename... Args>
    void execute_prepared(std::string_view sql, Args&&... params);
    
    template <typename... Args>
    std::unique_ptr<ResultSet> query_prepared(std::string_view sql, Args&&... params);
    
    std::string escape_string(std::string_view str);
    
    unsigned long long affected_rows() const;
    unsigned long long insert_id() const;
    
    void begin_transaction();
    void commit();
    void rollback();
    
    const ConnectionConfig& config() const noexcept { return config_; }

private:
    // 预编译语句信息结构
    struct PreparedStmtInfo {
        MYSQL_STMT* stmt;
        unsigned long param_count;
        unsigned long column_count;
        
        PreparedStmtInfo(MYSQL_STMT* s, unsigned long pc, unsigned long cc)
            : stmt(s), param_count(pc), column_count(cc) {}
    };
    
    void check_connection() const;
    void throw_mysql_error(const std::string& context) const;
    void throw_stmt_error(MYSQL_STMT* stmt, const std::string& context) const;
    
    // 预编译语句缓存管理
    MYSQL_STMT* get_or_prepare_stmt(std::string_view sql);
    void clear_stmt_cache();
    std::string make_cache_key(std::string_view sql) const;
    
    // 错误处理
    bool is_recoverable_error(unsigned int error_code) const;
    void handle_stmt_error_with_retry(MYSQL_STMT* stmt, std::string_view sql, const std::string& context);
    
    // 参数绑定
    void bind_parameters(MYSQL_STMT* stmt, std::vector<MYSQL_BIND>& binds);
    template<typename T>
    void bind_param(MYSQL_BIND& bind, T&& value, std::vector<std::string>& string_buffers);
    template<typename... Args>
    void prepare_and_bind_params(MYSQL_STMT* stmt, Args&&... params);
    
    ConnectionConfig config_;
    MYSQL* mysql_;
    bool connected_;
    
    // 预编译语句缓存(FIFO)
    std::deque<std::pair<std::string, PreparedStmtInfo>> stmt_cache_;
    std::string current_database_;
};

}  // namespace yxmysql

#include "yxmysql/connection_prepared.hpp"