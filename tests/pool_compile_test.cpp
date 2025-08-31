#include "yxmysql/pool.h"
#include <iostream>

// Simple compilation test for the connection pool
int main() {
    try {
        // Test pool options construction
        yxmysql_pool::PoolOptions opts;
        opts.min_size = 1;
        opts.max_size = 5;
        
        // Test connection config
        yxmysql::ConnectionConfig config;
        config.host = "localhost";
        config.user = "test";
        config.password = "test";
        config.database = "test";
        
        // Test pool construction (will fail without actual MySQL, but should compile)
        try {
            yxmysql_pool::ConnectionPool pool(config, opts);
            std::cout << "Pool created successfully (unexpected in test environment)" << std::endl;
        } catch (const std::exception& e) {
            std::cout << "Expected error: " << e.what() << std::endl;
        }
        
        // Test exception types compilation
        try {
            throw yxmysql_pool::AcquireTimeoutException(std::chrono::seconds(5));
        } catch (const yxmysql_pool::PoolException& e) {
            std::cout << "Caught pool exception: " << e.what() << std::endl;
        }
        
        try {
            throw yxmysql_pool::PoolShutdownException();
        } catch (const yxmysql_pool::PoolException& e) {
            std::cout << "Caught shutdown exception: " << e.what() << std::endl;
        }
        
        try {
            throw yxmysql_pool::HealthCheckException("test failure");
        } catch (const yxmysql_pool::PoolException& e) {
            std::cout << "Caught health check exception: " << e.what() << std::endl;
        }
        
        std::cout << "Connection pool compilation test completed successfully!" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Unexpected error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}