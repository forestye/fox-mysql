// 测试: execute_prepared / query_prepared 的运行时动态参数重载 (vector<Param>)。
//
// 需求背景: 变参模板版参数个数编译期固定, 无法表达动态 IN 列表
// (WHERE col IN (?,?,...,?))、按条件拼接的可选过滤、批量插入
// (INSERT ... VALUES (?,?),(?,?),...) 这类占位符个数运行时决定的查询。
// 调用方此前只能绕道 FIND_IN_SET(col, ?) 或固定占位符填哨兵, 代价是无法走索引。
//
// 语义要求 (与变参版对齐):
//   - 同一条 SQL 走同一份 stmt_cache
//   - 绑定路径复用现有 bind_param 特化 (nullptr_t → MYSQL_TYPE_NULL)
//   - params.size() 与 ? 个数不符抛 SQLException, 错误信息带两边实际个数
//   - 空 vector 合法, 等价于无参调用
//   - 重载决议: vector<Param> 的 const/非const 左值、右值都必须落到
//     vector 版, 不能被变参模板抢走 (模板对 T&/T&& 是精确匹配)

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
    return c;
}

static void setup_table(Connection& conn) {
    conn.execute("DROP TABLE IF EXISTS dyn_param_test");
    conn.execute(
        "CREATE TABLE dyn_param_test ("
        "  id INT PRIMARY KEY AUTO_INCREMENT,"
        "  category VARCHAR(32),"
        "  name VARCHAR(64),"
        "  score DOUBLE"
        ")");
}

// 生成 n 个 "?,?,...,?" 占位符
static std::string placeholders(size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += (i == 0 ? "?" : ",?");
    }
    return s;
}

// 用例 1: 批量插入 — VALUES 行数运行时决定
static void test_batch_insert(Connection& conn) {
    std::cout << "[1] 批量插入: VALUES (?,?,?),(?,?,?),... 行数运行时决定\n";

    struct Row { const char* cat; const char* name; double score; };
    std::vector<Row> rows = {
        {"fruit", "apple", 1.5}, {"fruit", "banana", 2.0}, {"fruit", "cherry", 3.5},
        {"veg", "carrot", 0.8}, {"veg", "daikon", 1.2},
    };

    std::string sql = "INSERT INTO dyn_param_test (category, name, score) VALUES ";
    std::vector<Param> params;
    for (size_t i = 0; i < rows.size(); ++i) {
        sql += (i == 0 ? "(?,?,?)" : ",(?,?,?)");
        params.emplace_back(rows[i].cat);   // const char* → std::string
        params.emplace_back(rows[i].name);
        params.emplace_back(rows[i].score); // double
    }

    conn.execute_prepared(sql, params);
    CHECK(conn.affected_rows() == rows.size());
}

// 用例 2: 动态 IN 列表 — 占位符个数随请求变化
static void test_dynamic_in_list(Connection& conn) {
    std::cout << "[2] 动态 IN: WHERE name IN (?,...,?)\n";

    std::vector<Param> wanted = {std::string("apple"), std::string("cherry"), std::string("daikon")};
    auto rs = conn.query_prepared(
        "SELECT name FROM dyn_param_test WHERE name IN (" + placeholders(wanted.size()) +
            ") ORDER BY name",
        wanted);

    std::vector<std::string> got;
    while (rs->next()) {
        got.push_back(rs->get_string(0));
    }
    CHECK(got.size() == 3);
    CHECK(got == (std::vector<std::string>{"apple", "cherry", "daikon"}));

    // 同一形态 SQL 换一个更短的列表, 走的是另一条缓存 key (占位符个数不同,
    // SQL 文本不同), 也应正常工作。
    std::vector<Param> wanted2 = {std::string("banana")};
    auto rs2 = conn.query_prepared(
        "SELECT name FROM dyn_param_test WHERE name IN (" + placeholders(wanted2.size()) + ")",
        wanted2);
    CHECK(rs2->next());
    CHECK(rs2->get_string(0) == "banana");
}

// 用例 3: 混合类型 + 可选过滤拼接 (int / double / string 同一列表)
static void test_mixed_types_optional_filters(Connection& conn) {
    std::cout << "[3] 混合类型参数: 动态拼接可选过滤\n";

    // 模拟"按条件拼 WHERE": category 过滤 + score 下限, 两个条件都启用。
    std::string sql = "SELECT COUNT(*) FROM dyn_param_test WHERE 1=1";
    std::vector<Param> params;

    sql += " AND category = ?";
    params.emplace_back(std::string("fruit"));

    sql += " AND score >= ?";
    params.emplace_back(2.0);

    auto rs = conn.query_prepared(sql, params);
    CHECK(rs->next());
    CHECK(rs->get_int64(0) == 2);  // banana(2.0), cherry(3.5)
}

// 用例 4: NULL 参数 (nullptr → MYSQL_TYPE_NULL)
static void test_null_param(Connection& conn) {
    std::cout << "[4] NULL 参数: nullptr 绑定为 SQL NULL\n";

    std::vector<Param> params;
    params.emplace_back(std::string("misc"));
    params.emplace_back(nullptr);  // name 置 NULL
    params.emplace_back(0.0);
    conn.execute_prepared(
        "INSERT INTO dyn_param_test (category, name, score) VALUES (?,?,?)", params);

    std::vector<Param> q = {std::string("misc")};
    auto rs = conn.query_prepared(
        "SELECT name FROM dyn_param_test WHERE category = ?", q);
    CHECK(rs->next());
    CHECK(rs->is_null(0));
}

// 用例 5: 参数个数不匹配 → SQLException, 错误信息带两边实际个数
static void test_count_mismatch(Connection& conn) {
    std::cout << "[5] 个数不匹配: 报错带 expected/got 实际个数\n";

    std::vector<Param> too_few = {std::string("fruit")};  // SQL 里有 2 个 ?
    bool threw = false;
    try {
        conn.query_prepared(
            "SELECT * FROM dyn_param_test WHERE category = ? AND score > ?", too_few);
    } catch (const SQLException& e) {
        threw = true;
        std::string msg = e.what();
        CHECK(msg.find("expected 2") != std::string::npos);
        CHECK(msg.find("got 1") != std::string::npos);
    }
    CHECK(threw);
}

// 用例 6: 空 vector 合法, 等价于无参调用
static void test_empty_vector(Connection& conn) {
    std::cout << "[6] 空 vector: 等价于无参调用\n";

    std::vector<Param> empty;
    auto rs = conn.query_prepared("SELECT COUNT(*) FROM dyn_param_test", empty);
    CHECK(rs->next());
    CHECK(rs->get_int64(0) >= 5);
}

// 用例 7: 重载决议 — 非const左值 / const左值 / 右值 vector<Param> 都必须
// 落到 vector 版而不是被变参模板抢走。修复点: 变参模板对 T&/T&& 是精确匹配,
// 若不 SFINAE 掉会优先于非模板的 const T&, 把整个 vector 当单参数去 bind。
static void test_overload_resolution(Connection& conn) {
    std::cout << "[7] 重载决议: 左值/const左值/右值 vector<Param> 均走 vector 版\n";

    // 非 const 左值
    std::vector<Param> mutable_params = {std::string("fruit")};
    auto rs1 = conn.query_prepared(
        "SELECT COUNT(*) FROM dyn_param_test WHERE category = ?", mutable_params);
    CHECK(rs1->next());
    CHECK(rs1->get_int64(0) == 3);

    // const 左值
    const std::vector<Param> const_params = {std::string("veg")};
    auto rs2 = conn.query_prepared(
        "SELECT COUNT(*) FROM dyn_param_test WHERE category = ?", const_params);
    CHECK(rs2->next());
    CHECK(rs2->get_int64(0) == 2);

    // 右值 (临时对象, 就地构造参数列表的写法)
    auto rs3 = conn.query_prepared(
        "SELECT COUNT(*) FROM dyn_param_test WHERE category = ?",
        std::vector<Param>{std::string("fruit")});
    CHECK(rs3->next());
    CHECK(rs3->get_int64(0) == 3);
}

// 用例 8: 变参版与 vector 版共享 stmt_cache (同一条 SQL 只 prepare 一次),
// 且重构骨架后变参版行为不变 (回归保护)。
static void test_variadic_still_works_and_shares_cache(Connection& conn) {
    std::cout << "[8] 变参版回归 + 两版共享同一条 SQL 的缓存\n";

    const char* sql = "SELECT COUNT(*) FROM dyn_param_test WHERE category = ?";

    // 先用变参版 prepare 并缓存
    auto rs1 = conn.query_prepared(sql, std::string("fruit"));
    CHECK(rs1->next());
    long long via_variadic = rs1->get_int64(0);

    // 再用 vector 版跑同一条 SQL (缓存 key 相同, 命中同一 stmt)
    std::vector<Param> params = {std::string("fruit")};
    auto rs2 = conn.query_prepared(sql, params);
    CHECK(rs2->next());
    CHECK(rs2->get_int64(0) == via_variadic);

    // 变参版各类型混合仍然正常
    conn.execute_prepared(
        "INSERT INTO dyn_param_test (category, name, score) VALUES (?,?,?)",
        "regress", std::string("vararg"), 9.9);
    auto rs3 = conn.query_prepared(
        "SELECT score FROM dyn_param_test WHERE category = ?", "regress");
    CHECK(rs3->next());
    CHECK(rs3->get_double(0) == 9.9);
}

int main() {
    try {
        Connection conn(make_config());
        setup_table(conn);

        test_batch_insert(conn);
        test_dynamic_in_list(conn);
        test_mixed_types_optional_filters(conn);
        test_null_param(conn);
        test_count_mismatch(conn);
        test_empty_vector(conn);
        test_overload_resolution(conn);
        test_variadic_still_works_and_shares_cache(conn);

        conn.execute("DROP TABLE IF EXISTS dyn_param_test");
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
