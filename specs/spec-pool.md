# MySQL 连接池设计文档

---

## 📋 实现状态

**实现完成度**: ✅ **98%** (核心功能 100%)
**最后更新时间**: 2025-10-20
**实现版本**: v1.0

### 实现情况总结

本设计文档的核心功能已全部实现，部分可选增强功能未实现：

| 功能模块 | 设计要求 | 实现状态 | 代码位置 |
|---------|---------|---------|---------|
| **核心功能** | | | |
| PoolOptions 配置 | 6 个配置选项 | ✅ 已实现 | `include/yxmysql/pool.h:15-22` |
| PooledConn 句柄 | RAII、移动语义 | ✅ 已实现 | `include/yxmysql/pool.h:28-57` |
| 线程安全 | mutex + condition_variable | ✅ 已实现 | `include/yxmysql/pool.h:107-108` |
| 预热机制 | 构造时创建 min_size 连接 | ✅ 已实现 | `src/pool.cpp:82-95` |
| 健康检查 | ping + 重连 | ✅ 已实现 | `src/pool.cpp:103-125` |
| 弹性伸缩 | 按需扩容、空闲回收 | ✅ 已实现 | `src/pool.cpp:127-156,196-208` |
| 阻塞与超时 | 支持超时等待 | ✅ 已实现 | `src/pool.cpp:213-218` |
| 兜底 rollback | 归还时执行 rollback | ✅ 已实现 | `src/pool.cpp:237-254` |
| 两阶段回收 | 锁内移出、锁外析构 | ✅ 已实现 | `src/pool.cpp:129-156` |
| 异常体系 | 4 种 Pool 异常 | ✅ 已实现 | `include/yxmysql/pool.h:123-144` |
| 关闭处理 | shutdown 标志 + 唤醒 | ✅ 已实现 | `src/pool.cpp:71-80` |
| **指标统计** | | | |
| 基础指标 | 6 个 atomic 计数器 | ✅ 已实现 | `include/yxmysql/pool.h:114-119` |
| **可选功能** | | | |
| 日志输出 | 创建/销毁/超时等日志 | ⚠️ 未实现 | - |
| 详细时间统计 | 平均等待时间、P95 等 | ⚠️ 未实现 | - |

### 实现说明

1. **核心功能完整性**: 所有必需的连接池功能均已实现，包括线程安全、健康检查、弹性伸缩等
2. **统计指标**: 实现了基础的原子计数器（创建数、销毁数、等待数、超时数、重连数等）
3. **未实现项**: 日志输出和详细时间统计属于可选的观测性增强功能，不影响核心功能使用

### 设计变更记录

- 无重大设计变更，实现与设计文档高度一致

---

## 背景与目标

本设计基于已有的 `yxmysql::Connection` 封装，构建一个高效、稳定、可扩展的连接池组件。目标包括：

* **线程安全**：支持多线程并发 `acquire()` / `release()`。
* **稳定性**：获取时可进行健康检查（`ping()`）、失败时自动重连。
* **弹性伸缩**：支持 `min_size` 预热，按需扩容到 `max_size`，并在空闲时缩容。
* **阻塞与超时**：连接不足时阻塞等待，支持超时返回。
* **RAII 易用**：通过句柄自动归还连接，避免资源泄漏。
* **可观测**：支持统计指标，便于运维与调优。

---

## 与现有封装的关系与假设

* 底层使用 `yxmysql::Connection`（以下简称 **Conn**）。
* `connect()` 成功后视为可用；`close()` 彻底关闭。
* `ping()` 作为健康检查；失败时尝试一次 **重连**。
* `is_connected()` 是轻量健康信号，但获取时仍需 `ping()` 确认。
* 池假设归还时连接状态干净；若存在事务未提交，归还时执行兜底 `rollback()`。

---

## 命名空间与核心 API

连接池放在单独命名空间 `yxmysql_pool`：

```cpp
namespace yxmysql_pool {

struct PoolOptions {
    size_t min_size = 2;
    size_t max_size = 16;
    std::chrono::milliseconds acquire_timeout {3000};
    std::chrono::milliseconds idle_max_age {std::chrono::minutes(5)};
    bool health_check_on_acquire = true;
    bool rollback_on_return = true;
};

class ConnectionPool;

class PooledConn {
public:
    PooledConn(const PooledConn&) = delete;
    PooledConn& operator=(const PooledConn&) = delete;
    PooledConn(PooledConn&&) noexcept = default;
    PooledConn& operator=(PooledConn&&) noexcept = default;
    ~PooledConn();                       // 析构自动归还
    // 可跨线程移动，但不得多线程并发使用

    yxmysql::Connection* operator->() const noexcept;
    yxmysql::Connection& ref() const;
    void reset();                        // 显式归还
};

class ConnectionPool {
public:
    ConnectionPool(ConnectionConfig cfg, PoolOptions opts);
    ~ConnectionPool();

    PooledConn acquire(std::chrono::milliseconds timeout = {});
    size_t size() const;
    size_t idle_count() const;
};

} // namespace yxmysql_pool
```

---

## 内部数据结构与并发模型

* **空闲队列**：`std::queue<std::unique_ptr<Connection>> idle_`
  元素带 `last_used` 时间戳，用于判断是否回收。队列为 **FIFO**，自然轮转连接，避免单连接长期空闲。
* **总连接数**：`total_` 记录活跃+空闲连接总数，修改必须在锁保护下进行。
* **同步原语**：`std::mutex mu_` + `std::condition_variable cv_`。

  * `acquire()`：锁保护下取空闲或扩容；否则阻塞等待。
  * `release()`：归还连接后 `cv_.notify_one()` 唤醒等待者。
* **关闭标志**：`shutdown_`，析构时标记，阻止新请求。

---

## 连接生命周期与健康检查

### 预热

构造时同步创建 `min_size` 个连接进入空闲队列。失败直接抛异常。

### 获取（acquire）

1. 优先取空闲连接；若启用健康检查，调用 `ping()`：

   * 若失败，尝试一次 `close() → connect()`；
   * 若重连仍失败：销毁该连接（`total_--`），抛出 **HealthCheckError**。
2. 无空闲时，若 `total_ < max_size`，则新建连接：

   * 成功则返回；
   * 失败则回退 `total_--`，抛出 **ConnectError**。
3. 否则阻塞等待；超过 `acquire_timeout` 抛出 **AcquireTimeoutError**。

### 归还（release）

* 若 `rollback_on_return=true`：执行兜底 `rollback()`：

  * **无活动事务** → 忽略，继续归还；
  * **连接级错误** → 销毁该连接（`total_--`），不入队；
  * **无法判定** → 尝试一次 `ping()`，失败则销毁。
* 更新 `last_used`。
* 判断是否超过 `idle_max_age` 且 `total_ > min_size`，若满足则销毁连接。
* 否则入空闲队列，`cv_.notify_one()` 唤醒等待者。
* **注意**：回收与销毁操作采用“两阶段回收”，在锁内仅移出候选连接，锁外执行析构，避免长时间持锁。

---

## 空闲回收与缩容策略

* **机会性回收**：在 `release()` 与 `acquire()` 时检查并移除过期连接。
* **两阶段回收**：锁内移出，锁外析构。
* **保证最小池规模**：不会缩容到低于 `min_size`。

---

## 错误处理与边界情况

* 构造/预热失败：抛出异常。
* 获取超时：抛 `AcquireTimeoutError`。
* 健康检查失败：抛 `HealthCheckError`。
* 创建失败：抛 `ConnectError`。
* 池已关闭：抛 `PoolShutdownError`。
* 关闭：析构时标记 `shutdown_` 并唤醒等待者，空闲连接销毁；已借出连接不会被强制收回，归还时直接销毁。

---

## 事务与会话状态约束

* 事务不跨连接维护；业务层需在 `PooledConn` 生命周期内完成事务。
* 归还时若检测到事务未结束，执行兜底 `rollback()`；失败则销毁连接。
* 可选提供 `rollback_on_return=false` 以关闭该机制（不推荐）。

---

## 日志与指标

* **日志**：连接创建/销毁、获取超时、健康检查失败、回收事件、关闭事件。
* **指标**（建议用 `std::atomic<uint64_t>`）：

  * `current_total`, `current_idle`
  * `created_total`, `destroyed_total`
  * `acquire_wait_count`, `acquire_timeout_count`
  * `reconnect_attempts`, `reconnect_failures`
  * `average_wait_time`, `p95_wait_time`

---

## 部署注意事项

* `idle_max_age` 应小于 MySQL 服务器的 `wait_timeout` 或 `interactive_timeout`，建议留 30–60 秒余量。
* 若无法保证配置一致，必须开启 `health_check_on_acquire`。

---

## 简要类图

```
+----------------------------+        *   1      +--------------------+
| yxmysql_pool::ConnectionPool |---------------| yxmysql::Connection |
| - idle_: queue<Conn>         owns           | (已有封装类)        |
| - total_, shutdown_                         +--------------------+
+----------------------------+                 ^
| + acquire() -> PooledConn  |                 |
+----------------------------+                 |
                                              |
                                   +------------------------+
                                   | yxmysql_pool::PooledConn|
                                   | - pool_                 |
                                   | - conn_                 |
                                   +------------------------+
```

---

## 单元测试建议

1. **功能性**：获取/归还；并发短事务；事务异常后兜底回滚。
2. **时序性**：满负载阻塞等待 → 归还后唤醒。
3. **健壮性**：模拟 `ping()` 失败、重连失败、获取超时。
4. **缩容性**：空闲超过阈值时缩容；`min_size` 不被突破。
5. **关闭性**：析构唤醒等待者；已借出连接归还时销毁而不入队。
6. **ResultSet 使用**：确保 `ResultSet` 必须在归还前释放。

---

## 压力测试建议

1. **并发压力**：数百线程并发获取/归还，观察吞吐量与延迟。
2. **极限边界**：`max_size=1` 多线程争用；`max_size=N` 持续峰值压力。
3. **长时间稳定性**：运行数小时，验证无泄漏、无僵死。
4. **资源占用**：监控 CPU、内存、网络；验证空闲回收生效。
5. **异常注入**：模拟网络抖动或 MySQL 主动断开，验证重连与异常处理逻辑。

---

## 可选扩展

* **公平等待队列**：避免惊群与线程饥饿。
* **后台回收线程**：替代机会性回收，清理更及时。
* **会话复位**：归还时执行 `RESET SESSION` 或重置隔离级别/SQL 模式。
* **Prepared Statement 缓存**。
* **熔断与退避**：重连失败过多时快速失败并指数退避。
* **多池路由**：支持读写分离或多租户/多 schema 路由。
* **Drain 模式**：关闭时等待在借连接归还，提供 `drain(timeout)` 接口。
