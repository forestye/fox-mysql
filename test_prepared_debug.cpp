#include "yxmysql/yxmysql.h"
#include <iostream>
#include <cassert>

int main() {
    try {
        yxmysql::ConnectionConfig config;
        config.host = "localhost";
        config.port = 3306;
        config.user = "test";
        config.password = "test";
        config.database = "test";

        yxmysql::Connection conn(config);
        std::cout << "Connected successfully\n";

        // 创建测试表
        conn.execute("DROP TABLE IF EXISTS debug_test");
        conn.execute("CREATE TABLE debug_test (id INT, name VARCHAR(100), description TEXT)");

        // 插入测试数据 - 使用不同长度的字符串
        std::string short_str = "ABC";
        std::string medium_str = "ABCDEFGHIJ";
        std::string long_str = "This is a very long string that should test the buffer allocation";

        conn.execute_prepared("INSERT INTO debug_test VALUES (?, ?, ?)", 1, std::string(short_str), std::string(long_str));
        conn.execute_prepared("INSERT INTO debug_test VALUES (?, ?, ?)", 2, std::string(medium_str), std::string(long_str));
        conn.execute_prepared("INSERT INTO debug_test VALUES (?, ?, ?)", 3, std::string(long_str), std::string(long_str));

        std::cout << "Data inserted\n";

        // 查询并检查结果
        auto result = conn.query_prepared("SELECT id, name, description FROM debug_test ORDER BY id");

        std::cout << "\nQuery results:\n";
        std::cout << std::string(80, '=') << "\n";

        int row_num = 0;
        while (result->next()) {
            row_num++;
            int id = result->get_int32(0);
            std::string name = result->get_string(1);
            std::string desc = result->get_string(2);

            std::cout << "Row " << row_num << ":\n";
            std::cout << "  ID: " << id << "\n";
            std::cout << "  Name: [" << name << "] (length: " << name.length() << ")\n";
            std::cout << "  Desc: [" << desc << "] (length: " << desc.length() << ")\n";
            std::cout << std::string(80, '-') << "\n";

            // 验证数据完整性
            if (id == 1) {
                if (name != short_str) {
                    std::cerr << "ERROR: Expected name '" << short_str << "' but got '" << name << "'\n";
                }
            } else if (id == 2) {
                if (name != medium_str) {
                    std::cerr << "ERROR: Expected name '" << medium_str << "' but got '" << name << "'\n";
                }
            } else if (id == 3) {
                if (name != long_str) {
                    std::cerr << "ERROR: Expected name '" << long_str << "' but got '" << name << "'\n";
                }
            }
        }

        // 暂时不清理，以便检查数据
        //conn.execute("DROP TABLE debug_test");
        std::cout << "\nTest completed - table 'debug_test' left for inspection\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
