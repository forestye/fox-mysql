#include "yxmysql/exception.h"
#include "yxmysql/types.h"

int main() {
    try {
        yxmysql::ConnectionConfig config;
        config.host = "localhost";
        config.user = "test";
        
        yxmysql::SQLException ex("Test exception");
        yxmysql::ConnectionException conn_ex("Connection error");
        yxmysql::QueryException query_ex("Query error");
        yxmysql::TypeConversionException type_ex("Type error");
        
        return 0;
    } catch (const yxmysql::SQLException& e) {
        return 1;
    }
}