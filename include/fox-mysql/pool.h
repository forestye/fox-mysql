#pragma once

#include "fox-mysql/connection.h"
#include "fox-mysql/types.h"
#include "fox-mysql/exception.h"
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>

namespace fox::mysql::pool {

struct PoolOptions {
    size_t min_size = 2;
    size_t max_size = 16;
    std::chrono::milliseconds acquire_timeout{3000};
    std::chrono::milliseconds idle_max_age{std::chrono::minutes(5)};
    bool health_check_on_acquire = true;
    bool rollback_on_return = true;

    // 时间戳启发式预检 (acquire 路径): 取出 idle 连接时, 若上次归还距今超过
    // 该阈值, 直接销毁并新建, 跳过 ping+reconnect 流程。0 表示关闭。
    //
    // 与 health_check_on_acquire 的关系: 互补, 不冲突。
    //   - health_check_on_acquire: 询问 server "你还在吗", 准确但有 RTT 成本
    //   - idle_assume_stale: 看本地时间戳, 完全无 RTT, 但靠启发式 (可能误杀
    //     还活着的连接, 不过反正要新建一条, 浪费仅一个连接 slot)
    //
    // 推荐场景: 低频服务 + MySQL 默认 wait_timeout=8h, 设为 5h45min (~70%)
    // 可以避免每次首次请求踩 stale (省下 N 次 ping 失败的 RTT, N = idle 队列里
    // 的 stale 数量)。0 = 关闭时, 行为退化为 health_check_on_acquire 兜底,
    // 业务方仍然能拿到好连接, 只是首次请求会多 N 次失败 ping 的延迟。
    std::chrono::milliseconds idle_assume_stale{0};
};

// Forward declaration
class ConnectionPool;

// Pool connection handle with RAII
class PooledConn {
public:
    PooledConn() = default;
    
    // Non-copyable but movable
    PooledConn(const PooledConn&) = delete;
    PooledConn& operator=(const PooledConn&) = delete;
    PooledConn(PooledConn&& other) noexcept;
    PooledConn& operator=(PooledConn&& other) noexcept;
    
    ~PooledConn();
    
    // Access the underlying connection
    fox::mysql::Connection* operator->() const noexcept;
    fox::mysql::Connection& ref() const;
    
    // Explicitly return the connection to pool
    void reset();
    
    // Check if this handle holds a valid connection
    explicit operator bool() const noexcept { return conn_ != nullptr; }

private:
    friend class ConnectionPool;
    
    PooledConn(std::unique_ptr<fox::mysql::Connection> conn, ConnectionPool* pool);
    
    std::unique_ptr<fox::mysql::Connection> conn_;
    ConnectionPool* pool_;
};

// Connection wrapper with timestamp for idle management
struct PoolConnection {
    std::unique_ptr<fox::mysql::Connection> conn;
    std::chrono::steady_clock::time_point last_used;
    
    PoolConnection(std::unique_ptr<fox::mysql::Connection> c)
        : conn(std::move(c)), last_used(std::chrono::steady_clock::now()) {}
};

// Main connection pool class
class ConnectionPool {
public:
    explicit ConnectionPool(const fox::mysql::ConnectionConfig& config, 
                           const PoolOptions& options = {});
    ~ConnectionPool();
    
    // Non-copyable and non-movable
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;
    ConnectionPool(ConnectionPool&&) = delete;
    ConnectionPool& operator=(ConnectionPool&&) = delete;
    
    // Acquire a connection from the pool
    PooledConn acquire(std::chrono::milliseconds timeout = std::chrono::milliseconds{0});
    
    // Get pool statistics
    size_t size() const;
    size_t idle_count() const;
    
    // Get pool configuration
    const fox::mysql::ConnectionConfig& config() const noexcept { return config_; }
    const PoolOptions& options() const noexcept { return options_; }

private:
    friend class PooledConn;
    
    // Internal methods
    void release_connection(std::unique_ptr<fox::mysql::Connection> conn);
    std::unique_ptr<fox::mysql::Connection> create_connection();
    bool health_check_connection(fox::mysql::Connection* conn);
    void cleanup_idle_connections();
    void warm_up_pool();
    
    // Configuration
    fox::mysql::ConnectionConfig config_;
    PoolOptions options_;
    
    // Pool state
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<std::unique_ptr<PoolConnection>> idle_connections_;
    std::atomic<size_t> total_connections_{0};
    std::atomic<bool> shutdown_{false};
    
    // Statistics (atomic for thread-safe access)
    mutable std::atomic<uint64_t> created_total_{0};
    mutable std::atomic<uint64_t> destroyed_total_{0};
    mutable std::atomic<uint64_t> acquire_wait_count_{0};
    mutable std::atomic<uint64_t> acquire_timeout_count_{0};
    mutable std::atomic<uint64_t> reconnect_attempts_{0};
    mutable std::atomic<uint64_t> reconnect_failures_{0};
};

// Pool-specific exceptions
class PoolException : public fox::mysql::SQLException {
public:
    explicit PoolException(const std::string& message)
        : SQLException("Pool error: " + message) {}
};

class AcquireTimeoutException : public PoolException {
public:
    explicit AcquireTimeoutException(std::chrono::milliseconds timeout)
        : PoolException("Acquire timeout after " + std::to_string(timeout.count()) + "ms") {}
};

class PoolShutdownException : public PoolException {
public:
    PoolShutdownException() : PoolException("Pool has been shutdown") {}
};

class HealthCheckException : public PoolException {
public:
    explicit HealthCheckException(const std::string& details)
        : PoolException("Health check failed: " + details) {}
};

} // namespace fox::mysql::pool