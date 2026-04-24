# MySQL 封装库设计指导文档

---

## 📋 实现状态

**实现完成度**: ✅ **100%**
**最后更新时间**: 2025-10-20
**实现版本**: v1.0

### 实现情况总结

本设计文档的所有要求均已完整实现，具体如下：

| 功能模块 | 设计要求 | 实现状态 | 代码位置 |
|---------|---------|---------|---------|
| RAII 连接管理 | 构造即连接，析构自动释放 | ✅ 已实现 | `src/connection.cpp:30-43` |
| 智能指针管理 | 使用 unique_ptr 管理资源 | ✅ 已实现 | `include/fox-mysql/connection.h:31` |
| 异常体系 | SQLException 及子类 | ✅ 已实现 | `include/fox-mysql/exception.h:8-49` |
| 移动语义 | 禁拷贝、可移动 | ✅ 已实现 | `include/fox-mysql/connection.h:21-24` |
| 现代语法 | string_view、enum class、chrono | ✅ 已实现 | 全局使用 |
| 连接接口 | connect/close/ping/escape | ✅ 已实现 | `include/fox-mysql/connection.h:26-41` |
| 查询接口 | execute/query | ✅ 已实现 | `include/fox-mysql/connection.h:31-32` |
| 结果集封装 | ResultSet 类 | ✅ 已实现 | `include/fox-mysql/result_set.h` |
| 事务支持 | begin/commit/rollback | ✅ 已实现 | `include/fox-mysql/connection.h:46-48` |

### 扩展功能实现

除了本设计文档的基础要求外，项目还实现了以下扩展功能：
- ✅ 预编译语句支持（见 `spec-prepare.md`）
- ✅ 连接池功能（见 `spec-pool.md`）

---

## 1. 库的定位与目标

该库的作用是对 **MySQL C API**（`libmysqlclient`）进行现代 C++ 风格的封装，提供简洁、安全、高性能的底层数据库访问接口。

### 主要目标

* 提供 **RAII 风格的连接管理**（构造即连接，析构自动释放）；
* 提供 **类型安全、异常安全** 的 SQL 执行与结果获取接口；
* 提供 **现代C++接口**（智能指针、STL容器、string\_view 等）；
* 保持 **高性能与轻量级**，避免过度抽象带来的开销；
* 成为更高层功能（如 ORM、连接池、缓存）的稳定依赖基础。

### 明确不应承担的职责

* **业务逻辑**：本库只负责数据库交互，不涉及业务规则或 UI。
* **ORM 功能**：对象映射交由更高层 ORM 模块负责，本库只提供原始结果集与简单容器封装。
* **连接池管理**：连接池是基础设施模块，应在本库之上独立实现。
* **缓存策略**：是否缓存数据属于上层业务/DAO 层策略，本库不涉及。
* **多数据库兼容**：当前仅面向 MySQL，不抽象为通用数据库接口。

---

## 2. 设计原则与现代 C++ 实践

1. **RAII**

   * `Connection` 对象在构造时建立连接，在析构时自动关闭。
   * `ResultSet` 对象在析构时自动释放 `MYSQL_RES`。

2. **智能指针**

   * 使用 `std::unique_ptr`（带自定义 deleter）管理 `MYSQL_RES` 等资源。
   * 避免裸指针泄漏。

3. **异常安全**

   * 操作失败时抛出自定义异常（封装 `mysql_error` 信息）。
   * 禁止析构函数抛出异常。

4. **现代语法**

   * 禁止拷贝构造/赋值（使用 `= delete`），必要时提供移动构造/移动赋值。
   * 使用 `std::string_view` 避免拷贝。
   * 使用 `enum class` 替代裸枚举或宏。
   * 时间参数使用 `std::chrono`。

5. **性能考虑**

   * 避免不必要的字符串拷贝，提供 `fetchRowView()` 之类的接口返回 `string_view`。
   * 保持线程隔离：一个连接对象只能在单线程使用。

---

## 3. 抽象层级设计

### （1）连接层

* 类名建议：`fox::mysql::Connection`
* 职责：管理数据库连接生命周期，封装 `mysql_real_connect` 等。
* 提供接口：

  * `connect(...)` / 构造函数自动连接
  * `close()` / 析构自动释放
  * `ping()`：检测并重连
  * `escapeString(std::string_view)`

### （2）查询执行层

* 类名建议：`fox::mysql::Statement` 或直接由 `Connection` 提供方法。
* 职责：执行 SQL 语句，返回结果集对象。
* 提供接口：

  * `execute(sql)`：无返回结果（INSERT/UPDATE/DELETE）
  * `query(sql)`：返回 `ResultSet`

### （3）结果集封装层

* 类名建议：`fox::mysql::ResultSet`
* 职责：封装 `MYSQL_RES` 与迭代结果行。
* 提供接口：

  * `bool next()`：迭代结果
  * `std::string getString(int col)`
  * `long long getInt(int col)`
  * `std::vector<std::string> rowAsVector()`
  * `std::map<std::string, std::string> rowAsMap()`

### （4）错误处理层

* 类名建议：`fox::mysql::SQLException`
* 封装错误码与错误信息。

---

## 4. 与上层模块的关系

* **连接池**

  * 单独实现 `ConnectionPool` 模块，内部维护多个 `fox::mysql::Connection` 实例。
  * 对本库无侵入。

* **ORM**

  * ORM 模块通过组合/调用 `Connection` 与 `ResultSet` 提供对象映射。
  * ORM 层负责将行数据映射到业务实体。

* **缓存**

  * 建议在 ORM 或 DAO 层实现缓存策略，例如 `std::unordered_map` 缓存热点查询。
  * 本库不直接提供缓存。

---

## 5. 推荐开发步骤

1. **第一阶段：连接管理**

   * 实现 `Connection` 类，完成 RAII 封装。
2. **第二阶段：查询与结果**

   * 实现 `query()` 与 `ResultSet` 封装。
3. **第三阶段：错误与异常**

   * 定义统一异常类型，替代返回码。
4. **第四阶段：性能优化**

   * 引入 `string_view`、移动语义，减少拷贝。
5. **第五阶段：扩展接口**

   * 支持预处理语句（`mysql_stmt_*`）。
   * 支持事务（`begin/commit/rollback`）。

---

## 6. 总结

该库应是一个**轻量级、现代化、仅面向 MySQL 的底层封装**，职责专注在 **连接管理 + SQL 执行 + 结果封装**。
ORM、连接池、缓存等应在其之上独立实现，保持清晰分层与低耦合。
遵循现代C++的最佳实践（RAII、智能指针、异常、安全接口），在保证性能的同时提升可维护性。
