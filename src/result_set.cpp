#include "yxmysql/result_set.h"
#include <cstring>
#include <cstdlib>
#include <limits>

namespace yxmysql {

ResultSet::ResultSet(MYSQL_RES* result)
    : result_(result, mysql_free_result), current_row_(nullptr), 
      field_lengths_(nullptr), stmt_(nullptr), fields_(nullptr), 
      has_current_row_(false), is_stmt_result_(false) {
    if (!result_) {
        throw SQLException("Invalid result set");
    }
    
    field_count_ = mysql_num_fields(result_.get());
    fields_ = mysql_fetch_fields(result_.get());
}

ResultSet::ResultSet(MYSQL_STMT* stmt)
    : result_(nullptr, mysql_free_result), current_row_(nullptr),
      field_lengths_(nullptr), stmt_(stmt), fields_(nullptr),
      has_current_row_(false), is_stmt_result_(true) {
    if (!stmt_) {
        throw SQLException("Invalid prepared statement");
    }
    
    // 获取结果元信息
    MYSQL_RES* meta = mysql_stmt_result_metadata(stmt_);
    if (!meta) {
        throw SQLException("Failed to get statement result metadata");
    }
    
    field_count_ = mysql_num_fields(meta);
    fields_ = mysql_fetch_fields(meta);
    
    // 立即释放元信息
    mysql_free_result(meta);
    
    // 初始化绑定
    init_stmt_binds();
}

ResultSet::~ResultSet() {
    if (is_stmt_result_ && stmt_) {
        mysql_stmt_free_result(stmt_);
    }
}

ResultSet::ResultSet(ResultSet&& other) noexcept
    : result_(std::move(other.result_)), current_row_(other.current_row_),
      field_lengths_(other.field_lengths_), stmt_(other.stmt_),
      result_binds_(std::move(other.result_binds_)),
      string_buffers_(std::move(other.string_buffers_)),
      lengths_(std::move(other.lengths_)),
      is_nulls_(std::move(other.is_nulls_)),
      errors_(std::move(other.errors_)),
      fields_(other.fields_), field_count_(other.field_count_),
      has_current_row_(other.has_current_row_),
      is_stmt_result_(other.is_stmt_result_) {
    other.current_row_ = nullptr;
    other.field_lengths_ = nullptr;
    other.stmt_ = nullptr;
    other.fields_ = nullptr;
    other.field_count_ = 0;
    other.has_current_row_ = false;
    other.is_stmt_result_ = false;
}

ResultSet& ResultSet::operator=(ResultSet&& other) noexcept {
    if (this != &other) {
        // 清理当前对象的资源
        if (is_stmt_result_ && stmt_) {
            mysql_stmt_free_result(stmt_);
        }
        
        result_ = std::move(other.result_);
        current_row_ = other.current_row_;
        field_lengths_ = other.field_lengths_;
        stmt_ = other.stmt_;
        result_binds_ = std::move(other.result_binds_);
        string_buffers_ = std::move(other.string_buffers_);
        lengths_ = std::move(other.lengths_);
        is_nulls_ = std::move(other.is_nulls_);
        errors_ = std::move(other.errors_);
        fields_ = other.fields_;
        field_count_ = other.field_count_;
        has_current_row_ = other.has_current_row_;
        is_stmt_result_ = other.is_stmt_result_;
        
        other.current_row_ = nullptr;
        other.field_lengths_ = nullptr;
        other.stmt_ = nullptr;
        other.fields_ = nullptr;
        other.field_count_ = 0;
        other.has_current_row_ = false;
        other.is_stmt_result_ = false;
    }
    return *this;
}

bool ResultSet::next() {
    if (is_stmt_result_) {
        if (!stmt_) {
            return false;
        }
        
        int fetch_result = mysql_stmt_fetch(stmt_);
        if (fetch_result == 0) {
            // 成功获取行
            fetch_stmt_row();
            has_current_row_ = true;
            return true;
        } else if (fetch_result == MYSQL_NO_DATA) {
            // 没有更多数据
            has_current_row_ = false;
            return false;
        } else if (fetch_result == MYSQL_DATA_TRUNCATED) {
            // 数据被截断，但我们仍然可以使用
            fetch_stmt_row();
            has_current_row_ = true;
            return true;
        } else {
            // 错误
            has_current_row_ = false;
            return false;
        }
    } else {
        if (!result_) {
            return false;
        }
        
        current_row_ = mysql_fetch_row(result_.get());
        if (current_row_) {
            field_lengths_ = mysql_fetch_lengths(result_.get());
            has_current_row_ = true;
            return true;
        }
        
        has_current_row_ = false;
        field_lengths_ = nullptr;
        return false;
    }
}

std::string_view ResultSet::get_string_view(int column_index) const {
    check_column_index(column_index);
    
    const char* value = get_field_value(column_index);
    if (!value) {
        throw TypeConversionException("Cannot convert NULL to string_view");
    }
    
    return std::string_view(value, get_field_length(column_index));
}

std::string ResultSet::get_string(int column_index) const {
    check_column_index(column_index);
    
    const char* value = get_field_value(column_index);
    if (!value) {
        throw TypeConversionException("Cannot convert NULL to string");
    }
    
    return std::string(value, get_field_length(column_index));
}

std::optional<std::string> ResultSet::get_string_opt(int column_index) const {
    check_column_index(column_index);
    
    const char* value = get_field_value(column_index);
    if (!value) {
        return std::nullopt;
    }
    
    return std::string(value, get_field_length(column_index));
}

long long ResultSet::get_int64(int column_index) const {
    check_column_index(column_index);
    
    const char* value = get_field_value(column_index);
    if (!value) {
        throw TypeConversionException("Cannot convert NULL to int64");
    }
    
    char* endptr;
    long long result = std::strtoll(value, &endptr, 10);
    
    if (endptr == value || *endptr != '\0') {
        throw TypeConversionException("Invalid integer format: " + std::string(value));
    }
    
    return result;
}

std::optional<long long> ResultSet::get_int64_opt(int column_index) const {
    check_column_index(column_index);
    
    const char* value = get_field_value(column_index);
    if (!value) {
        return std::nullopt;
    }
    
    char* endptr;
    long long result = std::strtoll(value, &endptr, 10);
    
    if (endptr == value || *endptr != '\0') {
        throw TypeConversionException("Invalid integer format: " + std::string(value));
    }
    
    return result;
}

int ResultSet::get_int32(int column_index) const {
    long long value = get_int64(column_index);
    
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        throw TypeConversionException("Integer value out of range for int32");
    }
    
    return static_cast<int>(value);
}

std::optional<int> ResultSet::get_int32_opt(int column_index) const {
    auto value = get_int64_opt(column_index);
    if (!value) {
        return std::nullopt;
    }
    
    if (*value < std::numeric_limits<int>::min() || *value > std::numeric_limits<int>::max()) {
        throw TypeConversionException("Integer value out of range for int32");
    }
    
    return static_cast<int>(*value);
}

double ResultSet::get_double(int column_index) const {
    check_column_index(column_index);
    
    const char* value = get_field_value(column_index);
    if (!value) {
        throw TypeConversionException("Cannot convert NULL to double");
    }
    
    char* endptr;
    double result = std::strtod(value, &endptr);
    
    if (endptr == value || *endptr != '\0') {
        throw TypeConversionException("Invalid double format: " + std::string(value));
    }
    
    return result;
}

std::optional<double> ResultSet::get_double_opt(int column_index) const {
    check_column_index(column_index);
    
    const char* value = get_field_value(column_index);
    if (!value) {
        return std::nullopt;
    }
    
    char* endptr;
    double result = std::strtod(value, &endptr);
    
    if (endptr == value || *endptr != '\0') {
        throw TypeConversionException("Invalid double format: " + std::string(value));
    }
    
    return result;
}

bool ResultSet::is_null(int column_index) const {
    check_column_index(column_index);
    return get_field_value(column_index) == nullptr;
}

std::vector<std::string> ResultSet::get_row_as_vector() const {
    if (!has_current_row_) {
        throw SQLException("No current row available");
    }
    
    std::vector<std::string> row;
    row.reserve(field_count_);
    
    for (unsigned int i = 0; i < field_count_; ++i) {
        const char* value = current_row_[i];
        if (value) {
            row.emplace_back(value, field_lengths_[i]);
        } else {
            row.emplace_back();
        }
    }
    
    return row;
}

std::map<std::string, std::string> ResultSet::get_row_as_map() const {
    if (!has_current_row_) {
        throw SQLException("No current row available");
    }
    
    std::map<std::string, std::string> row;
    
    for (unsigned int i = 0; i < field_count_; ++i) {
        std::string field_name = fields_[i].name;
        const char* value = current_row_[i];
        
        if (value) {
            row[field_name] = std::string(value, field_lengths_[i]);
        } else {
            row[field_name] = std::string();
        }
    }
    
    return row;
}

unsigned int ResultSet::field_count() const {
    return field_count_;
}

unsigned long long ResultSet::row_count() const {
    if (!result_) {
        return 0;
    }
    return mysql_num_rows(result_.get());
}

std::string ResultSet::get_field_name(int column_index) const {
    check_column_index(column_index);
    return std::string(fields_[column_index].name);
}

FieldType ResultSet::get_field_type(int column_index) const {
    check_column_index(column_index);
    return static_cast<FieldType>(fields_[column_index].type);
}

void ResultSet::check_column_index(int column_index) const {
    if (!has_current_row_) {
        throw SQLException("No current row available");
    }
    
    if (column_index < 0 || static_cast<unsigned int>(column_index) >= field_count_) {
        throw SQLException("Column index out of range: " + std::to_string(column_index));
    }
}

const char* ResultSet::get_field_value(int column_index) const {
    if (is_stmt_result_) {
        if (is_nulls_[column_index]) {
            return nullptr;
        }
        return string_buffers_[column_index].c_str();
    } else {
        return current_row_[column_index];
    }
}

unsigned long ResultSet::get_field_length(int column_index) const {
    if (is_stmt_result_) {
        return lengths_[column_index];
    } else {
        return field_lengths_[column_index];
    }
}

void ResultSet::init_stmt_binds() {
    result_binds_.resize(field_count_);
    string_buffers_.resize(field_count_);
    lengths_.resize(field_count_);
    is_nulls_.resize(field_count_);
    errors_.resize(field_count_);
    
    // 清零绑定结构
    std::memset(result_binds_.data(), 0, sizeof(MYSQL_BIND) * field_count_);
    
    for (unsigned int i = 0; i < field_count_; ++i) {
        // 为每个字段预分配缓冲区(根据字段类型和最大长度)
        unsigned long max_length = fields_[i].max_length;
        if (max_length == 0) {
            max_length = fields_[i].length;
        }
        if (max_length == 0) {
            max_length = 255; // 默认缓冲区大小
        }
        
        string_buffers_[i].resize(max_length + 1);
        
        result_binds_[i].buffer_type = MYSQL_TYPE_STRING;
        result_binds_[i].buffer = &string_buffers_[i][0];
        result_binds_[i].buffer_length = max_length;
        result_binds_[i].length = &lengths_[i];
        result_binds_[i].is_null = reinterpret_cast<bool*>(&is_nulls_[i]);
        result_binds_[i].error = reinterpret_cast<bool*>(&errors_[i]);
    }
    
    // 绑定结果
    if (mysql_stmt_bind_result(stmt_, result_binds_.data()) != 0) {
        throw SQLException("Failed to bind result: " + std::string(mysql_stmt_error(stmt_)));
    }
}

void ResultSet::fetch_stmt_row() {
    // 更新字符串缓冲区的实际长度
    for (unsigned int i = 0; i < field_count_; ++i) {
        if (!is_nulls_[i]) {
            string_buffers_[i].resize(lengths_[i]);
        } else {
            string_buffers_[i].clear();
        }
    }
}

}  // namespace yxmysql