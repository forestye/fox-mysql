# 连接池与预编译SQL使用指南

## 快速开始

```cpp
#include "fox-mysql/pool.h"

int main() {
    // 1. 配置连接
    fox::mysql::ConnectionConfig config;
    config.host = "localhost";
    config.user = "root";
    config.password = "password";
    config.database = "mydb";

    // 2. 创建连接池
    fox::mysql::pool::PoolOptions opts;
    opts.min_size = 2;
    opts.max_size = 10;
    fox::mysql::pool::ConnectionPool pool(config, opts);

    // 3. 使用预编译SQL
    auto conn = pool.acquire();

    // 方法1：通过 -> 操作符（推荐）
    conn->execute_prepared("INSERT INTO users VALUES (?, ?, ?)",
                          1, "Alice", "alice@example.com");

    // 方法2：通过 ref() 获取引用
    fox::mysql::Connection& ref = conn.ref();
    auto result = ref.query_prepared("SELECT * FROM users WHERE id = ?", 1);

    return 0;
}
```

## 支持的参数类型

### ✅ 完全支持

```cpp
// 整数类型
conn->execute_prepared("INSERT INTO t VALUES (?)", 42);
conn->execute_prepared("INSERT INTO t VALUES (?)", 42LL);

// 浮点类型
conn->execute_prepared("INSERT INTO t VALUES (?)", 3.14);
conn->execute_prepared("INSERT INTO t VALUES (?)", 3.14f);

// 字符串类型
conn->execute_prepared("INSERT INTO t VALUES (?)", "literal");        // 字符串字面量
conn->execute_prepared("INSERT INTO t VALUES (?)", std::string("s")); // std::string
conn->execute_prepared("INSERT INTO t VALUES (?)", std::string_view("sv")); // string_view

const char* cstr = "hello";
conn->execute_prepared("INSERT INTO t VALUES (?)", cstr);             // const char*

// NULL值
conn->execute_prepared("INSERT INTO t VALUES (?)", nullptr);
```

## 常见用法

### 查询数据

```cpp
auto conn = pool.acquire();

// 单个条件
auto result = conn->query_prepared("SELECT * FROM users WHERE id = ?", 123);

// 多个条件
result = conn->query_prepared(
    "SELECT * FROM users WHERE age > ? AND city = ?",
    18, "Beijing"
);

// 处理结果
while (result->next()) {
    int id = result->get_int32(0);
    std::string name = result->get_string(1);
    std::cout << id << ": " << name << "\n";
}
```

### 插入数据

```cpp
auto conn = pool.acquire();

// 单行插入
conn->execute_prepared(
    "INSERT INTO users (name, age, email) VALUES (?, ?, ?)",
    "Alice", 25, "alice@example.com"
);

// 批量插入
for (const auto& user : users) {
    conn->execute_prepared(
        "INSERT INTO users (name, age) VALUES (?, ?)",
        user.name, user.age
    );
}
```

### 更新和删除

```cpp
auto conn = pool.acquire();

// 更新
conn->execute_prepared(
    "UPDATE users SET age = ? WHERE id = ?",
    26, 123
);

// 删除
conn->execute_prepared(
    "DELETE FROM users WHERE age < ?",
    18
);

// 获取影响行数
unsigned long long affected = conn->affected_rows();
std::cout << "Affected " << affected << " rows\n";
```

### 事务处理

```cpp
auto conn = pool.acquire();

try {
    conn->begin_transaction();

    conn->execute_prepared("UPDATE accounts SET balance = balance - ? WHERE id = ?", 100, 1);
    conn->execute_prepared("UPDATE accounts SET balance = balance + ? WHERE id = ?", 100, 2);

    conn->commit();
} catch (const std::exception& e) {
    conn->rollback();
    throw;
}
```

## 性能优势

预编译SQL相比字符串拼接的优势：

1. **SQL注入防护**：参数自动转义
2. **性能提升**：SQL只解析一次，可重复执行
3. **类型安全**：编译时类型检查
4. **代码简洁**：无需手动转义字符串

### 性能对比

```cpp
// ❌ 不推荐：字符串拼接
for (int i = 0; i < 1000; i++) {
    std::string sql = "INSERT INTO t VALUES (" +
                     std::to_string(i) + ", '" +
                     conn->escape_string(names[i]) + "')";
    conn->execute(sql);
}

// ✅ 推荐：预编译SQL
for (int i = 0; i < 1000; i++) {
    conn->execute_prepared("INSERT INTO t VALUES (?, ?)", i, names[i]);
}
// 性能提升：约30-50%（取决于SQL复杂度）
```

## 连接池最佳实践

### 配置建议

```cpp
fox::mysql::pool::PoolOptions opts;
opts.min_size = 5;              // 最小连接数：根据平均负载设置
opts.max_size = 20;             // 最大连接数：根据峰值负载设置
opts.acquire_timeout = std::chrono::milliseconds(3000);  // 获取超时
opts.idle_max_age = std::chrono::minutes(5);             // 空闲连接最大存活时间
opts.health_check_on_acquire = true;   // 获取时健康检查
opts.rollback_on_return = true;        // 归还时回滚事务
```

### 使用模式

```cpp
// ✅ 推荐：使用RAII自动归还
{
    auto conn = pool.acquire();
    conn->query_prepared("SELECT ...", params...);
    // 作用域结束时自动归还连接
}

// ✅ 推荐：显式归还
auto conn = pool.acquire();
conn->query_prepared("SELECT ...", params...);
conn.reset();  // 显式归还

// ❌ 避免：长时间持有连接
auto conn = pool.acquire();
// ... 大量非数据库操作 ...
conn->query_prepared("SELECT ...", params...);  // 连接被占用太久
```

## 错误处理

```cpp
#include "fox-mysql/exception.h"

try {
    auto conn = pool.acquire();
    conn->query_prepared("SELECT * FROM users WHERE id = ?", id);

} catch (const fox::mysql::pool::AcquireTimeoutException& e) {
    // 连接池获取超时
    std::cerr << "Pool timeout: " << e.what() << "\n";

} catch (const fox::mysql::QueryException& e) {
    // SQL查询错误
    std::cerr << "Query error: " << e.what() << "\n";

} catch (const fox::mysql::ConnectionException& e) {
    // 连接错误
    std::cerr << "Connection error: " << e.what() << "\n";

} catch (const fox::mysql::SQLException& e) {
    // 其他SQL错误
    std::cerr << "SQL error: " << e.what() << "\n";
}
```

## 注意事项

### ⚠️ 重要

1. **参数数量必须匹配**
   ```cpp
   // ❌ 错误：参数数量不匹配
   conn->query_prepared("SELECT * FROM t WHERE id = ? AND name = ?", 123);
   // 期望2个参数，但只提供了1个

   // ✅ 正确
   conn->query_prepared("SELECT * FROM t WHERE id = ? AND name = ?", 123, "Alice");
   ```

2. **占位符只能是 `?`**
   ```cpp
   // ❌ 错误：不支持命名占位符
   conn->query_prepared("SELECT * FROM t WHERE id = :id", 123);

   // ✅ 正确
   conn->query_prepared("SELECT * FROM t WHERE id = ?", 123);
   ```

3. **不要在参数中使用SQL**
   ```cpp
   // ❌ 错误：SQL注入风险
   std::string table = "users; DROP TABLE users--";
   conn->query_prepared("SELECT * FROM ?", table);  // 表名不能参数化

   // ✅ 正确：只对值使用参数化
   std::string value = "'; DROP TABLE users--";
   conn->query_prepared("SELECT * FROM users WHERE name = ?", value);  // 安全
   ```

## 完整示例

见 `examples/` 目录：
- `pool_basic.cpp` - 连接池基础用法
- `prepared_crud.cpp` - 预编译SQL CRUD操作
- `transaction.cpp` - 事务处理示例

## 编译

```bash
g++ -std=c++17 -I include your_code.cpp -L build -lfox-mysql -lmysqlclient -o your_app
```

## 参考

- [API文档](./README.md)
- [设计文档](./specs/)
- [Bug报告](./BUG_STRING_LITERAL.md)
