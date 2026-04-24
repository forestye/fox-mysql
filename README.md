# YXMySQL

现代 C++ MySQL 封装库，提供简洁、安全、高性能的数据库访问接口。

## 特性

- **RAII 资源管理**：自动管理数据库连接生命周期
- **异常安全**：统一的异常处理机制
- **类型安全**：提供类型安全的数据访问接口
- **现代 C++**：使用 C++17 特性，包括 `std::optional`、`std::string_view`、变参模板等
- **高性能**：避免不必要的字符串拷贝，提供高效的数据访问
- **预编译 SQL**：支持 Prepared Statements，内置语句缓存（LRU），自动参数绑定
- **连接池支持**：线程安全的连接池，支持弹性伸缩、健康检查和自动回滚
- **易于使用**：简洁的 API 设计，RAII 风格

## 安装依赖

```bash
sudo apt update
sudo apt install pkg-config libmysqlclient-dev
```

## 构建

```bash
mkdir build && cd build
cmake .. -DBUILD_EXAMPLES=ON -DBUILD_TESTS=ON
make -j4
```

## 基础用法

### 1. 连接数据库

```cpp
#include "fox-mysql/fox-mysql.h"

fox::mysql::ConnectionConfig config;
config.host = "localhost";
config.user = "your_user";
config.password = "your_password";
config.database = "your_database";

fox::mysql::Connection conn(config);
```

### 2. 执行查询

```cpp
// 执行无返回结果的语句
conn.execute("CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(100))");
conn.execute("INSERT INTO users (name) VALUES ('Alice')");

// 执行查询语句
auto result = conn.query("SELECT id, name FROM users");
while (result->next()) {
    int id = result->get_int32(0);
    std::string name = result->get_string(1);
    std::cout << "ID: " << id << ", Name: " << name << std::endl;
}
```

### 3. 类型安全的数据访问

```cpp
// 安全的可选类型访问
auto opt_id = result->get_int64_opt(0);
if (opt_id.has_value()) {
    std::cout << "ID: " << opt_id.value() << std::endl;
}

// 检查 NULL 值
if (!result->is_null(1)) {
    std::string name = result->get_string(1);
}

// 获取行数据为容器
auto row_vector = result->get_row_as_vector();
auto row_map = result->get_row_as_map();
```

### 4. 事务管理

```cpp
try {
    conn.begin_transaction();
    conn.execute("UPDATE users SET name = 'Bob' WHERE id = 1");
    conn.execute("INSERT INTO users (name) VALUES ('Charlie')");
    conn.commit();
    std::cout << "Transaction committed successfully" << std::endl;
} catch (const fox::mysql::SQLException& e) {
    conn.rollback();
    std::cerr << "Transaction failed: " << e.what() << std::endl;
}
```

### 5. 字符串转义

```cpp
std::string user_input = "O'Reilly";
std::string escaped = conn.escape_string(user_input);
std::string sql = "INSERT INTO users (name) VALUES ('" + escaped + "')";
conn.execute(sql);
```

### 6. 预编译 SQL（Prepared Statements）

使用预编译 SQL 可以提高性能并防止 SQL 注入：

```cpp
// 执行无返回结果的预编译语句
std::string name = "Alice";
int age = 25;
conn.execute_prepared("INSERT INTO users (name, age) VALUES (?, ?)", name, age);

// 执行查询的预编译语句
auto result = conn.query_prepared("SELECT * FROM users WHERE age > ? AND name LIKE ?",
                                   20, "A%");
while (result->next()) {
    std::cout << result->get_string("name") << std::endl;
}

// 支持多种参数类型
conn.execute_prepared("UPDATE users SET name = ?, age = ?, score = ?, active = ? WHERE id = ?",
                      "Bob",           // std::string
                      30,              // int
                      95.5,            // double
                      nullptr,         // NULL
                      1);              // int

// 使用 string_view 避免拷贝
std::string_view name_view = "Charlie";
conn.execute_prepared("INSERT INTO users (name) VALUES (?)", name_view);
```

**特性**：
- 自动参数绑定，支持 `int`、`long long`、`float`、`double`、`std::string`、`std::string_view`、`const char*`、`nullptr`
- 内部自动深拷贝字符串参数，生命周期安全
- 语句缓存（LRU，默认容量 32），自动复用已准备的语句
- 连接断开时自动重试（白名单错误码）
- 与普通查询相比更高性能，尤其是重复执行的语句

### 7. 连接池使用

```cpp
#include "fox-mysql/pool.h"

// 配置连接池
fox::mysql::pool::PoolOptions pool_opts;
pool_opts.min_size = 2;                                    // 最小连接数（预热）
pool_opts.max_size = 16;                                   // 最大连接数
pool_opts.acquire_timeout = std::chrono::seconds(5);       // 获取超时
pool_opts.idle_max_age = std::chrono::minutes(5);          // 空闲连接最大存活时间
pool_opts.health_check_on_acquire = true;                  // 获取时健康检查
pool_opts.rollback_on_return = true;                       // 归还时自动回滚

fox::mysql::pool::ConnectionPool pool(config, pool_opts);

// 使用连接池
{
    auto conn = pool.acquire();  // 自动获取连接（RAII）
    conn->execute("INSERT INTO users (name) VALUES ('Pool User')");

    auto result = conn->query("SELECT COUNT(*) FROM users");
    result->next();
    int count = result->get_int32(0);
    std::cout << "Total users: " << count << std::endl;

    // 连接在作用域结束时自动归还给连接池
}

// 支持预编译 SQL
{
    auto conn = pool.acquire();
    conn->execute_prepared("INSERT INTO users (name, age) VALUES (?, ?)", "Dave", 28);
}

// 获取池统计信息
std::cout << "Pool size: " << pool.size() << std::endl;
std::cout << "Idle connections: " << pool.idle_count() << std::endl;
```

**连接池特性**：
- 线程安全，支持多线程并发获取连接
- 弹性伸缩：按需扩容到 max_size，空闲时自动缩容
- 健康检查：获取时自动 ping，失败时重连或重建
- 兜底回滚：归还时自动执行 rollback，防止事务泄漏
- 超时控制：支持阻塞等待和超时返回

## 配置选项

### 连接配置

```cpp
fox::mysql::ConnectionConfig config;
config.host = "localhost";                          // 服务器地址
config.port = 3306;                                 // 端口号
config.user = "username";                           // 用户名
config.password = "password";                       // 密码
config.database = "database_name";                  // 数据库名
config.charset = "utf8mb4";                         // 字符集
config.connect_timeout = std::chrono::seconds{10};  // 连接超时
config.read_timeout = std::chrono::seconds{30};     // 读超时
config.write_timeout = std::chrono::seconds{30};    // 写超时
config.auto_reconnect = true;                       // 自动重连
config.multi_statements = false;                    // 多语句支持
config.stmt_cache_capacity = 32;                    // 预编译语句缓存容量（默认 32）
```

### 连接池配置

```cpp
fox::mysql::pool::PoolOptions pool_opts;
pool_opts.min_size = 2;                                    // 最小连接数
pool_opts.max_size = 16;                                   // 最大连接数
pool_opts.acquire_timeout = std::chrono::milliseconds{3000}; // 获取超时
pool_opts.idle_max_age = std::chrono::minutes{5};          // 空闲连接最大存活时间
pool_opts.health_check_on_acquire = true;                  // 获取时是否健康检查
pool_opts.rollback_on_return = true;                       // 归还时是否自动回滚
```

## 异常处理

库提供了层次化的异常体系：

### 核心异常

- `fox::mysql::SQLException`：基础 SQL 异常（包含错误码）
- `fox::mysql::ConnectionException`：连接相关异常
- `fox::mysql::QueryException`：查询相关异常
- `fox::mysql::TypeConversionException`：类型转换异常

### 连接池异常

- `fox::mysql::pool::PoolException`：连接池基础异常
- `fox::mysql::pool::AcquireTimeoutException`：获取连接超时
- `fox::mysql::pool::PoolShutdownException`：连接池已关闭
- `fox::mysql::pool::HealthCheckException`：健康检查失败

```cpp
try {
    // 数据库操作
} catch (const fox::mysql::ConnectionException& e) {
    std::cerr << "Connection error: " << e.what() << " (Code: " << e.error_code() << ")" << std::endl;
} catch (const fox::mysql::QueryException& e) {
    std::cerr << "Query error: " << e.what() << std::endl;
} catch (const fox::mysql::SQLException& e) {
    std::cerr << "SQL error: " << e.what() << std::endl;
}

// 连接池异常处理
try {
    auto conn = pool.acquire(std::chrono::seconds(5));
    // 使用连接...
} catch (const fox::mysql::pool::AcquireTimeoutException& e) {
    std::cerr << "Pool timeout: " << e.what() << std::endl;
} catch (const fox::mysql::pool::HealthCheckException& e) {
    std::cerr << "Health check failed: " << e.what() << std::endl;
} catch (const fox::mysql::pool::PoolException& e) {
    std::cerr << "Pool error: " << e.what() << std::endl;
}
```

## API 参考

### Connection 类

**基础方法**：
- `Connection(const ConnectionConfig& config)`：构造函数，自动连接
- `void connect()`：手动连接
- `void close()`：关闭连接
- `bool is_connected() const`：检查连接状态
- `void ping()`：检测并重连

**查询方法**：
- `std::unique_ptr<ResultSet> query(std::string_view sql)`：执行查询
- `void execute(std::string_view sql)`：执行无返回语句
- `template<typename... Args> void execute_prepared(std::string_view sql, Args&&... params)`：执行预编译语句（无返回）
- `template<typename... Args> std::unique_ptr<ResultSet> query_prepared(std::string_view sql, Args&&... params)`：执行预编译查询

**实用方法**：
- `std::string escape_string(std::string_view str)`：字符串转义
- `unsigned long long affected_rows() const`：获取影响的行数
- `unsigned long long insert_id() const`：获取最后插入的 ID

**事务方法**：
- `void begin_transaction()`：开始事务
- `void commit()`：提交事务
- `void rollback()`：回滚事务

### ResultSet 类

**迭代方法**：
- `bool next()`：移动到下一行

**数据访问**（支持列索引或列名）：
- `std::string get_string(int column_index) const`：获取字符串
- `std::optional<std::string> get_string_opt(int column_index) const`：可选字符串
- `long long get_int64(int column_index) const`：获取 64 位整数
- `std::optional<long long> get_int64_opt(int column_index) const`：可选 64 位整数
- `int get_int32(int column_index) const`：获取 32 位整数
- `double get_double(int column_index) const`：获取双精度浮点数
- `bool is_null(int column_index) const`：检查是否为 NULL

**行转换**：
- `std::vector<std::string> get_row_as_vector() const`：行转向量
- `std::map<std::string, std::string> get_row_as_map() const`：行转映射

### ConnectionPool 类

**构造与析构**：
- `ConnectionPool(const ConnectionConfig& config, const PoolOptions& options = {})`：构造连接池，预热 min_size 个连接
- `~ConnectionPool()`：析构时关闭所有连接

**连接获取**：
- `PooledConn acquire(std::chrono::milliseconds timeout = {})`：获取连接（默认使用 acquire_timeout）

**统计信息**：
- `size_t size() const`：当前总连接数（活跃 + 空闲）
- `size_t idle_count() const`：当前空闲连接数

### PooledConn 类

**RAII 句柄**（不可拷贝，可移动）：
- `Connection* operator->() const`：访问底层连接
- `Connection& ref() const`：获取连接引用
- `void reset()`：显式归还连接到池
- `~PooledConn()`：析构时自动归还连接

## 设计原则

1. **清晰分层**：核心库专注于 MySQL 封装，连接池作为独立模块，不包含 ORM 等上层功能
2. **现代 C++**：充分利用 C++17 特性（RAII、智能指针、可选类型、变参模板等）提升安全性和性能
3. **资源安全**：RAII 自动管理资源，防止内存泄漏和连接泄漏
4. **异常安全**：统一的错误处理机制，层次化的异常体系
5. **高性能**：避免不必要的拷贝，预编译语句缓存，高效的数据访问接口
6. **易用性**：简洁的 API 设计，符合直觉的使用方式

## 项目结构

```
fox-mysql/
├── include/fox-mysql/        # 公共头文件
│   ├── fox-mysql.h          # 主头文件（包含所有核心功能）
│   ├── connection.h       # 连接类
│   ├── result_set.h       # 结果集类
│   ├── pool.h             # 连接池（独立命名空间 fox::mysql::pool）
│   ├── exception.h        # 异常类型
│   └── types.h            # 类型定义
├── src/                   # 实现文件
├── examples/              # 示例代码
├── tests/                 # 单元测试
├── specs/                 # 设计文档
└── docs/                  # 用户文档
```

## 许可证

MIT License