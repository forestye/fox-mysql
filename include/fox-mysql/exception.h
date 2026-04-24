#pragma once

#include <stdexcept>
#include <string>

namespace fox::mysql {

class SQLException : public std::runtime_error {
public:
    explicit SQLException(const std::string& message, unsigned int error_code = 0)
        : std::runtime_error(message), error_code_(error_code) {}
    
    explicit SQLException(const char* message, unsigned int error_code = 0)
        : std::runtime_error(message), error_code_(error_code) {}

    unsigned int error_code() const noexcept { return error_code_; }

private:
    unsigned int error_code_;
};

class ConnectionException : public SQLException {
public:
    explicit ConnectionException(const std::string& message, unsigned int error_code = 0)
        : SQLException("Connection error: " + message, error_code) {}
    
    explicit ConnectionException(const char* message, unsigned int error_code = 0)
        : SQLException(std::string("Connection error: ") + message, error_code) {}
};

class QueryException : public SQLException {
public:
    explicit QueryException(const std::string& message, unsigned int error_code = 0)
        : SQLException("Query error: " + message, error_code) {}
    
    explicit QueryException(const char* message, unsigned int error_code = 0)
        : SQLException(std::string("Query error: ") + message, error_code) {}
};

class TypeConversionException : public SQLException {
public:
    explicit TypeConversionException(const std::string& message)
        : SQLException("Type conversion error: " + message) {}
    
    explicit TypeConversionException(const char* message)
        : SQLException(std::string("Type conversion error: ") + message) {}
};

}  // namespace fox::mysql