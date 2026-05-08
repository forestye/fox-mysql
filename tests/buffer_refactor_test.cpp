// 验证 ResultSet stmt 路径 buffer 重构 (Bug #3 完整修复)
// 覆盖:
//   1. 跨多行 fetch buffer 地址稳定 (原 use-after-free 路径)
//   2. NULL 列在 stmt 路径下正确处理
//   3. get_row_as_vector / get_row_as_map 在 stmt 路径下不再 null-deref
//   4. MYSQL_DATA_TRUNCATED 路径: 大于默认 64KB 的列触发扩容 + rebind
//
// 注意: 该测试有意只用 1 个 int 参数，避开 bind_param<int> 的静态变量冲突 bug
// (与本次 buffer 重构无关，是预存的另一处问题)。

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

static void test_multi_row_buffer_stability(Connection& conn) {
    std::cout << "[1] multi-row stmt fetch (buffer stability)\n";
    conn.execute("DROP TABLE IF EXISTS buf_test_rows");
    conn.execute("CREATE TABLE buf_test_rows (k INT PRIMARY KEY, v VARCHAR(200))");
    for (int i = 1; i <= 20; ++i) {
        std::string val = "value-" + std::to_string(i) + "-" + std::string(i * 3, 'x');
        conn.execute_prepared("INSERT INTO buf_test_rows (k, v) VALUES (?, ?)", i, val);
    }

    auto rs = conn.query_prepared("SELECT k, v FROM buf_test_rows WHERE k >= ? ORDER BY k", 0);
    int count = 0;
    while (rs->next()) {
        ++count;
        int k = rs->get_int32(0);
        std::string v = rs->get_string(1);
        std::string expected = "value-" + std::to_string(k) + "-" + std::string(k * 3, 'x');
        CHECK(v == expected);
    }
    CHECK(count == 20);
    conn.execute("DROP TABLE buf_test_rows");
}

static void test_null_handling(Connection& conn) {
    std::cout << "[2] NULL handling in stmt path\n";
    conn.execute("DROP TABLE IF EXISTS buf_test_null");
    conn.execute("CREATE TABLE buf_test_null (k INT PRIMARY KEY, s VARCHAR(50))");
    conn.execute("INSERT INTO buf_test_null (k, s) VALUES (1, 'present'), (2, NULL), (3, 'tail')");

    auto rs = conn.query_prepared("SELECT k, s FROM buf_test_null WHERE k >= ? ORDER BY k", 0);
    int seen = 0;
    while (rs->next()) {
        ++seen;
        int k = rs->get_int32(0);
        if (k == 2) {
            CHECK(rs->is_null(1));
            CHECK(!rs->get_string_opt(1).has_value());
        } else {
            CHECK(!rs->is_null(1));
            CHECK(rs->get_string(1) == (k == 1 ? "present" : "tail"));
        }
    }
    CHECK(seen == 3);
    conn.execute("DROP TABLE buf_test_null");
}

static void test_row_as_vector_and_map(Connection& conn) {
    std::cout << "[3] get_row_as_vector / get_row_as_map on stmt result\n";
    conn.execute("DROP TABLE IF EXISTS buf_test_rowas");
    conn.execute("CREATE TABLE buf_test_rowas (k INT, name VARCHAR(50), note VARCHAR(50))");
    conn.execute("INSERT INTO buf_test_rowas VALUES (10, 'alice', 'first'), (20, 'bob', NULL)");

    auto rs = conn.query_prepared("SELECT k, name, note FROM buf_test_rowas WHERE k >= ? ORDER BY k", 0);
    CHECK(rs->next());
    auto vec = rs->get_row_as_vector();
    CHECK(vec.size() == 3);
    CHECK(vec[0] == "10");
    CHECK(vec[1] == "alice");
    CHECK(vec[2] == "first");

    auto m = rs->get_row_as_map();
    CHECK(m.at("k") == "10");
    CHECK(m.at("name") == "alice");
    CHECK(m.at("note") == "first");

    CHECK(rs->next());
    auto vec2 = rs->get_row_as_vector();
    CHECK(vec2[0] == "20");
    CHECK(vec2[1] == "bob");
    CHECK(vec2[2].empty());  // NULL → empty
    CHECK(rs->is_null(2));

    CHECK(!rs->next());
    conn.execute("DROP TABLE buf_test_rowas");
}

static void test_truncation_rebind(Connection& conn) {
    std::cout << "[4] MYSQL_DATA_TRUNCATED rebind path (>64KB column)\n";
    conn.execute("DROP TABLE IF EXISTS buf_test_big");
    // MEDIUMTEXT 上限 16MB；默认 buffer 是 min(fields_[i].length, 65535)，
    // 所以 >65535 字节的值会触发 MYSQL_DATA_TRUNCATED → rebind 路径。
    conn.execute("CREATE TABLE buf_test_big (k INT PRIMARY KEY, blob_text MEDIUMTEXT)");

    // 三行：64KB+1, 200KB, 1KB（最后一行验证 rebind 之后的 buffer 在小数据上仍然正常）
    std::string big1(65 * 1024 + 1, 'A');
    std::string big2(200 * 1024, 'B');
    std::string small(1024, 'C');
    conn.execute_prepared("INSERT INTO buf_test_big (k, blob_text) VALUES (?, ?)", 1, big1);
    conn.execute_prepared("INSERT INTO buf_test_big (k, blob_text) VALUES (?, ?)", 2, big2);
    conn.execute_prepared("INSERT INTO buf_test_big (k, blob_text) VALUES (?, ?)", 3, small);

    auto rs = conn.query_prepared("SELECT k, blob_text FROM buf_test_big WHERE k >= ? ORDER BY k", 0);
    int rows = 0;
    while (rs->next()) {
        ++rows;
        int k = rs->get_int32(0);
        std::string v = rs->get_string(1);
        if (k == 1) {
            CHECK(v.size() == big1.size());
            CHECK(v == big1);
        } else if (k == 2) {
            CHECK(v.size() == big2.size());
            CHECK(v == big2);
        } else if (k == 3) {
            CHECK(v.size() == small.size());
            CHECK(v == small);
        }
    }
    CHECK(rows == 3);
    conn.execute("DROP TABLE buf_test_big");
}

int main() {
    std::cout << "=== buffer_refactor_test ===\n";
    try {
        Connection conn(make_config());
        test_multi_row_buffer_stability(conn);
        test_null_handling(conn);
        test_row_as_vector_and_map(conn);
        test_truncation_rebind(conn);
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
