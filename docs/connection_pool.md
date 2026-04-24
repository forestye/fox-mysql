# 连接池使用说明

YXMySQL 连接池提供了高效、安全、可扩展的数据库连接管理功能。

## 快速开始

### 1. 包含头文件

```cpp
#include "fox-mysql/pool.h"
```

### 2. 配置连接池

```cpp
// 数据库连接配置
fox::mysql::ConnectionConfig config;
config.host = "localhost";
config.user = "your_user";
config.password = "your_password";
config.database = "your_database";

// 连接池选项
fox::mysql::pool::PoolOptions pool_opts;
pool_opts.min_size = 2;                                           // 最小连接数
pool_opts.max_size = 16;                                          // 最大连接数
pool_opts.acquire_timeout = std::chrono::seconds(5);              // 获取超时
pool_opts.idle_max_age = std::chrono::minutes(5);                 // 空闲连接最大存活时间
pool_opts.health_check_on_acquire = true;                         // 获取时健康检查
pool_opts.rollback_on_return = true;                              // 归还时自动回滚
```

### 3. 创建连接池

```cpp
fox::mysql::pool::ConnectionPool pool(config, pool_opts);
```

### 4. 使用连接池

#### 基本使用

```cpp
{
    // 获取连接（RAII 自动管理）
    auto conn = pool.acquire();
    
    // 使用连接执行数据库操作
    conn->execute("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(100))");
    conn->execute("INSERT INTO users VALUES (1, 'Alice')");
    
    auto result = conn->query("SELECT * FROM users");
    while (result->next()) {
        int id = result->get_int32(0);
        std::string name = result->get_string(1);
        std::cout << "ID: " << id << ", Name: " << name << std::endl;
    }
    
    // 连接在作用域结束时自动归还给连接池
}
```

#### 事务处理

```cpp
{
    auto conn = pool.acquire();
    
    try {
        conn->begin_transaction();
        conn->execute("INSERT INTO users VALUES (2, 'Bob')");
        conn->execute("UPDATE users SET name = 'Bobby' WHERE id = 2");
        conn->commit();
        std::cout << "Transaction committed" << std::endl;
    } catch (const std::exception& e) {
        conn->rollback();
        std::cerr << "Transaction failed: " << e.what() << std::endl;
    }
}
```

#### 带超时的连接获取

```cpp
try {
    // 等待最多10秒获取连接
    auto conn = pool.acquire(std::chrono::seconds(10));
    // 使用连接...
} catch (const fox::mysql::pool::AcquireTimeoutException& e) {
    std::cerr << "Failed to acquire connection: " << e.what() << std::endl;
}
```

#### 显式释放连接

```cpp
auto conn = pool.acquire();
// 使用连接...

// 显式释放连接到池中
conn.reset();

// 此时 conn 不再有效
```

## 配置选项详解

### PoolOptions 参数

- **`min_size`**: 连接池最小连接数，池会预热这么多连接
- **`max_size`**: 连接池最大连接数，超过此数量会阻塞等待
- **`acquire_timeout`**: 获取连接的超时时间，超时抛出 `AcquireTimeoutException`
- **`idle_max_age`**: 空闲连接的最大存活时间，超过会被回收（但不会低于 `min_size`）
- **`health_check_on_acquire`**: 获取连接时是否进行健康检查（`ping`）
- **`rollback_on_return`**: 归还连接时是否自动执行 `rollback` 清理事务

### 建议配置

```cpp
// 高并发场景
fox::mysql::pool::PoolOptions high_concurrency;
high_concurrency.min_size = 5;
high_concurrency.max_size = 50;
high_concurrency.acquire_timeout = std::chrono::seconds(10);
high_concurrency.health_check_on_acquire = true;

// 轻量级场景  
fox::mysql::pool::PoolOptions lightweight;
lightweight.min_size = 1;
lightweight.max_size = 5;
lightweight.acquire_timeout = std::chrono::seconds(3);
lightweight.idle_max_age = std::chrono::minutes(2);
```

## 异常处理

连接池提供了层次化的异常体系：

```cpp
try {
    auto conn = pool.acquire();
    // 使用连接...
} catch (const fox::mysql::pool::AcquireTimeoutException& e) {
    // 获取连接超时
    std::cerr << "Acquire timeout: " << e.what() << std::endl;
} catch (const fox::mysql::pool::PoolShutdownException& e) {
    // 连接池已关闭
    std::cerr << "Pool shutdown: " << e.what() << std::endl;
} catch (const fox::mysql::pool::HealthCheckException& e) {
    // 健康检查失败
    std::cerr << "Health check failed: " << e.what() << std::endl;
} catch (const fox::mysql::pool::PoolException& e) {
    // 其他连接池错误
    std::cerr << "Pool error: " << e.what() << std::endl;
} catch (const fox::mysql::SQLException& e) {
    // 数据库相关错误
    std::cerr << "Database error: " << e.what() << std::endl;
}
```

## 多线程使用

连接池是线程安全的，可以在多线程环境下安全使用：

```cpp
#include <thread>
#include <vector>

void worker_thread(fox::mysql::pool::ConnectionPool& pool, int thread_id) {
    try {
        auto conn = pool.acquire();
        // 执行数据库操作...
        conn->execute("INSERT INTO logs VALUES (" + std::to_string(thread_id) + ")");
    } catch (const std::exception& e) {
        std::cerr << "Thread " << thread_id << " error: " << e.what() << std::endl;
    }
}

// 创建多个工作线程
std::vector<std::thread> threads;
for (int i = 0; i < 10; ++i) {
    threads.emplace_back(worker_thread, std::ref(pool), i);
}

// 等待所有线程完成
for (auto& t : threads) {
    t.join();
}
```

## 监控和统计

可以获取连接池的状态信息：

```cpp
std::cout << "Total connections: " << pool.size() << std::endl;
std::cout << "Idle connections: " << pool.idle_count() << std::endl;
```

## 注意事项

1. **连接生命周期**: `PooledConn` 对象在析构时会自动归还连接，确保在合适的作用域内使用
2. **事务管理**: 事务不能跨连接维护，必须在单个 `PooledConn` 生命周期内完成
3. **ResultSet 使用**: `ResultSet` 必须在连接归还前释放，不要将 `ResultSet` 存储超过连接的生命周期
4. **线程安全**: 单个 `PooledConn` 不能在多线程间共享，但连接池本身是线程安全的
5. **超时配置**: `idle_max_age` 应该小于 MySQL 服务器的 `wait_timeout`，建议预留 30-60 秒余量

## 完整示例

参见 `examples/pool_example.cpp` 了解完整的使用示例。