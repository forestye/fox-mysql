#include "fox-mysql/result_set.h"
#include <cstring>
#include <cstdlib>
#include <limits>

namespace fox::mysql {

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
    MYSQL_FIELD* meta_fields = mysql_fetch_fields(meta);

    // 复制字段信息到我们自己的存储中
    stmt_fields_.assign(meta_fields, meta_fields + field_count_);
    fields_ = stmt_fields_.data();

    // 释放元信息(我们已经复制了需要的数据)
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
      result_buffers_(std::move(other.result_buffers_)),
      lengths_(std::move(other.lengths_)),
      is_nulls_(std::move(other.is_nulls_)),
      errors_(std::move(other.errors_)),
      stmt_fields_(std::move(other.stmt_fields_)),
      field_count_(other.field_count_),
      has_current_row_(other.has_current_row_),
      is_stmt_result_(other.is_stmt_result_) {
    // 如果是预编译语句结果，fields_需要指向新的stmt_fields_
    if (is_stmt_result_ && !stmt_fields_.empty()) {
        fields_ = stmt_fields_.data();
    } else {
        fields_ = other.fields_;
    }

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
        result_buffers_ = std::move(other.result_buffers_);
        lengths_ = std::move(other.lengths_);
        is_nulls_ = std::move(other.is_nulls_);
        errors_ = std::move(other.errors_);
        stmt_fields_ = std::move(other.stmt_fields_);
        field_count_ = other.field_count_;
        has_current_row_ = other.has_current_row_;
        is_stmt_result_ = other.is_stmt_result_;

        // 如果是预编译语句结果，fields_需要指向新的stmt_fields_
        if (is_stmt_result_ && !stmt_fields_.empty()) {
            fields_ = stmt_fields_.data();
        } else {
            fields_ = other.fields_;
        }

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
            // 数据被截断，重新分配 buffer 并 refetch 被截断的列。
            // lengths_[i] 是 MySQL 报告的实际数据长度（与 buffer_length 无关），
            // 我们扩容到 actual_length+1，然后用 fetch_column 从偏移 0 开始重读。
            // 注意：vector 扩容会改变 .data() 地址，而 mysql_stmt_bind_result 在内部
            // 拷贝过 bind 数组，不会同步看到我们修改后的指针；必须在所有 buffer
            // 调整完后重新调用 mysql_stmt_bind_result，否则下一行 fetch 会写入已释放内存。
            bool rebind_needed = false;
            for (unsigned int i = 0; i < field_count_; ++i) {
                if (errors_[i]) {
                    unsigned long actual_length = lengths_[i];
                    result_buffers_[i].assign(actual_length + 1, '\0');

                    result_binds_[i].buffer = result_buffers_[i].data();
                    result_binds_[i].buffer_length = actual_length;
                    rebind_needed = true;

                    if (mysql_stmt_fetch_column(stmt_, &result_binds_[i], i, 0) != 0) {
                        throw SQLException("Failed to fetch truncated column: " + std::string(mysql_stmt_error(stmt_)));
                    }
                    errors_[i] = 0;
                }
            }
            if (rebind_needed) {
                if (mysql_stmt_bind_result(stmt_, result_binds_.data()) != 0) {
                    throw SQLException("Failed to rebind result after truncation: " + std::string(mysql_stmt_error(stmt_)));
                }
            }
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
        if (is_stmt_result_) {
            if (is_nulls_[i]) {
                row.emplace_back();
            } else {
                row.emplace_back(result_buffers_[i].data(), lengths_[i]);
            }
        } else {
            const char* value = current_row_[i];
            if (value) {
                row.emplace_back(value, field_lengths_[i]);
            } else {
                row.emplace_back();
            }
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

        if (is_stmt_result_) {
            if (is_nulls_[i]) {
                row[field_name] = std::string();
            } else {
                row[field_name] = std::string(result_buffers_[i].data(), lengths_[i]);
            }
        } else {
            const char* value = current_row_[i];
            if (value) {
                row[field_name] = std::string(value, field_lengths_[i]);
            } else {
                row[field_name] = std::string();
            }
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
        return result_buffers_[column_index].data();
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
    result_buffers_.resize(field_count_);
    lengths_.resize(field_count_);
    is_nulls_.resize(field_count_);
    errors_.resize(field_count_);

    std::memset(result_binds_.data(), 0, sizeof(MYSQL_BIND) * field_count_);

    for (unsigned int i = 0; i < field_count_; ++i) {
        // fields_[i].length 是字段定义的最大长度（如 VARCHAR(100) 的 100）。
        // 对于 BLOB/TEXT 或长度不明的字段使用 64KB 默认值；
        // 超长的列会触发 MYSQL_DATA_TRUNCATED，由 next() 中的截断分支扩容并 refetch。
        unsigned long buffer_size = fields_[i].length;
        if (buffer_size == 0 || buffer_size > 65535) {
            buffer_size = 65535;
        }

        // 多分配 1 字节用于 fetch 后写入 '\0'，让 strtoll/strtod 等 C 字符串 API 可直接解析。
        result_buffers_[i].assign(buffer_size + 1, '\0');

        result_binds_[i].buffer_type = MYSQL_TYPE_STRING;
        result_binds_[i].buffer = result_buffers_[i].data();
        result_binds_[i].buffer_length = buffer_size;
        result_binds_[i].length = &lengths_[i];
        result_binds_[i].is_null = reinterpret_cast<bool*>(&is_nulls_[i]);
        result_binds_[i].error = reinterpret_cast<bool*>(&errors_[i]);
    }

    if (mysql_stmt_bind_result(stmt_, result_binds_.data()) != 0) {
        throw SQLException("Failed to bind result: " + std::string(mysql_stmt_error(stmt_)));
    }
}

void ResultSet::fetch_stmt_row() {
    // MySQL 直接写入 result_buffers_，无需复制；只需把数据末尾置零，
    // 让 get_field_value 返回的指针对 strtoll/strtod 安全。
    for (unsigned int i = 0; i < field_count_; ++i) {
        if (is_nulls_[i]) {
            continue;
        }
        unsigned long len = lengths_[i];
        // 截断分支已在 next() 中处理并清掉 errors_；走到这里时 buffer 一定能容纳 len+1。
        if (len >= result_buffers_[i].size()) {
            len = result_buffers_[i].size() - 1;
        }
        result_buffers_[i][len] = '\0';
    }
}

}  // namespace fox::mysql