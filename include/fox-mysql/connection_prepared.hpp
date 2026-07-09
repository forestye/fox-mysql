#pragma once

#include "fox-mysql/connection.h"
#include "fox-mysql/result_set.h"
#include <cstring>
#include <type_traits>

namespace fox::mysql {

// 执行骨架: reset → bind (由 bind_fn 注入) → execute → 可恢复错误重试一次。
// 变参模板版与 vector<Param> 运行时版共用, 消除两份复制粘贴。
// 注意 bind_fn 在重试时会被再次调用, 因此它不能移走外部状态 (vector 版
// 从 const& 拷贝, 满足; 变参版行为与重构前一致)。
template <typename BindFn>
void Connection::execute_prepared_with(std::string_view sql, BindFn&& bind_fn) {
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
            bind_fn(stmt);

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
            mysql_stmt_free_result(stmt);

            break; // 成功执行，退出重试循环

        } catch (const SQLException& e) {
            if (retry_attempted) {
                throw; // 已经重试过，重新抛出异常
            }
            throw; // 非可恢复异常，直接抛出
        }
    }
}

template <typename BindFn>
std::unique_ptr<ResultSet> Connection::query_prepared_with(std::string_view sql, BindFn&& bind_fn) {
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
            bind_fn(stmt);

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

template <typename... Args, typename>
void Connection::execute_prepared(std::string_view sql, Args&&... params) {
    execute_prepared_with(sql, [&](MYSQL_STMT* stmt) {
        prepare_and_bind_params(stmt, std::forward<Args>(params)...);
    });
}

template <typename... Args, typename>
std::unique_ptr<ResultSet> Connection::query_prepared(std::string_view sql, Args&&... params) {
    return query_prepared_with(sql, [&](MYSQL_STMT* stmt) {
        prepare_and_bind_params(stmt, std::forward<Args>(params)...);
    });
}

// 参数绑定的特化实现
// 数值类型不能再用 static thread_local 局部变量做载体：同一条 SQL 里的多个同类型
// 参数会共享同一个 static，使得所有 MYSQL_BIND 都指向最后一次写入的值。
// 改为按参数位置在 param_scalar_buffers_ 中各占一个槽，地址稳定（prepare_and_bind_params
// 已 reserve 到 param_count，emplace_back 不会触发重分配）。
template<>
inline void Connection::bind_param<int>(MYSQL_BIND& bind, int&& value, std::vector<std::string>& /* string_buffers */) {
    auto& slot = param_scalar_buffers_.emplace_back();
    slot.i = value;

    bind.buffer_type = MYSQL_TYPE_LONG;
    bind.buffer = &slot.i;
    bind.buffer_length = sizeof(int);
    bind.is_null = nullptr;
}

template<>
inline void Connection::bind_param<long long>(MYSQL_BIND& bind, long long&& value, std::vector<std::string>& /* string_buffers */) {
    auto& slot = param_scalar_buffers_.emplace_back();
    slot.ll = value;

    bind.buffer_type = MYSQL_TYPE_LONGLONG;
    bind.buffer = &slot.ll;
    bind.buffer_length = sizeof(long long);
    bind.is_null = nullptr;
}

template<>
inline void Connection::bind_param<double>(MYSQL_BIND& bind, double&& value, std::vector<std::string>& /* string_buffers */) {
    auto& slot = param_scalar_buffers_.emplace_back();
    slot.d = value;

    bind.buffer_type = MYSQL_TYPE_DOUBLE;
    bind.buffer = &slot.d;
    bind.buffer_length = sizeof(double);
    bind.is_null = nullptr;
}

template<>
inline void Connection::bind_param<float>(MYSQL_BIND& bind, float&& value, std::vector<std::string>& /* string_buffers */) {
    auto& slot = param_scalar_buffers_.emplace_back();
    slot.f = value;

    bind.buffer_type = MYSQL_TYPE_FLOAT;
    bind.buffer = &slot.f;
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
        // 使用成员变量而不是局部变量，以保持字符串/数值参数生命周期。
        // reserve 到 param_count 关键：保证 emplace_back 不会触发重分配，
        // 否则之前 bind 中保存的 buffer 指针会失效。
        param_binds_.resize(param_count);
        param_string_buffers_.clear();
        param_string_buffers_.reserve(param_count);
        param_scalar_buffers_.clear();
        param_scalar_buffers_.reserve(param_count);

        // 清零MYSQL_BIND结构
        std::memset(param_binds_.data(), 0, sizeof(MYSQL_BIND) * param_count);

        size_t index = 0;
        // 对参数进行类型转换，使其能匹配现有的bind_param特化
        ([&] {
            using PlainType = typename std::remove_cv<typename std::remove_reference<Args>::type>::type;

            // 1. 检测字符串字面量（数组）- 优先处理
            if constexpr (std::is_array_v<typename std::remove_reference<Args>::type> &&
                         std::is_same_v<typename std::remove_extent<typename std::remove_reference<Args>::type>::type, const char>) {
                // 字符串字面量：转换为const char*
                bind_param(param_binds_[index++], static_cast<const char*>(params), param_string_buffers_);
            }
            // 2. 检测std::string的左值引用
            else if constexpr (std::is_same_v<PlainType, std::string> && std::is_lvalue_reference_v<Args>) {
                // 左值string（包括const和非const）：转换为string_view
                bind_param(param_binds_[index++], std::string_view(params), param_string_buffers_);
            }
            // 3. 检测其他类型的非const左值引用
            else if constexpr (std::is_lvalue_reference_v<Args> &&
                              !std::is_const_v<typename std::remove_reference<Args>::type>) {
                // 非const左值引用：去除引用后传递（会被当作右值）
                // 对于基础类型(int, double等)，这会复制值
                bind_param(param_binds_[index++], static_cast<PlainType>(params), param_string_buffers_);
            }
            // 4. 其他类型：正常转发
            else {
                bind_param(param_binds_[index++], std::forward<Args>(params), param_string_buffers_);
            }
        }(), ...);

        bind_parameters(stmt, param_binds_);
    }
}

}  // namespace fox::mysql