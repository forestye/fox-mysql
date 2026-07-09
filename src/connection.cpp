#include "fox-mysql/connection.h"
#include "fox-mysql/result_set.h"
#include <cstring>
#include <algorithm>
#include <cctype>

// MySQL错误码常量
#ifndef ER_UNSUPPORTED_PS
#define ER_UNSUPPORTED_PS 1295
#endif

#ifndef CR_SERVER_GONE_ERROR
#define CR_SERVER_GONE_ERROR 2006
#endif

#ifndef CR_SERVER_LOST
#define CR_SERVER_LOST 2013
#endif

#ifndef ER_UNKNOWN_STMT_HANDLER
#define ER_UNKNOWN_STMT_HANDLER 1243
#endif

#ifndef ER_NEED_REPREPARE
#define ER_NEED_REPREPARE 1615
#endif

namespace fox::mysql {

Connection::Connection(const ConnectionConfig& config)
    : config_(config), mysql_(nullptr), connected_(false), current_database_(config.database) {
    mysql_ = mysql_init(nullptr);
    if (!mysql_) {
        throw ConnectionException("Failed to initialize MySQL connection");
    }
    
    connect();
}

Connection::~Connection() {
    clear_stmt_cache();
    close();
}

Connection::Connection(Connection&& other) noexcept
    : config_(std::move(other.config_)), mysql_(other.mysql_), connected_(other.connected_),
      stmt_cache_(std::move(other.stmt_cache_)), current_database_(std::move(other.current_database_)) {
    other.mysql_ = nullptr;
    other.connected_ = false;
}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        clear_stmt_cache();
        close();
        
        config_ = std::move(other.config_);
        mysql_ = other.mysql_;
        connected_ = other.connected_;
        stmt_cache_ = std::move(other.stmt_cache_);
        current_database_ = std::move(other.current_database_);
        
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
    
    // 不再调用 mysql_options(MYSQL_OPT_RECONNECT, ...):
    //   - MySQL 8.0.34+ 弃用并在每次设置时打印 warning, 8.4 直接移除该选项；
    //   - libmysql 层静默重连会丢失会话状态(事务/临时表/prepared stmt/锁等),
    //     被官方视为 footgun, 推荐由应用层处理重连；
    //   - 本库已有应用层重试 (Connection::handle_stmt_error_with_retry +
    //     ConnectionPool::acquire 按需建连), 这条选项实际冗余。
    // ConnectionConfig::auto_reconnect 字段保留作为源码兼容, 但已是 no-op。

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
    current_database_ = config_.database;
    
    // 重连后清空预编译语句缓存
    if (!stmt_cache_.empty()) {
        clear_stmt_cache();
    }
}

void Connection::close() {
    clear_stmt_cache();
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
        // ping 失败必然是连接级问题 (server 关掉了 idle 连接 / 网络断开 / wait_timeout),
        // 抛 ConnectionException 而不是基类 SQLException, 以便 pool 的健康检查
        // 路径能精准识别并走重连分支。
        unsigned int code = mysql_errno(mysql_);
        std::string msg = std::string("Connection lost, ping failed: ") + mysql_error(mysql_);
        connected_ = false;
        throw ConnectionException(msg, code);
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

void Connection::throw_stmt_error(MYSQL_STMT* stmt, const std::string& context) const {
    if (stmt) {
        unsigned int error_code = mysql_stmt_errno(stmt);
        const char* error_msg = mysql_stmt_error(stmt);
        throw SQLException(context + ": " + error_msg, error_code);
    } else {
        throw SQLException(context + ": Statement not initialized");
    }
}

std::string Connection::make_cache_key(std::string_view sql) const {
    // 缓存key格式: db_name + '\n' + trim(sql)
    std::string key = current_database_;
    key += '\n';
    
    // 去首尾空白
    size_t start = 0;
    size_t end = sql.length();
    
    while (start < end && std::isspace(sql[start])) {
        ++start;
    }
    while (end > start && std::isspace(sql[end - 1])) {
        --end;
    }
    
    key.append(sql.substr(start, end - start));
    return key;
}

void Connection::clear_stmt_cache() {
    for (auto& cache_entry : stmt_cache_) {
        if (cache_entry.second.stmt) {
            mysql_stmt_close(cache_entry.second.stmt);
        }
    }
    stmt_cache_.clear();
}

MYSQL_STMT* Connection::get_or_prepare_stmt(std::string_view sql) {
    check_connection();
    
    std::string key = make_cache_key(sql);
    
    // 查找缓存
    auto it = std::find_if(stmt_cache_.begin(), stmt_cache_.end(),
        [&key](const auto& entry) { return entry.first == key; });
    
    if (it != stmt_cache_.end()) {
        // 缓存命中，移到队尾(LRU行为，虽然文档说FIFO，但这样更高效)
        auto cache_entry = std::move(*it);
        stmt_cache_.erase(it);
        stmt_cache_.push_back(std::move(cache_entry));
        return stmt_cache_.back().second.stmt;
    }
    
    // 缓存未命中，准备新语句
    MYSQL_STMT* stmt = mysql_stmt_init(mysql_);
    if (!stmt) {
        throw_mysql_error("Failed to initialize prepared statement");
    }
    
    if (mysql_stmt_prepare(stmt, sql.data(), sql.length()) != 0) {
        unsigned int error_code = mysql_stmt_errno(stmt);
        mysql_stmt_close(stmt);
        
        // 检查是否是不支持prepare的语句
        if (error_code == ER_UNSUPPORTED_PS) {
            throw SQLException("Statement does not support prepared execution: " + 
                std::string(mysql_stmt_error(stmt)), error_code);
        }
        
        throw_stmt_error(stmt, "Failed to prepare statement");
    }
    
    unsigned long param_count = mysql_stmt_param_count(stmt);
    unsigned long column_count = mysql_stmt_field_count(stmt);
    
    // 检查缓存容量，如果满了则删除最旧的
    if (stmt_cache_.size() >= config_.stmt_cache_capacity) {
        if (stmt_cache_.front().second.stmt) {
            mysql_stmt_close(stmt_cache_.front().second.stmt);
        }
        stmt_cache_.pop_front();
    }
    
    // 添加到缓存
    stmt_cache_.emplace_back(key, PreparedStmtInfo(stmt, param_count, column_count));
    
    return stmt;
}

bool Connection::is_recoverable_error(unsigned int error_code) const {
    return error_code == CR_SERVER_GONE_ERROR ||
           error_code == CR_SERVER_LOST ||
           error_code == ER_UNKNOWN_STMT_HANDLER ||
           error_code == ER_NEED_REPREPARE;
}

void Connection::handle_stmt_error_with_retry(MYSQL_STMT* stmt, std::string_view sql, const std::string& context) {
    unsigned int error_code = mysql_stmt_errno(stmt);
    
    if (is_recoverable_error(error_code)) {
        // 清空缓存并重连
        clear_stmt_cache();
        
        try {
            if (mysql_ping(mysql_) != 0) {
                // 尝试重连
                connected_ = false;
                connect();
            }
            
            // 重新准备语句并执行一次
            MYSQL_STMT* new_stmt = mysql_stmt_init(mysql_);
            if (!new_stmt) {
                throw_mysql_error("Failed to reinitialize prepared statement after recovery");
            }
            
            if (mysql_stmt_prepare(new_stmt, sql.data(), sql.length()) != 0) {
                mysql_stmt_close(new_stmt);
                throw_stmt_error(new_stmt, "Failed to reprepare statement after recovery");
            }
            
            // 注意：这里只是重新准备了语句，实际执行需要在调用方处理
            mysql_stmt_close(new_stmt);
        } catch (const std::exception&) {
            // 重试失败，抛出原始错误
            throw_stmt_error(stmt, context);
        }
    } else {
        // 不可恢复的错误，直接抛出
        throw_stmt_error(stmt, context);
    }
}

void Connection::bind_parameters(MYSQL_STMT* stmt, std::vector<MYSQL_BIND>& binds) {
    if (binds.empty()) {
        return;
    }

    if (mysql_stmt_bind_param(stmt, binds.data()) != 0) {
        throw_stmt_error(stmt, "Failed to bind parameters");
    }
}

void Connection::bind_params_runtime(MYSQL_STMT* stmt, const std::vector<Param>& params) {
    const size_t expected = mysql_stmt_param_count(stmt);
    if (params.size() != expected) {
        // 动态拼接 SQL 时占位符个数和参数个数对不上是高频 bug,
        // 报错带上两边实际个数便于排查 (与变参模板版的措辞保持一致)。
        throw SQLException("Parameter count mismatch: expected " +
            std::to_string(expected) + ", got " + std::to_string(params.size()));
    }

    if (params.empty()) {
        return;  // 空 vector 合法, 等价于无参调用
    }

    // 缓冲区管理与 prepare_and_bind_params 完全一致: reserve 到参数个数,
    // 保证 emplace_back 不触发重分配, bind 里保存的 buffer 指针才稳定。
    param_binds_.resize(params.size());
    param_string_buffers_.clear();
    param_string_buffers_.reserve(params.size());
    param_scalar_buffers_.clear();
    param_scalar_buffers_.reserve(params.size());
    std::memset(param_binds_.data(), 0, sizeof(MYSQL_BIND) * params.size());

    for (size_t i = 0; i < params.size(); ++i) {
        std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
                bind_param(param_binds_[i], nullptr, param_string_buffers_);
            } else if constexpr (std::is_same_v<T, std::string>) {
                // 接口收 const&, 不能移走调用方的字符串; 走 string_view 特化
                // 拷贝一次进 param_string_buffers_。
                bind_param(param_binds_[i], std::string_view(v), param_string_buffers_);
            } else {
                // int / long long / double: 拷贝标量, 走对应特化占槽。
                bind_param(param_binds_[i], T(v), param_string_buffers_);
            }
        }, params[i]);
    }

    bind_parameters(stmt, param_binds_);
}

void Connection::execute_prepared(std::string_view sql, const std::vector<Param>& params) {
    execute_prepared_with(sql, [&](MYSQL_STMT* stmt) {
        bind_params_runtime(stmt, params);
    });
}

std::unique_ptr<ResultSet> Connection::query_prepared(std::string_view sql, const std::vector<Param>& params) {
    return query_prepared_with(sql, [&](MYSQL_STMT* stmt) {
        bind_params_runtime(stmt, params);
    });
}

// Variadic template implementations live in connection_prepared.hpp

}  // namespace fox::mysql