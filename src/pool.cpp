#include "fox-mysql/pool.h"
#include <algorithm>
#include <stdexcept>
#include <thread>

namespace fox::mysql::pool {

// ============================================================================
// PooledConn Implementation
// ============================================================================

PooledConn::PooledConn(std::unique_ptr<fox::mysql::Connection> conn, ConnectionPool* pool)
    : conn_(std::move(conn)), pool_(pool) {}

PooledConn::PooledConn(PooledConn&& other) noexcept
    : conn_(std::move(other.conn_)), pool_(other.pool_) {
    other.pool_ = nullptr;
}

PooledConn& PooledConn::operator=(PooledConn&& other) noexcept {
    if (this != &other) {
        reset();  // Return current connection to pool if any
        conn_ = std::move(other.conn_);
        pool_ = other.pool_;
        other.pool_ = nullptr;
    }
    return *this;
}

PooledConn::~PooledConn() {
    reset();
}

fox::mysql::Connection* PooledConn::operator->() const noexcept {
    return conn_.get();
}

fox::mysql::Connection& PooledConn::ref() const {
    if (!conn_) {
        throw std::runtime_error("PooledConn: No connection available");
    }
    return *conn_;
}

void PooledConn::reset() {
    if (conn_ && pool_) {
        pool_->release_connection(std::move(conn_));
        pool_ = nullptr;
    }
}

// ============================================================================
// ConnectionPool Implementation
// ============================================================================

ConnectionPool::ConnectionPool(const fox::mysql::ConnectionConfig& config, 
                             const PoolOptions& options)
    : config_(config), options_(options) {
    
    if (options_.min_size > options_.max_size) {
        throw PoolException("min_size cannot be greater than max_size");
    }
    
    if (options_.min_size == 0) {
        throw PoolException("min_size must be at least 1");
    }
    
    warm_up_pool();
}

ConnectionPool::~ConnectionPool() {
    shutdown_ = true;
    condition_.notify_all();
    
    std::lock_guard<std::mutex> lock(mutex_);
    while (!idle_connections_.empty()) {
        idle_connections_.pop();
        destroyed_total_++;
    }
}

void ConnectionPool::warm_up_pool() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (size_t i = 0; i < options_.min_size; ++i) {
        try {
            auto conn = create_connection();
            idle_connections_.emplace(std::make_unique<PoolConnection>(std::move(conn)));
            total_connections_++;
            created_total_++;
        } catch (const std::exception& e) {
            throw PoolException("Failed to warm up pool: " + std::string(e.what()));
        }
    }
}

std::unique_ptr<fox::mysql::Connection> ConnectionPool::create_connection() {
    auto conn = std::make_unique<fox::mysql::Connection>(config_);
    conn->connect();
    return conn;
}

bool ConnectionPool::health_check_connection(fox::mysql::Connection* conn) {
    if (!conn || !conn->is_connected()) {
        return false;
    }
    
    try {
        conn->ping();
        return true;
    } catch (const fox::mysql::ConnectionException&) {
        reconnect_attempts_++;
        
        try {
            conn->close();
            conn->connect();
            return true;
        } catch (const std::exception&) {
            reconnect_failures_++;
            return false;
        }
    } catch (const std::exception&) {
        return false;
    }
}

void ConnectionPool::cleanup_idle_connections() {
    auto now = std::chrono::steady_clock::now();
    std::vector<std::unique_ptr<PoolConnection>> to_destroy;
    
    while (!idle_connections_.empty()) {
        auto& pool_conn = idle_connections_.front();
        
        bool should_remove = false;
        
        // Check if connection is expired and we can shrink the pool
        if (total_connections_ > options_.min_size) {
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - pool_conn->last_used);
            if (age >= options_.idle_max_age) {
                should_remove = true;
            }
        }
        
        if (should_remove) {
            to_destroy.push_back(std::move(idle_connections_.front()));
            idle_connections_.pop();
            total_connections_--;
        } else {
            break;  // Queue is FIFO, so remaining connections are newer
        }
    }
    
    // Destroy connections outside the lock
    destroyed_total_ += to_destroy.size();
}

PooledConn ConnectionPool::acquire(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (shutdown_) {
        throw PoolShutdownException();
    }
    
    auto deadline = std::chrono::steady_clock::now() + 
                   (timeout.count() > 0 ? timeout : options_.acquire_timeout);
    
    while (true) {
        if (shutdown_) {
            throw PoolShutdownException();
        }
        
        // Clean up expired idle connections
        cleanup_idle_connections();
        
        // Try to get an idle connection
        if (!idle_connections_.empty()) {
            auto pool_conn = std::move(idle_connections_.front());
            idle_connections_.pop();
            
            auto conn = std::move(pool_conn->conn);
            
            // Health check if enabled
            if (options_.health_check_on_acquire) {
                if (!health_check_connection(conn.get())) {
                    total_connections_--;
                    destroyed_total_++;
                    throw HealthCheckException("Connection failed health check and could not be recovered");
                }
            }
            
            return PooledConn(std::move(conn), this);
        }
        
        // Try to create a new connection if we haven't reached max_size
        if (total_connections_ < options_.max_size) {
            try {
                auto conn = create_connection();
                total_connections_++;
                created_total_++;
                
                lock.unlock();  // Release lock before returning
                return PooledConn(std::move(conn), this);
            } catch (const std::exception& e) {
                total_connections_--;  // Rollback the increment
                throw fox::mysql::ConnectionException("Failed to create new connection: " + std::string(e.what()));
            }
        }
        
        // Wait for a connection to become available
        acquire_wait_count_++;
        
        if (condition_.wait_until(lock, deadline) == std::cv_status::timeout) {
            acquire_timeout_count_++;
            throw AcquireTimeoutException(
                timeout.count() > 0 ? timeout : options_.acquire_timeout);
        }
    }
}

void ConnectionPool::release_connection(std::unique_ptr<fox::mysql::Connection> conn) {
    if (!conn) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (shutdown_) {
        destroyed_total_++;
        total_connections_--;
        return;
    }
    
    bool should_destroy = false;
    
    // Rollback any uncommitted transactions if enabled
    if (options_.rollback_on_return) {
        try {
            conn->rollback();
        } catch (const fox::mysql::ConnectionException&) {
            // Connection-level error, destroy the connection
            should_destroy = true;
        } catch (const fox::mysql::QueryException&) {
            // Query error during rollback, try ping to check connection health
            try {
                conn->ping();
            } catch (const std::exception&) {
                should_destroy = true;
            }
        } catch (const std::exception&) {
            // Other errors, assume connection is unhealthy
            should_destroy = true;
        }
    }
    
    if (should_destroy) {
        destroyed_total_++;
        total_connections_--;
        return;
    }
    
    // Note: We don't shrink on return - shrinking happens in cleanup_idle_connections()
    // which is called during acquire()
    
    // Return to idle pool
    idle_connections_.emplace(std::make_unique<PoolConnection>(std::move(conn)));
    condition_.notify_one();
}

size_t ConnectionPool::size() const {
    return total_connections_.load();
}

size_t ConnectionPool::idle_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return idle_connections_.size();
}

} // namespace fox::mysql::pool