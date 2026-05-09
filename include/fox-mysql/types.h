#pragma once

#include <chrono>
#include <string>

namespace fox::mysql {

struct ConnectionConfig {
    std::string host = "localhost";
    unsigned int port = 3306;
    std::string user;
    std::string password;
    std::string database;
    std::string charset = "utf8mb4";
    std::chrono::seconds connect_timeout{10};
    std::chrono::seconds read_timeout{30};
    std::chrono::seconds write_timeout{30};
    // 历史字段, 现已是 no-op:
    //   原通过 mysql_options(MYSQL_OPT_RECONNECT) 启用, 但该选项在
    //   MySQL 8.0.34+ 弃用、8.4 移除, 且会打 deprecation warning。
    //   重连改由应用层处理 (Connection 的 stmt 重试路径与 ConnectionPool
    //   的按需建连)。字段保留以避免外部代码出现编译错误。
    bool auto_reconnect = true;
    bool multi_statements = false;
    size_t stmt_cache_capacity = 32;
    
    ConnectionConfig() = default;
    
    ConnectionConfig(const std::string& host, unsigned int port,
                    const std::string& user, const std::string& password,
                    const std::string& database = "")
        : host(host), port(port), user(user), password(password), database(database) {}
};

enum class FieldType {
    DECIMAL,
    TINY,
    SHORT,
    LONG,
    FLOAT,
    DOUBLE,
    NULL_TYPE,
    TIMESTAMP,
    LONGLONG,
    INT24,
    DATE,
    TIME,
    DATETIME,
    YEAR,
    NEWDATE,
    VARCHAR,
    BIT,
    JSON = 245,
    NEWDECIMAL = 246,
    ENUM = 247,
    SET = 248,
    TINY_BLOB = 249,
    MEDIUM_BLOB = 250,
    LONG_BLOB = 251,
    BLOB = 252,
    VAR_STRING = 253,
    STRING = 254,
    GEOMETRY = 255
};

}  // namespace fox::mysql