#pragma once

#include "yxmysql/connection.h"
#include "yxmysql/result_set.h"
#include <cstring>

namespace yxmysql {

template <typename... Args>
void Connection::execute_prepared(std::string_view sql, Args&&... params) {
    bool retry_attempted = false;
    
    while (true) {
        try {
            MYSQL_STMT* stmt = get_or_prepare_stmt(sql);
            
            // 重置语句状态
            if (mysql_stmt_reset(stmt) != 0) {
                unsigned int error_code = mysql_stmt_errno(stmt);
                if (!retry_attempted && is_recoverable_error(error_code)) {
                    handle_stmt_error_with_retry(stmt, sql, "Failed to reset prepared statement");
                    retry_attempted = true;
                    continue;
                }
                throw_stmt_error(stmt, "Failed to reset prepared statement");
            }
            
            // 绑定参数
            prepare_and_bind_params(stmt, std::forward<Args>(params)...);
            
            // 执行语句
            if (mysql_stmt_execute(stmt) != 0) {
                unsigned int error_code = mysql_stmt_errno(stmt);
                if (!retry_attempted && is_recoverable_error(error_code)) {
                    handle_stmt_error_with_retry(stmt, sql, "Failed to execute prepared statement");
                    retry_attempted = true;
                    continue;
                }
                throw_stmt_error(stmt, "Failed to execute prepared statement");
            }
            
            // 对于非查询语句，释放结果(如果有)
            if (mysql_stmt_field_count(stmt) == 0) {
                // 这是非查询语句(INSERT/UPDATE/DELETE)
                mysql_stmt_free_result(stmt);
            } else {
                // 这是查询语句但在execute_prepared中调用，释放结果
                mysql_stmt_free_result(stmt);
            }
            
            break; // 成功执行，退出重试循环
            
        } catch (const SQLException& e) {
            if (retry_attempted) {
                throw; // 已经重试过，重新抛出异常
            }
            throw; // 非可恢复异常，直接抛出
        }
    }
}

template <typename... Args>
std::unique_ptr<ResultSet> Connection::query_prepared(std::string_view sql, Args&&... params) {
    bool retry_attempted = false;
    
    while (true) {
        try {
            MYSQL_STMT* stmt = get_or_prepare_stmt(sql);
            
            // 重置语句状态
            if (mysql_stmt_reset(stmt) != 0) {
                unsigned int error_code = mysql_stmt_errno(stmt);
                if (!retry_attempted && is_recoverable_error(error_code)) {
                    handle_stmt_error_with_retry(stmt, sql, "Failed to reset prepared statement");
                    retry_attempted = true;
                    continue;
                }
                throw_stmt_error(stmt, "Failed to reset prepared statement");
            }
            
            // 绑定参数
            prepare_and_bind_params(stmt, std::forward<Args>(params)...);
            
            // 执行语句
            if (mysql_stmt_execute(stmt) != 0) {
                unsigned int error_code = mysql_stmt_errno(stmt);
                if (!retry_attempted && is_recoverable_error(error_code)) {
                    handle_stmt_error_with_retry(stmt, sql, "Failed to execute prepared statement");
                    retry_attempted = true;
                    continue;
                }
                throw_stmt_error(stmt, "Failed to execute prepared statement");
            }
            
            // 检查是否为查询语句
            if (mysql_stmt_field_count(stmt) == 0) {
                throw QueryException("Statement does not return a result set");
            }
            
            // 设置属性以获取正确的列长度
            bool update_max_length = true;
            if (mysql_stmt_attr_set(stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &update_max_length) != 0) {
                throw_stmt_error(stmt, "Failed to set statement attribute");
            }
            
            // 存储结果
            if (mysql_stmt_store_result(stmt) != 0) {
                unsigned int error_code = mysql_stmt_errno(stmt);
                if (!retry_attempted && is_recoverable_error(error_code)) {
                    handle_stmt_error_with_retry(stmt, sql, "Failed to store prepared statement result");
                    retry_attempted = true;
                    continue;
                }
                throw_stmt_error(stmt, "Failed to store prepared statement result");
            }
            
            // 创建并返回ResultSet
            return std::make_unique<ResultSet>(stmt);
            
        } catch (const SQLException& e) {
            if (retry_attempted) {
                throw; // 已经重试过，重新抛出异常
            }
            throw; // 非可恢复异常，直接抛出
        }
    }
}

// 参数绑定的特化实现
template<>
inline void Connection::bind_param<int>(MYSQL_BIND& bind, int&& value, std::vector<std::string>& /* string_buffers */) {
    static thread_local int temp_int;
    temp_int = value;
    
    bind.buffer_type = MYSQL_TYPE_LONG;
    bind.buffer = &temp_int;
    bind.buffer_length = sizeof(int);
    bind.is_null = nullptr;
}

template<>
inline void Connection::bind_param<long long>(MYSQL_BIND& bind, long long&& value, std::vector<std::string>& /* string_buffers */) {
    static thread_local long long temp_longlong;
    temp_longlong = value;
    
    bind.buffer_type = MYSQL_TYPE_LONGLONG;
    bind.buffer = &temp_longlong;
    bind.buffer_length = sizeof(long long);
    bind.is_null = nullptr;
}

template<>
inline void Connection::bind_param<double>(MYSQL_BIND& bind, double&& value, std::vector<std::string>& /* string_buffers */) {
    static thread_local double temp_double;
    temp_double = value;
    
    bind.buffer_type = MYSQL_TYPE_DOUBLE;
    bind.buffer = &temp_double;
    bind.buffer_length = sizeof(double);
    bind.is_null = nullptr;
}

template<>
inline void Connection::bind_param<float>(MYSQL_BIND& bind, float&& value, std::vector<std::string>& /* string_buffers */) {
    static thread_local float temp_float;
    temp_float = value;
    
    bind.buffer_type = MYSQL_TYPE_FLOAT;
    bind.buffer = &temp_float;
    bind.buffer_length = sizeof(float);
    bind.is_null = nullptr;
}

template<>
inline void Connection::bind_param<std::string>(MYSQL_BIND& bind, std::string&& value, std::vector<std::string>& string_buffers) {
    // 深拷贝字符串到缓冲区
    string_buffers.push_back(std::move(value));
    const std::string& str_ref = string_buffers.back();
    
    bind.buffer_type = MYSQL_TYPE_STRING;
    bind.buffer = const_cast<char*>(str_ref.c_str());
    bind.buffer_length = str_ref.length();
    bind.is_null = nullptr;
}

template<>
inline void Connection::bind_param<const char*>(MYSQL_BIND& bind, const char*&& value, std::vector<std::string>& string_buffers) {
    // 将C字符串转换为std::string并深拷贝
    if (value) {
        string_buffers.emplace_back(value);
        const std::string& str_ref = string_buffers.back();
        
        bind.buffer_type = MYSQL_TYPE_STRING;
        bind.buffer = const_cast<char*>(str_ref.c_str());
        bind.buffer_length = str_ref.length();
        bind.is_null = nullptr;
    } else {
        static bool is_null_flag = true;
        bind.buffer_type = MYSQL_TYPE_NULL;
        bind.buffer = nullptr;
        bind.buffer_length = 0;
        bind.is_null = &is_null_flag;
    }
}

template<>
inline void Connection::bind_param<std::string_view>(MYSQL_BIND& bind, std::string_view&& value, std::vector<std::string>& string_buffers) {
    // 深拷贝string_view到缓冲区
    string_buffers.emplace_back(value);
    const std::string& str_ref = string_buffers.back();
    
    bind.buffer_type = MYSQL_TYPE_STRING;
    bind.buffer = const_cast<char*>(str_ref.c_str());
    bind.buffer_length = str_ref.length();
    bind.is_null = nullptr;
}

template<>
inline void Connection::bind_param<std::nullptr_t>(MYSQL_BIND& bind, std::nullptr_t&& /* value */, std::vector<std::string>& /* string_buffers */) {
    static thread_local bool is_null_flag = true;
    bind.buffer_type = MYSQL_TYPE_NULL;
    bind.buffer = nullptr;
    bind.buffer_length = 0;
    bind.is_null = reinterpret_cast<bool*>(&is_null_flag);
}

// 通用的参数绑定helper函数
template<typename... Args>
void Connection::prepare_and_bind_params(MYSQL_STMT* stmt, Args&&... params) {
    constexpr size_t param_count = sizeof...(params);

    if (param_count != mysql_stmt_param_count(stmt)) {
        throw SQLException("Parameter count mismatch: expected " +
            std::to_string(mysql_stmt_param_count(stmt)) +
            ", got " + std::to_string(param_count));
    }

    if constexpr (param_count > 0) {
        // 使用成员变量而不是局部变量，以保持字符串生命周期
        param_binds_.resize(param_count);
        param_string_buffers_.clear();
        param_string_buffers_.reserve(param_count); // 预留空间

        // 清零MYSQL_BIND结构
        std::memset(param_binds_.data(), 0, sizeof(MYSQL_BIND) * param_count);

        size_t index = 0;
        (bind_param(param_binds_[index++], std::forward<Args>(params), param_string_buffers_), ...);

        bind_parameters(stmt, param_binds_);
    }
}

}  // namespace yxmysql