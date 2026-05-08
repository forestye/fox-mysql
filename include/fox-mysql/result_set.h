#pragma once

#include "fox-mysql/types.h"
#include "fox-mysql/exception.h"
#include <mysql/mysql.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <optional>

namespace fox::mysql {

class ResultSet {
public:
    explicit ResultSet(MYSQL_RES* result);
    explicit ResultSet(MYSQL_STMT* stmt);
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
    
    // 预编译语句相关方法
    void init_stmt_binds();
    void fetch_stmt_row();
    
    // 传统结果集成员
    std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result_;
    MYSQL_ROW current_row_;
    unsigned long* field_lengths_;
    
    // 预编译语句成员
    MYSQL_STMT* stmt_;
    std::vector<MYSQL_BIND> result_binds_;
    // 稳定地址缓冲区：vector<char> 在不 resize 时 .data() 不会失效，
    // 所以 result_binds_[i].buffer 始终指向有效内存。
    // 每个 buffer 多分配 1 字节用于在 fetch 后写入 '\0'，方便 strtoll/strtod 解析。
    std::vector<std::vector<char>> result_buffers_;
    std::vector<unsigned long> lengths_;
    std::vector<char> is_nulls_;
    std::vector<char> errors_;
    
    // 共用成员
    MYSQL_FIELD* fields_;  // 指向result_中的fields或stmt_fields_的首元素
    std::vector<MYSQL_FIELD> stmt_fields_;  // 预编译语句的字段信息副本
    unsigned int field_count_;
    bool has_current_row_;
    bool is_stmt_result_;
};

}  // namespace fox::mysql