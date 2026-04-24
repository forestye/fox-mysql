#include "fox-mysql/exception.h"
#include "fox-mysql/types.h"

int main() {
    try {
        fox::mysql::ConnectionConfig config;
        config.host = "localhost";
        config.user = "test";
        
        fox::mysql::SQLException ex("Test exception");
        fox::mysql::ConnectionException conn_ex("Connection error");
        fox::mysql::QueryException query_ex("Query error");
        fox::mysql::TypeConversionException type_ex("Type error");
        
        return 0;
    } catch (const fox::mysql::SQLException& e) {
        return 1;
    }
}