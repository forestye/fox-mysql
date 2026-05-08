// 验证 bind_param<int/long long/double/float> 不再因 static thread_local
// 共享导致同类型参数互相覆盖。

#include "fox-mysql/fox-mysql.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace fox::mysql;

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
    c.stmt_cache_capacity = 16;
    return c;
}

static void test_multi_int_params(Connection& conn) {
    std::cout << "[1] multiple int params do not collide\n";
    conn.execute("DROP TABLE IF EXISTS scalar_int_test");
    conn.execute("CREATE TABLE scalar_int_test (a INT, b INT, c INT)");
    // 三个 int 参数，过去会全部塌缩为最后一个（30）。
    conn.execute_prepared("INSERT INTO scalar_int_test (a, b, c) VALUES (?, ?, ?)", 10, 20, 30);
    auto rs = conn.query("SELECT a, b, c FROM scalar_int_test");
    CHECK(rs->next());
    CHECK(rs->get_int32(0) == 10);
    CHECK(rs->get_int32(1) == 20);
    CHECK(rs->get_int32(2) == 30);
    conn.execute("DROP TABLE scalar_int_test");
}

static void test_multi_double_params(Connection& conn) {
    std::cout << "[2] multiple double params do not collide\n";
    conn.execute("DROP TABLE IF EXISTS scalar_double_test");
    conn.execute("CREATE TABLE scalar_double_test (x DOUBLE, y DOUBLE, z DOUBLE)");
    conn.execute_prepared("INSERT INTO scalar_double_test (x, y, z) VALUES (?, ?, ?)",
                          1.25, 2.5, 3.75);
    auto rs = conn.query("SELECT x, y, z FROM scalar_double_test");
    CHECK(rs->next());
    CHECK(rs->get_double(0) == 1.25);
    CHECK(rs->get_double(1) == 2.5);
    CHECK(rs->get_double(2) == 3.75);
    conn.execute("DROP TABLE scalar_double_test");
}

static void test_mixed_scalar_types(Connection& conn) {
    std::cout << "[3] mixed scalar types in one statement\n";
    conn.execute("DROP TABLE IF EXISTS scalar_mixed_test");
    conn.execute("CREATE TABLE scalar_mixed_test ("
                 " ai INT, bi INT,"
                 " al BIGINT, bl BIGINT,"
                 " ad DOUBLE, bd DOUBLE)");
    long long big1 = 4000000000LL;
    long long big2 = 9000000000LL;
    conn.execute_prepared(
        "INSERT INTO scalar_mixed_test (ai, bi, al, bl, ad, bd) VALUES (?, ?, ?, ?, ?, ?)",
        7, 8, big1, big2, 1.5, 2.5);
    auto rs = conn.query("SELECT ai, bi, al, bl, ad, bd FROM scalar_mixed_test");
    CHECK(rs->next());
    CHECK(rs->get_int32(0) == 7);
    CHECK(rs->get_int32(1) == 8);
    CHECK(rs->get_int64(2) == big1);
    CHECK(rs->get_int64(3) == big2);
    CHECK(rs->get_double(4) == 1.5);
    CHECK(rs->get_double(5) == 2.5);
    conn.execute("DROP TABLE scalar_mixed_test");
}

static void test_lifetime_across_loop(Connection& conn) {
    std::cout << "[4] scalar buffers don't collide across consecutive prepared calls\n";
    conn.execute("DROP TABLE IF EXISTS scalar_loop_test");
    conn.execute("CREATE TABLE scalar_loop_test (k INT, v INT)");
    for (int i = 1; i <= 5; ++i) {
        conn.execute_prepared("INSERT INTO scalar_loop_test (k, v) VALUES (?, ?)", i, i * 100);
    }
    auto rs = conn.query("SELECT k, v FROM scalar_loop_test ORDER BY k");
    int count = 0;
    while (rs->next()) {
        ++count;
        CHECK(rs->get_int32(1) == rs->get_int32(0) * 100);
    }
    CHECK(count == 5);
    conn.execute("DROP TABLE scalar_loop_test");
}

int main() {
    std::cout << "=== scalar_param_test ===\n";
    try {
        Connection conn(make_config());
        test_multi_int_params(conn);
        test_multi_double_params(conn);
        test_mixed_scalar_types(conn);
        test_lifetime_across_loop(conn);
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return 2;
    }
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "ALL CHECKS PASSED\n";
    return 0;
}
