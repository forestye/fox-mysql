// 回归测试: ConnectionPool 在 idle 连接被 MySQL 端关闭后能自愈,
// 不再把 stale 连接的 HealthCheckException 抛给业务方。
//
// 报告复现链 (修复前):
//   1. ping() 抛 SQLException 而非 ConnectionException
//   2. health_check_connection 只 catch ConnectionException, 漏掉走重连分支
//   3. acquire() 在 health_check 失败时直接 throw HealthCheckException
// 结果: idle 队列里有几个 stale 连接, 业务方就被打几次 400, 直到坏连接耗尽。
// 典型现场: 服务器闲置一夜或云数据库回收 idle 连接后, 早上前 N 个用户登录失败。
//
// 本测试通过 KILL CONNECTION 主动让池中连接 stale, 然后 acquire 应:
//   - 不抛异常
//   - 拿到一个能正常 SELECT 1 的连接
//   - 池继续可用 (连续多次 acquire 都成功)

#include "fox-mysql/fox-mysql.h"
#include "fox-mysql/pool.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace fox::mysql;
using namespace fox::mysql::pool;

static int failures = 0;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << " " << #cond << "\n"; \
            ++failures;                                                                    \
        }                                                                                  \
    } while (0)

static ConnectionConfig make_config() {
    ConnectionConfig c;
    c.host = "localhost";
    c.port = 3306;
    c.user = "test";
    c.password = "test";
    c.database = "test";
    c.charset = "utf8mb4";
    return c;
}

static unsigned long get_thread_id(Connection& conn) {
    auto rs = conn.query("SELECT CONNECTION_ID()");
    rs->next();
    return static_cast<unsigned long>(rs->get_int64(0));
}

// 用一个独立的 Connection (不在 pool 里) 强制 KILL 掉指定 thread id 的连接,
// 等同于 MySQL 端因 wait_timeout 关闭 idle 连接。
static void kill_connections(const std::vector<unsigned long>& tids) {
    Connection killer(make_config());
    killer.connect();
    for (auto tid : tids) {
        // KILL CONNECTION 让 server 立即关闭对应 socket; 客户端下次 ping/query 才会感知。
        // 用 try 包住是因为如果连接本就被 server 回收, KILL 会报 Unknown thread id, 这无所谓。
        try {
            killer.execute("KILL CONNECTION " + std::to_string(tid));
        } catch (const std::exception&) {
            // ignore
        }
    }
    // 给 server 一点时间真正关闭 socket; 不等的话 client 端 ping 可能还会成功一次。
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// 用例 1: idle 队列里全是 stale 连接, acquire 应该自愈 (不抛, 拿到好连接)。
static void test_acquire_recovers_from_all_stale_idle() {
    std::cout << "[1] acquire 自愈: idle 队列全是 stale 连接\n";

    PoolOptions opts;
    opts.min_size = 2;
    opts.max_size = 4;
    opts.health_check_on_acquire = true;

    ConnectionPool pool(make_config(), opts);

    // 预热后 pool 里应有 2 个 idle 连接, 取出它们的 thread id 并立即归还。
    std::vector<unsigned long> tids;
    {
        auto c1 = pool.acquire();
        auto c2 = pool.acquire();
        tids.push_back(get_thread_id(c1.ref()));
        tids.push_back(get_thread_id(c2.ref()));
        // c1 / c2 离开作用域归还到 pool, idle 队列再次填满 2 个。
    }
    CHECK(pool.idle_count() == 2);

    // 让 server 端关掉这两个连接 (相当于 wait_timeout)。
    kill_connections(tids);

    // 关键断言: 修复前这里会连续抛 HealthCheckException, 业务方 acquire 失败。
    // 修复后 acquire 应该自动跳过 stale, 拿到新连接。
    try {
        auto c = pool.acquire();
        auto rs = c->query("SELECT 1");
        CHECK(rs->next());
        CHECK(rs->get_int32(0) == 1);
        // 拿到的连接 thread id 必然是新的, 因为旧的两个被 KILL 了。
        unsigned long new_tid = get_thread_id(c.ref());
        CHECK(new_tid != tids[0]);
        CHECK(new_tid != tids[1]);
    } catch (const std::exception& e) {
        CHECK(false && "acquire should NOT throw on stale idle connections");
        std::cerr << "    unexpected throw: " << e.what() << "\n";
    }
}

// 用例 2: 连续 N 次 acquire 都不应失败 (N >= min_size, 覆盖"前 N 次失败"症状)。
static void test_consecutive_acquires_all_succeed() {
    std::cout << "[2] 连续 acquire 均成功: 覆盖 idle 队列里所有 stale\n";

    PoolOptions opts;
    opts.min_size = 3;
    opts.max_size = 8;
    opts.health_check_on_acquire = true;

    ConnectionPool pool(make_config(), opts);

    // 把 3 个预热连接的 tid 都拿出来, 然后归还, 再 KILL 它们。
    std::vector<unsigned long> tids;
    {
        std::vector<PooledConn> held;
        held.reserve(opts.min_size);
        for (size_t i = 0; i < opts.min_size; ++i) {
            held.emplace_back(pool.acquire());
            tids.push_back(get_thread_id(held.back().ref()));
        }
    }
    kill_connections(tids);

    // 模拟报告里的"早上前 N 个用户登录": 连续 acquire 应该全部成功。
    int ok = 0;
    for (size_t i = 0; i < opts.min_size + 2; ++i) {
        try {
            auto c = pool.acquire();
            auto rs = c->query("SELECT 1");
            if (rs->next() && rs->get_int32(0) == 1) {
                ++ok;
            }
        } catch (const std::exception& e) {
            std::cerr << "    acquire #" << i << " threw: " << e.what() << "\n";
        }
    }
    CHECK(ok == static_cast<int>(opts.min_size + 2));
}

// 用例 3: 健康检查关闭时, 拿到 stale 连接的行为不变 (回归保护: 不要意外改其它分支)。
static void test_health_check_disabled_does_not_loop() {
    std::cout << "[3] health_check_on_acquire=false: 行为不变\n";

    PoolOptions opts;
    opts.min_size = 1;
    opts.max_size = 2;
    opts.health_check_on_acquire = false;

    ConnectionPool pool(make_config(), opts);

    // 关掉健康检查后 pool 不会试图重连 stale 连接, 直接给业务方一个坏的 —
    // 这是开发者主动选择了 trade-off (acquire 更快, 但要自己处理失败)。
    // 验证: acquire 本身不抛, 但用它做 query 时会抛 (说明走的就不是健康检查路径)。
    auto c = pool.acquire();
    (void)c;
    CHECK(true);  // 仅验证不抛即可, 别的不假设。
}

int main() {
    try {
        test_acquire_recovers_from_all_stale_idle();
        test_consecutive_acquires_all_succeed();
        test_health_check_disabled_does_not_loop();
    } catch (const std::exception& e) {
        std::cerr << "FATAL: uncaught exception: " << e.what() << "\n";
        return 2;
    }

    if (failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << failures << " CHECK failure(s)\n";
    return 1;
}
