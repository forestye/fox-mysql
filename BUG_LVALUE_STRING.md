# Bug Report: 左值string变量无法用于预编译SQL

## 问题描述

虽然 [BUG_STRING_LITERAL.md](./BUG_STRING_LITERAL.md) 修复了字符串字面量的问题，但**左值string变量**仍然无法用于 `query_prepared()` 和 `execute_prepared()`。

**问题类型**: 链接错误（undefined reference）

**影响范围**: 所有使用左值 `std::string` 变量作为参数的预编译SQL

## 症状

### 错误信息

```bash
undefined reference to `void yxmysql::Connection::bind_param<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&>(
    MYSQL_BIND&,
    std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&,
    std::vector<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >,
    std::allocator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > >&)'
```

### 失败的代码示例

```cpp
#include "yxmysql/pool.h"

auto conn = pool.acquire();

// ❌ 这会导致链接错误！
std::string user_id = "user123";
conn->query_prepared("SELECT * FROM users WHERE user_id = ?", user_id);
//                                                             ^^^^^^^ 左值string变量

// ❌ 多个左值参数也失败
std::string name = "Alice";
int age = 25;
conn->execute_prepared("INSERT INTO users (name, age) VALUES (?, ?)", name, age);
//                                                                     ^^^^ 左值string

// ✅ 字符串字面量可以工作（已修复）
conn->query_prepared("SELECT * FROM users WHERE name = ?", "Alice");

// ✅ 临时对象可以工作
conn->query_prepared("SELECT * FROM users WHERE name = ?", std::string("Alice"));
```

### 实际使用场景

在真实项目中，这种用法**极其常见**：

```cpp
// 典型场景1: 从请求中提取参数
std::string user_id = extract_user_id_from_request(req);
auto result = conn->query_prepared(
    "SELECT * FROM users WHERE user_id = ?",
    user_id  // ❌ 左值变量，链接失败
);

// 典型场景2: 在循环中使用
std::vector<std::string> ids = {"id1", "id2", "id3"};
for (const auto& id : ids) {
    conn->execute_prepared(
        "UPDATE users SET active = 1 WHERE user_id = ?",
        id  // ❌ 左值引用，链接失败
    );
}

// 典型场景3: 多个参数
std::string today = get_today_date();
std::string user_id = "user123";
auto rs = conn->query_prepared(
    "SELECT * FROM records WHERE user_id = ? AND date = ?",
    user_id, today  // ❌ 两个左值变量都失败
);
```

## 根本原因

### 技术细节

1. **完美转发的行为**:
   ```cpp
   std::string user_id = "test";
   conn->query_prepared("SELECT ...", user_id);
   //                                  ^^^^^^^
   // user_id 是左值，Args被推导为 std::string&（左值引用）
   ```

2. **现有特化**:
   `connection_prepared.hpp` 有以下 `bind_param` 特化：
   - ✓ `const char*`
   - ✓ `std::string` （值类型）
   - ✓ `const std::string&` （const引用）
   - ✓ `std::string_view`
   - ✗ `std::string&` - **缺失！**

3. **类型推导示例**:
   ```cpp
   template<typename... Args>
   void query_prepared(std::string_view sql, Args&&... params) {
       // 当传入左值 std::string user_id:
       // Args = std::string&  (不是 std::string)
       // std::forward<Args>(params) = std::string& （仍是引用）

       // 调用 bind_param<std::string&>(bind, params, buffers)
       // ❌ 找不到这个特化！
   }
   ```

4. **为什么编译通过但链接失败**:
   - **编译阶段**: 编译器看到模板声明，接受代码
   - **链接阶段**: 找不到 `bind_param<std::string&>` 的定义，报 undefined reference

### 与字面量bug的区别

| 类型 | 推导结果 | 状态 |
|------|----------|------|
| `"hello"` | `const char (&)[6]` | ✅ 已修复 (BUG_STRING_LITERAL.md) |
| `std::string("hello")` | `std::string` (右值) | ✅ 有特化 |
| `std::string s = "hello"; s` | `std::string&` (左值引用) | ❌ **本bug** |
| `const std::string& s` | `const std::string&` | ✅ 有特化 |

## 修复方案

### 方案1: 添加类型转换（推荐）

在 `prepare_and_bind_params()` 中检测并转换左值引用：

**文件**: `include/yxmysql/connection_prepared.hpp`

```cpp
template<typename... Args>
void Connection::prepare_and_bind_params(MYSQL_STMT* stmt, Args&&... params) {
    constexpr size_t N = sizeof...(Args);
    if (N == 0) return;

    param_binds_.resize(N);
    std::memset(param_binds_.data(), 0, sizeof(MYSQL_BIND) * N);
    param_string_buffers_.clear();

    size_t index = 0;

    // 对每个参数进行处理
    ([&] {
        using PlainType = typename std::remove_cv<typename std::remove_reference<Args>::type>::type;

        // 检测是否为string的左值引用
        if constexpr (std::is_same_v<PlainType, std::string> &&
                     std::is_lvalue_reference_v<Args> &&
                     !std::is_const_v<typename std::remove_reference<Args>::type>) {
            // 左值string引用：转换为const引用
            bind_param(param_binds_[index++], std::as_const(params), param_string_buffers_);
        }
        // 检测是否为字符串字面量（数组）
        else if constexpr (std::is_array_v<typename std::remove_reference<Args>::type> &&
                          std::is_same_v<typename std::remove_extent<typename std::remove_reference<Args>::type>::type, const char>) {
            // 字符串字面量：转换为const char*
            bind_param(param_binds_[index++], static_cast<const char*>(params), param_string_buffers_);
        }
        else {
            // 其他类型：正常转发
            bind_param(param_binds_[index++], std::forward<Args>(params), param_string_buffers_);
        }
    }(), ...);

    if (mysql_stmt_bind_param(stmt, param_binds_.data()) != 0) {
        throw QueryException(std::string("Failed to bind parameters: ") + mysql_stmt_error(stmt));
    }
}
```

### 方案2: 添加 `std::string&` 特化（不推荐）

添加新的 `bind_param` 特化：

```cpp
// 不推荐：这会为每种引用类型增加特化，不可扩展
template<>
void Connection::bind_param<std::string&>(
    MYSQL_BIND& bind,
    std::string& value,
    std::vector<std::string>& string_buffers
) {
    // 委托给const引用版本
    bind_param<const std::string&>(bind, value, string_buffers);
}
```

**为什么不推荐**: 需要为 `std::string&`, `const std::string&`, `std::string&&` 等所有引用类型都添加特化。

## 测试验证

### 测试代码

创建文件 `test_lvalue_string_fix.cpp`:

```cpp
#include "yxmysql/pool.h"
#include <iostream>
#include <cassert>

int main() {
    // 1. 配置连接
    yxmysql::ConnectionConfig config;
    config.host = "localhost";
    config.user = "test";
    config.password = "test";
    config.database = "testdb";

    // 2. 创建连接池
    yxmysql_pool::PoolOptions opts;
    opts.min_size = 1;
    opts.max_size = 5;
    yxmysql_pool::ConnectionPool pool(config, opts);

    // 3. 准备测试数据
    auto conn = pool.acquire();
    conn->execute("CREATE TABLE IF NOT EXISTS test_users (id INT, name VARCHAR(50), email VARCHAR(100))");
    conn->execute("TRUNCATE TABLE test_users");

    // 测试1: 单个左值string参数
    std::cout << "Test 1: Single lvalue string parameter..." << std::endl;
    std::string name = "Alice";
    conn->execute_prepared("INSERT INTO test_users (id, name) VALUES (1, ?)", name);

    auto rs = conn->query_prepared("SELECT name FROM test_users WHERE name = ?", name);
    assert(rs->next());
    assert(rs->get_string(0) == "Alice");
    std::cout << "✓ Test 1 PASSED" << std::endl;

    // 测试2: 多个左值string参数
    std::cout << "Test 2: Multiple lvalue string parameters..." << std::endl;
    std::string name2 = "Bob";
    std::string email = "bob@example.com";
    conn->execute_prepared("INSERT INTO test_users (id, name, email) VALUES (2, ?, ?)", name2, email);

    rs = conn->query_prepared("SELECT email FROM test_users WHERE name = ? AND email = ?", name2, email);
    assert(rs->next());
    assert(rs->get_string(0) == "bob@example.com");
    std::cout << "✓ Test 2 PASSED" << std::endl;

    // 测试3: 混合参数类型（左值string + 整数 + 字面量）
    std::cout << "Test 3: Mixed parameter types..." << std::endl;
    std::string search_name = "Charlie";
    int id = 3;
    conn->execute_prepared("INSERT INTO test_users (id, name, email) VALUES (?, ?, ?)",
                          id, search_name, "charlie@example.com");

    rs = conn->query_prepared("SELECT id, name FROM test_users WHERE id = ? AND name = ?", id, search_name);
    assert(rs->next());
    assert(rs->get_int32(0) == 3);
    assert(rs->get_string(1) == "Charlie");
    std::cout << "✓ Test 3 PASSED" << std::endl;

    // 测试4: 循环中使用左值
    std::cout << "Test 4: Using lvalues in a loop..." << std::endl;
    std::vector<std::string> names = {"Dave", "Eve", "Frank"};
    for (size_t i = 0; i < names.size(); ++i) {
        const auto& name_ref = names[i];  // 左值引用
        conn->execute_prepared("INSERT INTO test_users (id, name) VALUES (?, ?)",
                              static_cast<int>(i + 4), name_ref);
    }

    std::string dave = "Dave";
    rs = conn->query_prepared("SELECT COUNT(*) FROM test_users WHERE name IN (?, ?, ?)",
                             dave, names[1], names[2]);
    assert(rs->next());
    assert(rs->get_int32(0) == 3);
    std::cout << "✓ Test 4 PASSED" << std::endl;

    // 清理
    conn->execute("DROP TABLE test_users");

    std::cout << "\n✓ All lvalue string tests PASSED!\n" << std::endl;
    std::cout << "=== BUG FIX VERIFIED ===" << std::endl;
    std::cout << "Lvalue string variables now work correctly with prepared statements!" << std::endl;

    return 0;
}
```

### 编译和运行

```bash
# 编译测试
g++ -std=c++17 test_lvalue_string_fix.cpp -I include -L build -lyxmysql -lmysqlclient -o test_lvalue

# 运行测试
./test_lvalue
```

### 预期输出

```
Test 1: Single lvalue string parameter...
✓ Test 1 PASSED
Test 2: Multiple lvalue string parameters...
✓ Test 2 PASSED
Test 3: Mixed parameter types...
✓ Test 3 PASSED
Test 4: Using lvalues in a loop...
✓ Test 4 PASSED

✓ All lvalue string tests PASSED!

=== BUG FIX VERIFIED ===
Lvalue string variables now work correctly with prepared statements!
```

## 影响范围

### 受影响的功能

- ✓ `execute_prepared()` 使用左值string
- ✓ `query_prepared()` 使用左值string
- ✓ 通过 `PooledConn::operator->()` 调用
- ✓ 通过 `PooledConn::ref()` 调用
- ✓ 直接使用 `Connection` 对象

### 兼容性

- ✓ 不影响现有代码（字符串字面量、临时对象仍正常工作）
- ✓ 支持所有C++17编译器
- ✓ 没有性能损失（编译时转换）
- ✓ 向后兼容 BUG_STRING_LITERAL.md 的修复

## 实际案例

### 问题发现

在 `guiyuanji-server` 项目重构时发现：

```cpp
// handlers/health_handler.cpp
Json::Value health_daily_activity(photon::net::http::Request& req) {
    string user_id;
    AuthHelper::getUserIdFromRequest(req, user_id);

    string today = getTodayDateString();

    // ❌ 链接错误：user_id 和 today 都是左值
    auto step_rs = conn->query_prepared(
        "SELECT * FROM step_history WHERE user_id = ? AND record_date = ?",
        user_id, today
    );
    // ...
}
```

**影响**: 项目中有 18 处预编译SQL调用全部失败。

### Workaround（临时解决方案）

在修复前，用户被迫使用以下workaround：

```cpp
// 方法1: 使用临时对象（丑陋）
conn->query_prepared("SELECT ...", std::string(user_id), std::string(today));

// 方法2: 使用string_view（需要额外转换）
conn->query_prepared("SELECT ...", std::string_view(user_id), std::string_view(today));

// 方法3: 回退到不安全的字符串拼接（失去预编译SQL的优势）
string esc_user_id = conn->escape_string(user_id);
string sql = "SELECT * FROM users WHERE user_id = '" + esc_user_id + "'";
auto rs = conn->query(sql);
```

所有这些方法都不理想。

## 相关文件

- **需修改的文件**:
  - `include/yxmysql/connection_prepared.hpp` - 添加左值引用处理逻辑

- **测试文件**:
  - `test_lvalue_string_fix.cpp` - 验证修复的测试

- **相关bug**:
  - [BUG_STRING_LITERAL.md](./BUG_STRING_LITERAL.md) - 字符串字面量bug（已修复）

## 优先级

- **严重性**: **极高** - 阻止了最常见、最自然的使用模式
- **影响范围**: **广泛** - 几乎所有真实项目都会受影响
- **修复难度**: **低** - 只需在一个地方添加类型检测和转换
- **回归风险**: **极低** - 修复方法与字符串字面量修复类似，已验证

## 建议

### 对用户

修复前的临时建议：

```cpp
// ❌ 避免：直接使用左值string
std::string user_id = "test";
conn->query_prepared("SELECT ...", user_id);

// ✅ 推荐：使用string_view
conn->query_prepared("SELECT ...", std::string_view(user_id));

// 或者使用字符串字面量（如果值已知）
conn->query_prepared("SELECT ...", "test");
```

### 对开发者

1. **紧急修复**: 这是比字符串字面量bug更严重的问题
2. **统一处理**: 将字符串字面量和左值string的处理逻辑合并
3. **添加测试**: 补充左值引用的单元测试覆盖
4. **文档更新**: 在 USAGE_PREPARED_POOL.md 中说明参数类型限制

## 总结

- **严重性**: 极高 - 阻止了最自然、最常见的使用模式
- **修复难度**: 低 - 只需添加类型检测和转换逻辑
- **修复状态**: ⏳ 待修复
- **回归风险**: 极低 - 使用与字面量修复相同的模式

---

**发现日期**: 2025-10-20
**报告人**: Claude Code Assistant
**优先级**: P0 (Critical)
**预计修复时间**: 1-2小时
