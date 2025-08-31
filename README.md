# YXMySQL

现代 C++ MySQL 封装库，提供简洁、安全、高性能的数据库访问接口。

## 特性

- **RAII 资源管理**：自动管理数据库连接生命周期
- **异常安全**：统一的异常处理机制
- **类型安全**：提供类型安全的数据访问接口
- **现代 C++**：使用 C++17 特性，包括 `std::optional`、`std::string_view` 等
- **高性能**：避免不必要的字符串拷贝，提供高效的数据访问
- **连接池支持**：线程安全的连接池，支持弹性伸缩和健康检查
- **易于使用**：简洁的 API 设计

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
#include "yxmysql/yxmysql.h"

yxmysql::ConnectionConfig config;
config.host = "localhost";
config.user = "your_user";
config.password = "your_password";
config.database = "your_database";

yxmysql::Connection conn(config);
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
} catch (const yxmysql::SQLException& e) {
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

### 6. 连接池使用

```cpp
#include "yxmysql/pool.h"

// 配置连接池
yxmysql_pool::PoolOptions pool_opts;
pool_opts.min_size = 2;
pool_opts.max_size = 16;
pool_opts.acquire_timeout = std::chrono::seconds(5);

yxmysql_pool::ConnectionPool pool(config, pool_opts);

// 使用连接池
{
    auto conn = pool.acquire();  // 自动获取连接
    conn->execute("INSERT INTO users (name) VALUES ('Pool User')");
    
    auto result = conn->query("SELECT COUNT(*) FROM users");
    result->next();
    int count = result->get_int32(0);
    std::cout << "Total users: " << count << std::endl;
    
    // 连接在作用域结束时自动归还给连接池
}
```

## 配置选项

```cpp
yxmysql::ConnectionConfig config;
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
```

## 异常处理

库提供了层次化的异常体系：

- `yxmysql::SQLException`：基础 SQL 异常
- `yxmysql::ConnectionException`：连接相关异常
- `yxmysql::QueryException`：查询相关异常
- `yxmysql::TypeConversionException`：类型转换异常

```cpp
try {
    // 数据库操作
} catch (const yxmysql::ConnectionException& e) {
    std::cerr << "Connection error: " << e.what() << " (Code: " << e.error_code() << ")" << std::endl;
} catch (const yxmysql::QueryException& e) {
    std::cerr << "Query error: " << e.what() << std::endl;
} catch (const yxmysql::SQLException& e) {
    std::cerr << "SQL error: " << e.what() << std::endl;
}
```

## API 参考

### Connection 类

- `Connection(const ConnectionConfig& config)`：构造函数，自动连接
- `void connect()`：手动连接
- `void close()`：关闭连接
- `bool is_connected() const`：检查连接状态
- `void ping()`：检测并重连
- `std::unique_ptr<ResultSet> query(std::string_view sql)`：执行查询
- `void execute(std::string_view sql)`：执行无返回语句
- `std::string escape_string(std::string_view str)`：字符串转义
- `unsigned long long affected_rows() const`：获取影响的行数
- `unsigned long long insert_id() const`：获取最后插入的 ID
- `void begin_transaction()`：开始事务
- `void commit()`：提交事务
- `void rollback()`：回滚事务

### ResultSet 类

- `bool next()`：移动到下一行
- `std::string get_string(int column_index) const`：获取字符串
- `std::optional<std::string> get_string_opt(int column_index) const`：可选字符串
- `long long get_int64(int column_index) const`：获取 64 位整数
- `std::optional<long long> get_int64_opt(int column_index) const`：可选 64 位整数
- `int get_int32(int column_index) const`：获取 32 位整数
- `double get_double(int column_index) const`：获取双精度浮点数
- `bool is_null(int column_index) const`：检查是否为 NULL
- `std::vector<std::string> get_row_as_vector() const`：行转向量
- `std::map<std::string, std::string> get_row_as_map() const`：行转映射

## 设计原则

1. **职责单一**：仅负责 MySQL 底层封装，不包含 ORM、连接池等功能
2. **现代 C++**：充分利用 C++17 特性提升安全性和性能
3. **RAII**：资源自动管理，防止内存泄漏
4. **异常安全**：统一的错误处理机制
5. **高性能**：避免不必要的拷贝，提供高效接口

## 许可证

MIT License