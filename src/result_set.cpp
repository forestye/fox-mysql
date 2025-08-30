#include "yxmysql/result_set.h"
#include <cstring>
#include <cstdlib>
#include <limits>

namespace yxmysql {

ResultSet::ResultSet(MYSQL_RES* result)
    : result_(result, mysql_free_result), current_row_(nullptr), 
      field_lengths_(nullptr), fields_(nullptr), has_current_row_(false) {
    if (!result_) {
        throw SQLException("Invalid result set");
    }
    
    field_count_ = mysql_num_fields(result_.get());
    fields_ = mysql_fetch_fields(result_.get());
}

ResultSet::~ResultSet() = default;

ResultSet::ResultSet(ResultSet&& other) noexcept
    : result_(std::move(other.result_)), current_row_(other.current_row_),
      field_lengths_(other.field_lengths_), fields_(other.fields_),
      field_count_(other.field_count_), has_current_row_(other.has_current_row_) {
    other.current_row_ = nullptr;
    other.field_lengths_ = nullptr;
    other.fields_ = nullptr;
    other.field_count_ = 0;
    other.has_current_row_ = false;
}

ResultSet& ResultSet::operator=(ResultSet&& other) noexcept {
    if (this != &other) {
        result_ = std::move(other.result_);
        current_row_ = other.current_row_;
        field_lengths_ = other.field_lengths_;
        fields_ = other.fields_;
        field_count_ = other.field_count_;
        has_current_row_ = other.has_current_row_;
        
        other.current_row_ = nullptr;
        other.field_lengths_ = nullptr;
        other.fields_ = nullptr;
        other.field_count_ = 0;
        other.has_current_row_ = false;
    }
    return *this;
}

bool ResultSet::next() {
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
    return current_row_[column_index];
}

unsigned long ResultSet::get_field_length(int column_index) const {
    return field_lengths_[column_index];
}

}  // namespace yxmysql