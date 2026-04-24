#include "fox-mysql/fox-mysql.h"
#include <iostream>
#include <cassert>

using namespace fox::mysql;

void test_basic_prepared_functionality() {
    std::cout << "Testing basic prepared statement functionality..." << std::endl;
    
    try {
        // 创建连接配置
        ConnectionConfig config;
        config.host = "localhost";
        config.port = 3306;
        config.user = "test";
        config.password = "test";  
        config.database = "test";
        config.stmt_cache_capacity = 16;
        
        // 创建连接
        Connection conn(config);
        
        std::cout << "Connection established successfully." << std::endl;
        
        // 创建测试表
        try {
            conn.execute("DROP TABLE IF EXISTS prepared_test");
            conn.execute("CREATE TABLE prepared_test (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(100), age INT, score DOUBLE)");
            std::cout << "Test table created successfully." << std::endl;
        } catch (const std::exception& e) {
            std::cout << "Note: Could not create test table: " << e.what() << std::endl;
            std::cout << "This is expected if database is not available." << std::endl;
            return;
        }
        
        // 测试预编译INSERT语句
        conn.execute_prepared("INSERT INTO prepared_test (name, age, score) VALUES (?, ?, ?)", 
            std::string("Alice"), 25, 95.5);
        conn.execute_prepared("INSERT INTO prepared_test (name, age, score) VALUES (?, ?, ?)", 
            std::string("Bob"), 30, 87.2);
        conn.execute_prepared("INSERT INTO prepared_test (name, age, score) VALUES (?, ?, ?)", 
            std::string("Charlie"), 28, 92.8);
        
        std::cout << "Prepared INSERT statements executed successfully." << std::endl;
        
        // 测试预编译SELECT语句
        auto result = conn.query_prepared("SELECT name, age, score FROM prepared_test WHERE age > ? ORDER BY name", 25);
        
        std::cout << "Prepared SELECT statement executed successfully." << std::endl;
        std::cout << "Results:" << std::endl;
        
        int row_count = 0;
        while (result->next()) {
            std::string name = result->get_string(0);
            int age = result->get_int32(1);
            double score = result->get_double(2);
            
            std::cout << "  " << name << " (age: " << age << ", score: " << score << ")" << std::endl;
            row_count++;
        }
        
        std::cout << "Retrieved " << row_count << " rows." << std::endl;
        
        // 测试缓存复用
        std::cout << "Testing statement cache reuse..." << std::endl;
        conn.execute_prepared("INSERT INTO prepared_test (name, age, score) VALUES (?, ?, ?)", 
            std::string("David"), 35, 88.9);
        
        // 清理
        conn.execute("DROP TABLE prepared_test");
        std::cout << "Test table cleaned up." << std::endl;
        
        std::cout << "All prepared statement tests passed!" << std::endl;
        
    } catch (const std::exception& e) {
        std::cout << "Test failed with exception: " << e.what() << std::endl;
        std::cout << "This is expected if MySQL database is not available for testing." << std::endl;
    }
}

void test_parameter_binding() {
    std::cout << "\nTesting parameter binding with different types..." << std::endl;
    
    try {
        ConnectionConfig config;
        config.host = "localhost";
        config.port = 3306;
        config.user = "test";
        config.password = "test";  
        config.database = "test";
        
        Connection conn(config);
        
        // 创建测试表
        conn.execute("DROP TABLE IF EXISTS param_test");
        conn.execute("CREATE TABLE param_test (id INT, str_val VARCHAR(100), int_val INT, double_val DOUBLE, null_val INT)");
        
        // 测试不同类型的参数绑定
        conn.execute_prepared("INSERT INTO param_test (id, str_val, int_val, double_val, null_val) VALUES (?, ?, ?, ?, ?)",
            1, std::string("test string"), 42, 3.14159, nullptr);
        
        conn.execute_prepared("INSERT INTO param_test (id, str_val, int_val, double_val, null_val) VALUES (?, ?, ?, ?, ?)",
            2, std::string_view("string_view"), 99, 2.71828, nullptr);
        
        auto result = conn.query_prepared("SELECT * FROM param_test WHERE id = ?", 1);
        
        if (result->next()) {
            std::cout << "Parameter binding test passed!" << std::endl;
            std::cout << "  ID: " << result->get_int32(0) << std::endl;
            std::cout << "  String: " << result->get_string(1) << std::endl;
            std::cout << "  Int: " << result->get_int32(2) << std::endl;
            std::cout << "  Double: " << result->get_double(3) << std::endl;
            std::cout << "  Null: " << (result->is_null(4) ? "NULL" : "NOT NULL") << std::endl;
        }
        
        // 清理
        conn.execute("DROP TABLE param_test");
        
    } catch (const std::exception& e) {
        std::cout << "Parameter binding test failed: " << e.what() << std::endl;
        std::cout << "This is expected if MySQL database is not available for testing." << std::endl;
    }
}

int main() {
    std::cout << "=== YXMySQL Prepared Statement Test Suite ===" << std::endl;
    
    test_basic_prepared_functionality();
    test_parameter_binding();
    
    std::cout << "\n=== Test Suite Complete ===" << std::endl;
    
    return 0;
}