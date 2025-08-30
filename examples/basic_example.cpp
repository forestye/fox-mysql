#include "yx/mysql/yxmysql.h"
#include <iostream>

int main() {
    try {
        yxmysql::ConnectionConfig config;
        config.host = "localhost";
        config.user = "test";
        config.password = "test";
        config.database = "testdb";
        
        yxmysql::Connection conn(config);
        
        std::cout << "Connected to MySQL successfully!\n";
        
        conn.execute("CREATE TABLE IF NOT EXISTS users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(100), age INT)");
        
        conn.execute("INSERT INTO users (name, age) VALUES ('Alice', 25)");
        conn.execute("INSERT INTO users (name, age) VALUES ('Bob', 30)");
        
        std::cout << "Affected rows: " << conn.affected_rows() << std::endl;
        std::cout << "Last insert ID: " << conn.insert_id() << std::endl;
        
        auto result = conn.query("SELECT id, name, age FROM users");
        
        std::cout << "\nUsers in database:\n";
        while (result->next()) {
            int id = result->get_int32(0);
            std::string name = result->get_string(1);
            int age = result->get_int32(2);
            
            std::cout << "ID: " << id << ", Name: " << name << ", Age: " << age << std::endl;
        }
        
        auto result2 = conn.query("SELECT * FROM users WHERE age > 25");
        std::cout << "\nUsers older than 25:\n";
        while (result2->next()) {
            auto row_map = result2->get_row_as_map();
            for (const auto& [key, value] : row_map) {
                std::cout << key << ": " << value << " ";
            }
            std::cout << std::endl;
        }
        
        conn.begin_transaction();
        try {
            conn.execute("UPDATE users SET age = age + 1");
            conn.commit();
            std::cout << "\nTransaction committed successfully\n";
        } catch (const yxmysql::SQLException& e) {
            conn.rollback();
            std::cout << "Transaction rolled back: " << e.what() << std::endl;
        }
        
    } catch (const yxmysql::SQLException& e) {
        std::cerr << "MySQL Error: " << e.what() << " (Code: " << e.error_code() << ")" << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}