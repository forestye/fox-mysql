// 回归测试: 复现外部团队报告的 heap-use-after-free
// (docs: fox-mysql-prepared-statement-heap-corruption.md, 2026-05).
//
// 问题模式:
//   stmt 模式下 ResultSet::fetch_stmt_row 用 std::string move-assign 替换
//   string_buffers_[i]，会释放 MySQL bind 仍持有的 heap chunk；下一次
//   mysql_stmt_fetch (或同一 stmt 后续 reset+execute 周期内 libmysql 触摸
//   该 buffer) 写入已释放内存。任何含 SSO 阈值之上输出列 (DATETIME ≥ 19B,
//   BIGINT/VARCHAR(>=16B)) 的 stmt SELECT 都会触发；输入是不是 string 参数
//   并无关系 —— 报告的"prepared with string params crashes"只是巧合。
//
// 该用例 ASAN 编译运行时:
//   - 在 commit 7c48541 之前 (旧实现): 首次迭代即触发 heap-use-after-free
//   - 自 commit 7c48541 起 (vector<vector<char>> 稳定 buffer): 干净通过
// 普通构建下旧实现是否会显式崩溃依赖于 glibc 分配器布局，但任何运行下
// 当前实现都应正确返回数据，CHECK 通过。

#include "fox-mysql/fox-mysql.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

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
    c.charset = "utf8mb4";
    c.stmt_cache_capacity = 32;
    return c;
}

static void seed_schema(Connection& conn) {
    conn.execute("DROP TABLE IF EXISTS regression_history");
    conn.execute("DROP TABLE IF EXISTS regression_detail");
    conn.execute(
        "CREATE TABLE regression_history ("
        "  id BIGINT PRIMARY KEY AUTO_INCREMENT,"
        "  user_id VARCHAR(32) NOT NULL,"
        "  device_id VARCHAR(64) NOT NULL,"
        "  sleep_date DATE NOT NULL,"
        "  start_time DATETIME NULL,"
        "  end_time DATETIME NULL,"
        "  total_duration INT DEFAULT 0,"
        "  deep_sleep_duration INT DEFAULT 0,"
        "  light_sleep_duration INT DEFAULT 0,"
        "  rem_duration INT DEFAULT 0,"
        "  awake_times INT DEFAULT 0,"
        "  awake_duration INT DEFAULT 0,"
        "  sleep_quality_score INT DEFAULT 0,"
        "  UNIQUE KEY uk_user_date (user_id, sleep_date)"
        ") ENGINE=InnoDB");
    conn.execute(
        "CREATE TABLE regression_detail ("
        "  id BIGINT PRIMARY KEY AUTO_INCREMENT,"
        "  hist_id BIGINT NOT NULL,"
        "  record_time DATETIME,"
        "  status TINYINT,"
        "  INDEX idx_hist (hist_id, record_time)"
        ") ENGINE=InnoDB");
    conn.execute(
        "INSERT INTO regression_history "
        "(user_id, device_id, sleep_date, start_time, end_time, total_duration,"
        " deep_sleep_duration, light_sleep_duration, rem_duration, awake_times,"
        " awake_duration, sleep_quality_score) VALUES"
        "('regression_user', 'dev_1', '2026-05-05', '2026-05-04 22:30:00', '2026-05-05 07:14:00', 524, 130, 280, 74, 6, 40, 82),"
        "('regression_user', 'dev_1', '2026-05-04', '2026-05-03 22:40:00', '2026-05-04 07:05:00', 505, 120, 270, 70, 5, 45, 80)");
    // 给 hist_id=1 / hist_id=2 各几十条 detail，足够走多次 stmt fetch 命中 buffer 复用路径。
    // 两段 INSERT，为 hist_id=1 / hist_id=2 各填 50 条 detail。
    // a.n + b.n*10 取 0..49。
    auto seed_detail = [&conn](int hist_id, const std::string& base_dt) {
        conn.execute(
            "INSERT INTO regression_detail (hist_id, record_time, status) "
            "SELECT " + std::to_string(hist_id) + ", "
            "DATE_ADD('" + base_dt + "', INTERVAL (a.n + b.n*10) MINUTE), "
            "IF((a.n + b.n*10) % 5 = 0, 4, 2) "
            "FROM (SELECT 0 n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 "
            "      UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) a "
            "CROSS JOIN (SELECT 0 n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4) b");
    };
    seed_detail(1, "2026-05-04 22:30:00");
    seed_detail(2, "2026-05-03 22:40:00");
}

static void cleanup_schema(Connection& conn) {
    conn.execute("DROP TABLE regression_detail");
    conn.execute("DROP TABLE regression_history");
}

static void run_repro_loop(Connection& conn, int iterations) {
    std::vector<std::string> dates = {"2026-05-05", "2026-05-04"};
    std::string user_id = "regression_user";

    for (int iter = 1; iter <= iterations; ++iter) {
        for (const std::string& date : dates) {
            // (A) 两个 lvalue std::string 参数，11 列输出 (含 DATETIME 19B → heap buffer)
            auto rs = conn.query_prepared(
                "SELECT id, sleep_date, start_time, end_time, total_duration, deep_sleep_duration, "
                "light_sleep_duration, rem_duration, awake_times, awake_duration, sleep_quality_score "
                "FROM regression_history WHERE user_id = ? AND sleep_date = ? ORDER BY id DESC LIMIT 1",
                user_id, date);

            long long hist_id = 0;
            int total_duration = 0;
            std::string start_time;
            std::string end_time;
            CHECK(rs->next());
            hist_id = rs->get_int64(0);
            // 强制访问 DATETIME 列字符串 (heap buffer 路径)
            start_time = rs->get_string(2);
            end_time = rs->get_string(3);
            total_duration = rs->get_int32(4);
            CHECK(!start_time.empty());
            CHECK(!end_time.empty());

            int expected_total = (date == "2026-05-05") ? 524 : 505;
            long long expected_id = (date == "2026-05-05") ? 1 : 2;
            CHECK(hist_id == expected_id);
            CHECK(total_duration == expected_total);

            // (B) 单个数值参数, 在 (A) 之后立即触发新 stmt 的 init_stmt_binds + fetch
            auto cnt_rs = conn.query_prepared(
                "SELECT COUNT(*) FROM regression_detail "
                "WHERE hist_id = ? AND record_time IS NOT NULL", hist_id);
            CHECK(cnt_rs->next());
            long long cnt = cnt_rs->get_int64(0);
            CHECK(cnt == 50);

            // (C) 多行输出, 每行含 DATETIME；多次 fetch_stmt_row 复用同一组 buffer
            auto det_rs = conn.query_prepared(
                "SELECT record_time, status FROM regression_detail "
                "WHERE hist_id = ? ORDER BY record_time ASC LIMIT 25", hist_id);
            int rows = 0;
            while (det_rs->next()) {
                std::string ts = det_rs->get_string(0);
                int st = det_rs->get_int32(1);
                CHECK(!ts.empty());
                CHECK(st == 2 || st == 4);
                ++rows;
            }
            CHECK(rows == 25);
        }
    }
}

int main() {
    std::cout << "=== stmt_buffer_regression_test ===\n";
    try {
        Connection conn(make_config());
        seed_schema(conn);

        // 旧实现 (commit 7c48541 之前) 在 ASAN 下首次迭代即报
        // heap-use-after-free；这里跑足够多次 fetch 既覆盖 ASAN 检测，
        // 也提高在普通构建中触发崩溃的概率。
        std::cout << "[1] running 30 iterations of mixed stmt SELECTs (DATETIME columns)\n";
        run_repro_loop(conn, 30);

        cleanup_schema(conn);
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
