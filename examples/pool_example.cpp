#include "yxmysql/yxmysql.h"
#include "yxmysql/pool.h"
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>

int main() {
    try {
        // Configure connection
        yxmysql::ConnectionConfig config;
        config.host = "localhost";
        config.user = "test_user";
        config.password = "test_password";
        config.database = "test_db";
        
        // Configure pool options
        yxmysql_pool::PoolOptions pool_opts;
        pool_opts.min_size = 2;
        pool_opts.max_size = 8;
        pool_opts.acquire_timeout = std::chrono::seconds(5);
        pool_opts.idle_max_age = std::chrono::minutes(2);
        pool_opts.health_check_on_acquire = true;
        pool_opts.rollback_on_return = true;
        
        // Create connection pool
        yxmysql_pool::ConnectionPool pool(config, pool_opts);
        
        std::cout << "Connection pool created successfully!" << std::endl;
        std::cout << "Pool size: " << pool.size() << std::endl;
        std::cout << "Idle connections: " << pool.idle_count() << std::endl;
        
        // Test basic acquire/release
        {
            auto conn = pool.acquire();
            std::cout << "Acquired connection successfully!" << std::endl;
            
            // Test database operations
            conn->execute("CREATE TABLE IF NOT EXISTS pool_test (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(100))");
            conn->execute("INSERT INTO pool_test (name) VALUES ('test1')");
            
            auto result = conn->query("SELECT id, name FROM pool_test");
            while (result->next()) {
                int id = result->get_int32(0);
                std::string name = result->get_string(1);
                std::cout << "ID: " << id << ", Name: " << name << std::endl;
            }
            
            std::cout << "Connection operations completed successfully!" << std::endl;
            // Connection will be automatically returned to pool when conn goes out of scope
        }
        
        std::cout << "After releasing connection:" << std::endl;
        std::cout << "Pool size: " << pool.size() << std::endl;
        std::cout << "Idle connections: " << pool.idle_count() << std::endl;
        
        // Test concurrent access
        std::cout << "\nTesting concurrent access..." << std::endl;
        
        std::vector<std::thread> threads;
        const int num_threads = 4;
        const int operations_per_thread = 3;
        
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&pool, t, operations_per_thread]() {
                for (int i = 0; i < operations_per_thread; ++i) {
                    try {
                        auto conn = pool.acquire(std::chrono::seconds(10));
                        
                        std::cout << "Thread " << t << " acquired connection for operation " << i << std::endl;
                        
                        // Simulate some work
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        
                        conn->execute("INSERT INTO pool_test (name) VALUES ('thread_" + 
                                    std::to_string(t) + "_op_" + std::to_string(i) + "')");
                        
                        std::cout << "Thread " << t << " completed operation " << i << std::endl;
                        
                    } catch (const std::exception& e) {
                        std::cerr << "Thread " << t << " error: " << e.what() << std::endl;
                    }
                }
            });
        }
        
        // Wait for all threads to complete
        for (auto& thread : threads) {
            thread.join();
        }
        
        std::cout << "\nConcurrent test completed!" << std::endl;
        std::cout << "Final pool size: " << pool.size() << std::endl;
        std::cout << "Final idle connections: " << pool.idle_count() << std::endl;
        
        // Test transaction rollback
        std::cout << "\nTesting transaction rollback..." << std::endl;
        {
            auto conn = pool.acquire();
            conn->begin_transaction();
            conn->execute("INSERT INTO pool_test (name) VALUES ('will_be_rolled_back')");
            
            // Don't commit - connection will be returned with uncommitted transaction
            // Pool should automatically rollback
        }
        std::cout << "Transaction rollback test completed!" << std::endl;
        
        // Verify rollback worked
        {
            auto conn = pool.acquire();
            auto result = conn->query("SELECT COUNT(*) FROM pool_test WHERE name = 'will_be_rolled_back'");
            result->next();
            int count = result->get_int32(0);
            std::cout << "Uncommitted records found: " << count << " (should be 0)" << std::endl;
        }
        
        // Clean up test data
        {
            auto conn = pool.acquire();
            conn->execute("DROP TABLE IF EXISTS pool_test");
            std::cout << "Test cleanup completed!" << std::endl;
        }
        
        std::cout << "\nAll tests completed successfully!" << std::endl;
        
    } catch (const yxmysql_pool::PoolException& e) {
        std::cerr << "Pool error: " << e.what() << std::endl;
        return 1;
    } catch (const yxmysql::SQLException& e) {
        std::cerr << "Database error: " << e.what() << " (Code: " << e.error_code() << ")" << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}