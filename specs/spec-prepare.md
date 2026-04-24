# fox-mysql 预编译 SQL（Prepared）设计文档

> **目标**：在不改变现有用法的前提下，为 `Connection` 增加两条便捷接口与语句缓存（FIFO），覆盖 80% 高频场景，坚持**不过度设计**。本稿分为「必选功能」与「可选功能」两部分，开发团队先实现必选功能，可选功能根据数据驱动逐步演进。

---

## 📋 实现状态

**实现完成度**: ✅ **95%** (MVP 功能完整)
**最后更新时间**: 2025-10-20
**实现版本**: v1.0

### 实现情况总结

本设计文档的必选功能（MVP）已基本实现，少数项需注意：

| 功能模块 | 设计要求 | 实现状态 | 代码位置 |
|---------|---------|---------|---------|
| **API 接口** | | | |
| execute_prepared | 非查询语句接口 | ✅ 已实现 | `include/fox-mysql/connection.h:35-36` |
| query_prepared | 查询语句接口 | ✅ 已实现 | `include/fox-mysql/connection.h:38-39` |
| **参数绑定** | | | |
| 整数类型 | int, long long | ✅ 已实现 | `include/fox-mysql/connection_prepared.hpp:131-150` |
| 浮点类型 | float, double | ✅ 已实现 | `include/fox-mysql/connection_prepared.hpp:153-172` |
| 字符串类型 | string, string_view, const char* | ✅ 已实现 | `include/fox-mysql/connection_prepared.hpp:175-216` |
| NULL 值 | nullptr_t | ✅ 已实现 | `include/fox-mysql/connection_prepared.hpp:219-225` |
| 深拷贝字符串 | 内部复制字符串参数 | ✅ 已实现 | `include/fox-mysql/connection_prepared.hpp:176-210` |
| 参数数量检查 | 编译期+运行期检查 | ✅ 已实现 | `include/fox-mysql/connection_prepared.hpp:232-236` |
| **语句缓存** | | | |
| FIFO 缓存 | deque 实现（实际为 LRU） | ⚠️ 实现差异 | `include/fox-mysql/connection.h:88` |
| 缓存 Key | db_name + '\n' + trim(sql) | ✅ 已实现 | `src/connection.cpp:233-250` |
| 缓存容量 | 默认 32，可配置 | ✅ 已实现 | `src/connection.cpp:302` |
| close 清空缓存 | 析构/close 时清空 | ✅ 已实现 | `src/connection.cpp:40-43,119-120` |
| 重连清空缓存 | 重连成功后清空 | ✅ 已实现 | `src/connection.cpp:113-116` |
| USE db 清空缓存 | 切换数据库后清空 | ❌ **未实现** | - |
| **错误处理** | | | |
| 白名单重试 | 4 种错误码重试 | ✅ 已实现 | `src/connection.cpp:315-357` |
| 仅重试一次 | retry_attempted 标志 | ✅ 已实现 | `include/fox-mysql/connection_prepared.hpp:11,64` |
| ER_UNSUPPORTED_PS | 不支持的语句不入缓存 | ✅ 已实现 | `src/connection.cpp:290-293` |
| **执行流程** | | | |
| reset + bind + execute | 完整流程 | ✅ 已实现 | `include/fox-mysql/connection_prepared.hpp:10-127` |
| 设置 UPDATE_MAX_LENGTH | 查询时设置属性 | ✅ 已实现 | `include/fox-mysql/connection_prepared.hpp:101-104` |
| store_result | 查询时存储结果 | ✅ 已实现 | `include/fox-mysql/connection_prepared.hpp:107` |
| **ResultSet 一致性** | | | |
| 统一接口 | 与非 prepared 接口一致 | ✅ 已实现 | `include/fox-mysql/result_set.h` |
| **可选功能** | | | |
| 日志与指标 | 缓存命中/淘汰计数等 | ⚠️ 未实现 | - |

### 已知问题与差异

1. **❌ 缺失功能**:
   - **USE database 清空缓存**: 设计要求执行 `USE db` 后清空缓存，当前未实现。虽然 `current_database_` 变量存在，但 `execute()` 方法未检测 USE 语句。这可能导致切换数据库后，缓存的 prepared statement 指向错误的 schema。

2. **⚠️ 实现差异**:
   - **缓存策略**: 设计文档明确要求 **FIFO**，但实际实现是 **LRU** 风格（`src/connection.cpp:272-276` 将命中项移到队尾）。代码注释中也说明："LRU行为，虽然文档说FIFO，但这样更高效"。**LRU 通常更优，建议保持当前实现。**

3. **⚠️ 未实现项**:
   - 日志输出（SQL 片段、准备成功/失败等）
   - 统计指标（缓存命中率、执行耗时等）
   - 以上属于可选的观测性增强功能，不影响核心使用

### 设计变更记录

- **缓存策略**: 从设计的 FIFO 改为 LRU（更高效）

### 后续改进建议

1. **高优先级**: 实现 USE database 后清空缓存（避免跨 schema 语句混乱）
2. **中优先级**: 统一文档与代码，明确采用 LRU 缓存策略
3. **低优先级**: 添加日志和统计指标（按需添加）

---

## 一、必选功能

### 1. 对外 API

```cpp
// 非查询语句（INSERT/UPDATE/DELETE/DDL）
template <typename... Args>
void execute_prepared(std::string_view sql, Args&&... params);

// 查询语句
template <typename... Args>
std::unique_ptr<ResultSet> query_prepared(std::string_view sql, Args&&... params);
```

* 仅支持位置占位符 `?`。
* 参数个数必须与 `mysql_stmt_param_count(stmt)` 一致，否则抛异常。
* 支持：整数、浮点、字符串、二进制、可空、日期时间类型（与 `fox::mysql::types` 对齐）。

---

### 2. 参数生命周期（强约束）

* **字符串/二进制参数**：MVP 统一**内部复制**，由 binder 缓冲持有到执行完成。
* POD（整型/浮点/日期等）直接按值绑定。
* 避免了“调用者需保证 buffer 有效”的陷阱。

---

### 3. 语句缓存（FIFO）

* 每个 `Connection` 一份缓存，默认容量 32（`stmt_cache_capacity` 可配置，推荐 16–128）。
* **Key**：`db_name + '\n' + trim(sql)`（仅去首尾空白）。
* **Value**：`MYSQL_STMT*` + `param_count`/`column_count` 等元数据。
* 策略：FIFO 淘汰，溢出时 `mysql_stmt_close` 最旧项。
* 在以下情况清空整个缓存：

  * `Connection::close()` / 析构；
  * 断线重连成功后；
  * 执行 `USE db` 成功后。

---

### 4. 执行流程

以 `query_prepared` 为例：

1. `check_connection()`。
2. 缓存查找 → miss 时 `mysql_stmt_init + mysql_stmt_prepare` → 放入 FIFO。
3. `mysql_stmt_free_result(stmt)`（如上次是查询）→ `mysql_stmt_reset(stmt)`。
4. 参数绑定（含字符串/二进制**深拷贝**）。
5. `mysql_stmt_execute(stmt)`。
6. 查询：

   * `mysql_stmt_attr_set(..., STMT_ATTR_UPDATE_MAX_LENGTH, 1)`；
   * `mysql_stmt_store_result(stmt)`；
   * 构造基于 `stmt` 的 `ResultSet`，返回。
7. 非查询：结果通过连接级 API (`mysql_affected_rows`/`mysql_insert_id`) 获取。
8. 错误处理（见下）。

---

### 5. 错误与重试（白名单）

**仅对以下错误执行“删缓存 → 重连 → re-prepare → 仅重试一次”：**

* `CR_SERVER_GONE_ERROR`
* `CR_SERVER_LOST`
* `ER_UNKNOWN_STMT_HANDLER`
* `ER_NEED_REPREPARE`

**其余错误**：直接抛 `fox::mysql::exception`。

* 包括：语法错误、权限不足、死锁/唯一键冲突、`ER_UNSUPPORTED_PS`（语句不支持 prepare）等。
* `ER_UNSUPPORTED_PS` 情况下，该 SQL **不入缓存**。

---

### 6. ResultSet 一致性

* 外部 API 与非 prepared 的 `ResultSet` 完全一致。
* `get<T>(i)`/`get<T>(name)` 行为相同。
* **列名映射缓存**：构造时建立大小写不敏感的 `unordered_map`，重复列名取第一个。
* 内部资源管理：

  * 读取列元信息后立即 `mysql_free_result(meta)`；
  * `ResultSet` 析构时 `mysql_stmt_free_result(stmt)`。

---

### 7. 线程安全与事务

* 明确：`Connection` **非线程安全**，不得跨线程并发。
* 事务：支持 `begin_transaction()` → 多次 prepared 执行 → `commit/rollback`，缓存不随事务结束而清空。

---

### 8. 日志与指标（最小集）

* 准备成功/失败计数。
* 缓存命中/淘汰计数。
* 白名单重试次数。
* 执行耗时（均值/分位数可后续补）。
* **不记录参数值**（避免隐私泄露），仅日志 SQL 片段（可截断至前 256 字符）。

---

### 9. 不支持项（MVP 明确声明）

* 不支持 `mysql_stmt_send_long_data` 分片参数（大参数受 `max_allowed_packet` 限制）。
* 不支持多结果集（如存储过程返回的多个结果集）。
* 遇到 `ER_UNSUPPORTED_PS`：直接抛错，SQL 不入缓存。

---

### 10. 测试要点

1. 基本增删改查 + 多次复用。
2. 参数生命周期（临时字符串也能正确执行）。
3. 占位符数量与参数不匹配 → 抛异常。
4. 白名单错误模拟 → 验证“重连+re-prepare+仅重试一次”。
5. 非白名单错误 → 直接抛。
6. ResultSet 一致性：列名映射、大小写不敏感。
7. 缓存容量极端值 → FIFO 淘汰正确。

---

## 二、可选功能（后续演进）

以下功能 **不在 MVP 范围**，可根据性能与可维护性需求逐步引入：

1. **LRU 缓存**（替代 FIFO），提升缓存命中率。
2. **命名参数支持**（`:name` → `?`，需解析 SQL 跳过字符串/注释）。
3. **SQL 规范化**（去注释/压缩空白）以提升缓存命中率。
4. **PreparedStatement 对外类**（支持长期持有、手动 bind/execute）。
5. **流式结果模式**（`mysql_stmt_fetch` 无缓冲，适合大结果集）。
6. **大参数分片**（`mysql_stmt_send_long_data`）。
7. **多结果集支持**（存储过程）。
8. **更细粒度错误恢复**（除白名单外的其他可恢复场景）。
9. **高级日志/指标**（参数值脱敏日志、详细耗时分布）。

---

## 三、实施顺序（MVP）

1. FIFO 缓存与句柄 RAII。
2. 参数绑定器（含字符串/二进制深拷贝）。
3. 便捷接口跑通（execute/query）。
4. ResultSet stmt 构造路径与列名映射缓存。
5. 错误处理与白名单重试。
6. 单测与文档（含非线程安全提示与容量调优建议）。

---

✅ **最终总结**：
MVP 只做：**便捷接口 + FIFO 缓存 + 内部深拷贝参数 + 白名单重试一次 + ResultSet 一致性**。
所有增强（LRU、命名参数、流式结果等）放到「可选功能」章节，避免初期过度设计。

