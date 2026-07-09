#pragma once

#include "fox-mysql/types.h"
#include "fox-mysql/exception.h"
#include <mysql/mysql.h>
#include <memory>
#include <string>
#include <string_view>
#include <deque>
#include <type_traits>
#include <vector>

namespace fox::mysql {

class ResultSet;

namespace detail {
// 判定"变参恰好是单个 std::vector<Param>": 用于把这种调用从变参模板里
// SFINAE 掉, 强制落到 vector<Param> 非模板重载。
// 不能只依赖重载决议 —— 非 const 左值和右值的 vector<Param> 对模板是
// 精确匹配 (T& / T&&), 会优先于非模板的 const T&, 导致 vector 被当成
// 单个参数塞进 bind_param 然后报一屏模板错误。
template <typename... Args>
inline constexpr bool is_single_param_vector_v = false;

template <typename A>
inline constexpr bool is_single_param_vector_v<A> =
    std::is_same_v<std::remove_cv_t<std::remove_reference_t<A>>, std::vector<Param>>;
}  // namespace detail

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
    
    // 预编译SQL接口 (变参模板版: 参数个数编译期固定)
    template <typename... Args,
              typename = std::enable_if_t<!detail::is_single_param_vector_v<Args...>>>
    void execute_prepared(std::string_view sql, Args&&... params);

    template <typename... Args,
              typename = std::enable_if_t<!detail::is_single_param_vector_v<Args...>>>
    std::unique_ptr<ResultSet> query_prepared(std::string_view sql, Args&&... params);

    // 预编译SQL接口 (运行时动态参数版: 占位符个数运行时决定)。
    // 典型场景: WHERE col IN (?,?,...,?) 动态列表、按条件拼接的可选过滤、
    // INSERT ... VALUES (?,?),(?,?),... 批量插入。
    // 语义与变参版一致: 同一条 SQL 走同一份 stmt_cache, 绑定复用同一套
    // bind_param 特化; params.size() 与 ? 个数不符抛 SQLException (带两边
    // 实际个数); 空 vector 合法, 等价于无参调用。
    void execute_prepared(std::string_view sql, const std::vector<Param>& params);
    std::unique_ptr<ResultSet> query_prepared(std::string_view sql, const std::vector<Param>& params);
    
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
    
    // 执行骨架 (reset → bind → execute → 可恢复错误重试), 变参版与
    // vector<Param> 版共用, bind 步骤由调用方以 BindFn 注入。
    template<typename BindFn>
    void execute_prepared_with(std::string_view sql, BindFn&& bind_fn);
    template<typename BindFn>
    std::unique_ptr<ResultSet> query_prepared_with(std::string_view sql, BindFn&& bind_fn);

    // 参数绑定
    void bind_parameters(MYSQL_STMT* stmt, std::vector<MYSQL_BIND>& binds);
    template<typename T>
    void bind_param(MYSQL_BIND& bind, T&& value, std::vector<std::string>& string_buffers);
    template<typename... Args>
    void prepare_and_bind_params(MYSQL_STMT* stmt, Args&&... params);
    // prepare_and_bind_params 的运行时版: 遍历 vector, std::visit 分发进
    // 同一套 bind_param 特化。
    void bind_params_runtime(MYSQL_STMT* stmt, const std::vector<Param>& params);
    
    ConnectionConfig config_;
    MYSQL* mysql_;
    bool connected_;
    
    // 预编译语句缓存(FIFO)
    std::deque<std::pair<std::string, PreparedStmtInfo>> stmt_cache_;
    std::string current_database_;

    // 参数绑定缓冲区(用于保持字符串生命周期)
    std::vector<MYSQL_BIND> param_binds_;
    std::vector<std::string> param_string_buffers_;

    // 数值参数槽位(每槽位 8 字节、按 double 对齐)。
    // 替代原先 bind_param<int/long long/double/float> 中的 static thread_local 局部变量
    // —— 这些 static 在同一条 SQL 含多个同类型数值参数时会互相覆盖，
    // 导致 MYSQL_BIND 都指向同一个被最后一次写入的值。
    union ScalarParamSlot {
        int i;
        long long ll;
        double d;
        float f;
    };
    std::vector<ScalarParamSlot> param_scalar_buffers_;
};

}  // namespace fox::mysql

#include "fox-mysql/connection_prepared.hpp"