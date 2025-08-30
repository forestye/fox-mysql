#pragma once

#include "yx/mysql/types.h"
#include "yx/mysql/exception.h"
#include <mysql/mysql.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <optional>

namespace yxmysql {

class ResultSet {
public:
    explicit ResultSet(MYSQL_RES* result);
    ~ResultSet();

    ResultSet(const ResultSet&) = delete;
    ResultSet& operator=(const ResultSet&) = delete;
    ResultSet(ResultSet&& other) noexcept;
    ResultSet& operator=(ResultSet&& other) noexcept;

    bool next();
    
    std::string_view get_string_view(int column_index) const;
    std::string get_string(int column_index) const;
    std::optional<std::string> get_string_opt(int column_index) const;
    
    long long get_int64(int column_index) const;
    std::optional<long long> get_int64_opt(int column_index) const;
    
    int get_int32(int column_index) const;
    std::optional<int> get_int32_opt(int column_index) const;
    
    double get_double(int column_index) const;
    std::optional<double> get_double_opt(int column_index) const;
    
    bool is_null(int column_index) const;
    
    std::vector<std::string> get_row_as_vector() const;
    std::map<std::string, std::string> get_row_as_map() const;
    
    unsigned int field_count() const;
    unsigned long long row_count() const;
    
    std::string get_field_name(int column_index) const;
    FieldType get_field_type(int column_index) const;

private:
    void check_column_index(int column_index) const;
    const char* get_field_value(int column_index) const;
    unsigned long get_field_length(int column_index) const;
    
    std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result_;
    MYSQL_ROW current_row_;
    unsigned long* field_lengths_;
    MYSQL_FIELD* fields_;
    unsigned int field_count_;
    bool has_current_row_;
};

}  // namespace yxmysql