#pragma once

#include "yxmysql/connection.h"
#include "yxmysql/types.h"
#include "yxmysql/exception.h"
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>

namespace yxmysql_pool {

struct PoolOptions {
    size_t min_size = 2;
    size_t max_size = 16;
    std::chrono::milliseconds acquire_timeout{3000};
    std::chrono::milliseconds idle_max_age{std::chrono::minutes(5)};
    bool health_check_on_acquire = true;
    bool rollback_on_return = true;
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
    yxmysql::Connection* operator->() const noexcept;
    yxmysql::Connection& ref() const;
    
    // Explicitly return the connection to pool
    void reset();
    
    // Check if this handle holds a valid connection
    explicit operator bool() const noexcept { return conn_ != nullptr; }

private:
    friend class ConnectionPool;
    
    PooledConn(std::unique_ptr<yxmysql::Connection> conn, ConnectionPool* pool);
    
    std::unique_ptr<yxmysql::Connection> conn_;
    ConnectionPool* pool_;
};

// Connection wrapper with timestamp for idle management
struct PoolConnection {
    std::unique_ptr<yxmysql::Connection> conn;
    std::chrono::steady_clock::time_point last_used;
    
    PoolConnection(std::unique_ptr<yxmysql::Connection> c)
        : conn(std::move(c)), last_used(std::chrono::steady_clock::now()) {}
};

// Main connection pool class
class ConnectionPool {
public:
    explicit ConnectionPool(const yxmysql::ConnectionConfig& config, 
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
    const yxmysql::ConnectionConfig& config() const noexcept { return config_; }
    const PoolOptions& options() const noexcept { return options_; }

private:
    friend class PooledConn;
    
    // Internal methods
    void release_connection(std::unique_ptr<yxmysql::Connection> conn);
    std::unique_ptr<yxmysql::Connection> create_connection();
    bool health_check_connection(yxmysql::Connection* conn);
    void cleanup_idle_connections();
    void warm_up_pool();
    
    // Configuration
    yxmysql::ConnectionConfig config_;
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
class PoolException : public yxmysql::SQLException {
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

} // namespace yxmysql_pool