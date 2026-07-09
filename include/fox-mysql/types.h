#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <variant>

namespace fox::mysql {

// 运行时动态参数值, 用于 execute_prepared / query_prepared 的 vector<Param> 重载
// (参数个数在运行时决定的场景: 动态 IN 列表、可选过滤条件、批量插入)。
//
// 类型集合与变参模板版的 bind_param 特化对齐:
//   nullptr_t → MYSQL_TYPE_NULL, int → LONG, long long → LONGLONG,
//   double → DOUBLE, std::string → STRING
// float / const char* / string_view 构造时分别隐式收敛到 double / std::string。
//
// 注意: 无符号整型 (unsigned, size_t) 构造 Param 会因 int/long long/double
// 三者转换级别相同而编译报 ambiguous, 调用方需显式 cast 到上述某个类型。
//
// 默认构造 = nullptr_t = SQL NULL。
using Param = std::variant<std::nullptr_t, int, long long, double, std::string>;

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