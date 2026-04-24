# 预编译 SQL 功能 Bug 修复报告

**日期**: 2025-10-20
**会话**: 测试和修复预编译SQL功能的bug
**状态**: 进行中，已识别3个严重bug，完成2个修复

---

## 背景

用户报告在另一个项目中使用预编译SQL总是报错。经过详细测试，发现了多个严重的内存管理和数据正确性问题。

---

## 已发现的Bug

### 🔴 Bug #1: 参数绑定字符串生命周期问题 (严重 - 内存安全)

**位置**: `include/fox-mysql/connection_prepared.hpp:prepare_and_bind_params()`

**问题描述**:
```cpp
template<typename... Args>
void Connection::prepare_and_bind_params(MYSQL_STMT* stmt, Args&&... params) {
    std::vector<MYSQL_BIND> binds(param_count);
    std::vector<std::string> string_buffers;  // ← 局部变量！
    // ... 绑定参数
    bind_parameters(stmt, binds);  // MYSQL_BIND中的指针指向string_buffers
    // 函数返回后string_buffers被销毁，MYSQL_BIND中的指针变成悬空指针！
}
```

**症状**:
- INSERT语句执行失败，错误: "Incorrect string value: '\xAC\x0DW\xB2\x05\x00...'"
- 数据库收到乱码数据

**根本原因**:
- `string_buffers`是局部变量，在函数返回后被销毁
- `MYSQL_BIND`结构中的`buffer`指针还指向已释放的内存
- MySQL在`mysql_stmt_execute()`时读取无效内存

**修复方案**: ✅ 已完成
将绑定缓冲区改为Connection类的成员变量，确保生命周期正确：

```cpp
// connection.h
class Connection {
private:
    // 参数绑定缓冲区(用于保持字符串生命周期)
    std::vector<MYSQL_BIND> param_binds_;
    std::vector<std::string> param_string_buffers_;
};

// connection_prepared.hpp
template<typename... Args>
void Connection::prepare_and_bind_params(MYSQL_STMT* stmt, Args&&... params) {
    param_binds_.resize(param_count);
    param_string_buffers_.clear();
    param_string_buffers_.reserve(param_count);
    // 使用成员变量而不是局部变量
    // ...
}
```

**影响文件**:
- `include/fox-mysql/connection.h` - 添加成员变量
- `include/fox-mysql/connection_prepared.hpp` - 修改bind函数

**测试结果**: ✅ INSERT操作不再报错

---

### 🔴 Bug #2: ResultSet字段信息use-after-free (严重 - 内存安全)

**位置**: `src/result_set.cpp:ResultSet::ResultSet(MYSQL_STMT*)`

**问题描述**:
```cpp
ResultSet::ResultSet(MYSQL_STMT* stmt) {
    MYSQL_RES* meta = mysql_stmt_result_metadata(stmt_);
    field_count_ = mysql_num_fields(meta);
    fields_ = mysql_fetch_fields(meta);  // ← 指向meta内部的fields

    mysql_free_result(meta);  // ← meta被释放！
    // fields_现在是悬空指针！
}
```

**症状**:
- 查询结果中字符串被截断
- 缓冲区大小计算错误（使用了无效的`fields_[i].length`）

**根本原因**:
- `fields_`指针指向`meta`内部的字段数组
- `mysql_free_result(meta)`后，这块内存被释放
- 后续使用`fields_[i].length`读取到垃圾数据

**修复方案**: ✅ 已完成
复制字段信息到ResultSet自己的存储中：

```cpp
// result_set.h
class ResultSet {
private:
    MYSQL_FIELD* fields_;  // 指向result_中的fields或stmt_fields_的首元素
    std::vector<MYSQL_FIELD> stmt_fields_;  // 预编译语句的字段信息副本
};

// result_set.cpp
ResultSet::ResultSet(MYSQL_STMT* stmt) {
    MYSQL_RES* meta = mysql_stmt_result_metadata(stmt_);
    field_count_ = mysql_num_fields(meta);
    MYSQL_FIELD* meta_fields = mysql_fetch_fields(meta);

    // 复制字段信息到我们自己的存储中
    stmt_fields_.assign(meta_fields, meta_fields + field_count_);
    fields_ = stmt_fields_.data();

    // 释放元信息(我们已经复制了需要的数据)
    mysql_free_result(meta);
}
```

**影响文件**:
- `include/fox-mysql/result_set.h` - 添加`stmt_fields_`成员
- `src/result_set.cpp` - 修改构造函数和移动操作

**测试结果**: ✅ 缓冲区大小正确分配

---

### 🟡 Bug #3: ResultSet数据获取buffer管理问题 (严重 - 数据正确性)

**位置**: `src/result_set.cpp:init_stmt_binds(), fetch_stmt_row()`

**问题描述**:
MySQL的prepared statement需要预先绑定输出缓冲区，并且在整个fetch过程中buffer地址不能改变。
但我们使用`std::string`作为buffer，它会在某些操作（如`resize()`, `assign()`）时重新分配内存。

```cpp
void ResultSet::init_stmt_binds() {
    for (unsigned int i = 0; i < field_count_; ++i) {
        string_buffers_[i].resize(buffer_size + 1);
        result_binds_[i].buffer = &string_buffers_[i][0];  // ← 绑定string的地址
    }
    mysql_stmt_bind_result(stmt_, result_binds_.data());
}

void ResultSet::fetch_stmt_row() {
    for (unsigned int i = 0; i < field_count_; ++i) {
        string_buffers_[i].resize(lengths_[i]);  // ← 可能重新分配！
        // result_binds_[i].buffer还指向旧地址！
    }
}
```

**症状**:
- Row 2: name="ABC       " (期望 "ABCDEFGHIJ") - 尾部填充空格
- Row 3: name="This is a                    " - 数据截断且填充
- 数据库中实际数据是正确的（已验证）

**根本原因**:
1. `std::string`不保证地址稳定性
2. `resize()`如果新大小小于capacity，不会重新分配，但会在尾部填充旧数据
3. MySQL将数据写入了buffer，但string对象不知道

**当前临时修复**: ⚠️ 不完美
```cpp
void ResultSet::fetch_stmt_row() {
    for (unsigned int i = 0; i < field_count_; ++i) {
        if (!is_nulls_[i]) {
            // 每次从buffer复制到新的string
            std::string temp(static_cast<const char*>(result_binds_[i].buffer), lengths_[i]);
            string_buffers_[i] = std::move(temp);
        }
    }
}
```

**问题**:
- 每次fetch都需要复制，有性能损失
- 如果buffer地址在string操作后改变，还是有风险

**正确修复方案**: ⚠️ 待实现
使用固定地址的buffer：

```cpp
class ResultSet {
private:
    // 方案A: 使用vector<char>作为稳定buffer
    std::vector<std::vector<char>> result_buffers_;
    std::vector<std::string> string_buffers_;  // 用于返回string

    // 或方案B: 使用unique_ptr<char[]>
    std::vector<std::unique_ptr<char[]>> result_buffers_;
    std::vector<size_t> buffer_sizes_;
};

void ResultSet::init_stmt_binds() {
    for (unsigned int i = 0; i < field_count_; ++i) {
        result_buffers_[i].resize(buffer_size);
        result_binds_[i].buffer = result_buffers_[i].data();  // 地址不会改变
    }
}

void ResultSet::fetch_stmt_row() {
    for (unsigned int i = 0; i < field_count_; ++i) {
        if (!is_nulls_[i]) {
            // 从固定buffer创建string_view或复制到string
            string_buffers_[i].assign(result_buffers_[i].data(), lengths_[i]);
        }
    }
}
```

**影响文件**:
- `include/fox-mysql/result_set.h` - 重新设计buffer管理
- `src/result_set.cpp` - 重写init_stmt_binds和fetch_stmt_row

**测试结果**:
- ⚠️ 临时方案: 部分数据正确，部分仍有问题
- ⚠️ 需要完整重构

---

## 测试环境

### 测试程序
文件: `/home/yelin/code/fox-mysql/test_prepared_debug.cpp`

```cpp
// 创建表并插入不同长度的字符串
CREATE TABLE debug_test (id INT, name VARCHAR(100), description TEXT)

// 测试数据
Row 1: id=1, name="ABC" (3字符), description=长字符串(65字符)
Row 2: id=2, name="ABCDEFGHIJ" (10字符), description=长字符串(65字符)
Row 3: id=3, name=长字符串(65字符), description=长字符串(65字符)
```

### 数据库验证
```bash
mysql -u test -ptest test -e "SELECT id, name, LENGTH(name) as len FROM debug_test"
```
结果显示数据库中的数据是**完全正确**的，证明问题在读取端。

---

## 修复文件清单

### ✅ 已修复
1. `include/fox-mysql/connection.h` - 添加参数绑定缓冲区成员变量
2. `include/fox-mysql/connection_prepared.hpp` - 修改参数绑定逻辑
3. `include/fox-mysql/result_set.h` - 添加字段信息副本存储
4. `src/result_set.cpp` - 修复构造函数、移动操作、fetch逻辑

### ⚠️ 待完成
1. `include/fox-mysql/result_set.h` - 重新设计buffer管理（使用vector<char>）
2. `src/result_set.cpp` - 完整重构buffer管理代码

---

## 当前测试结果

### Bug #1 修复后
```
✅ INSERT操作成功
✅ 数据正确写入数据库
```

### Bug #2 修复后
```
✅ 缓冲区大小正确分配（VARCHAR(100) → 400字节）
✅ 不再使用悬空指针
```

### Bug #3 临时修复后
```
⚠️ Row 1: 完全正确
✅ Row 2 name: 修复成功 (ABCDEFGHIJ)
⚠️ Row 2 description: 仍有问题
⚠️ Row 3: 部分数据截断
```

---

## 下一步计划

### 优先级1: 完成Bug #3的完整修复
1. **重新设计buffer管理**
   - 将`string_buffers_`改为`vector<vector<char>> result_buffers_`
   - 在`init_stmt_binds()`中分配固定地址buffer
   - 在`fetch_stmt_row()`中从buffer创建string

2. **修改get_field_value()**
   - 从`result_buffers_`而不是`string_buffers_`读取数据
   - 需要即时创建string或返回string_view

3. **测试验证**
   - 运行`test_prepared_debug`验证所有数据正确
   - 运行原有测试套件确保没有回归

### 优先级2: 清理和优化
1. 移除临时的debug输出代码
2. 添加更详细的注释说明buffer管理的设计
3. 更新相关文档

### 优先级3: 提交修复
1. 提交Bug #1和#2的修复（已完成且稳定）
2. 完成Bug #3修复后再提交
3. 更新CHANGELOG和README

---

## 关键代码片段备份

### 当前临时的fetch_stmt_row实现
```cpp
void ResultSet::fetch_stmt_row() {
    for (unsigned int i = 0; i < field_count_; ++i) {
        if (!is_nulls_[i]) {
            std::string temp(static_cast<const char*>(result_binds_[i].buffer), lengths_[i]);
            string_buffers_[i] = std::move(temp);
        } else {
            string_buffers_[i].clear();
        }
    }
}
```

### 推荐的新buffer管理设计
```cpp
class ResultSet {
private:
    // 稳定的缓冲区，地址永不改变
    std::vector<std::vector<char>> result_buffers_;
    // 用于返回的string（按需从buffer构建）
    mutable std::vector<std::string> string_cache_;
};

void init_stmt_binds() {
    result_buffers_.resize(field_count_);
    for (unsigned int i = 0; i < field_count_; ++i) {
        result_buffers_[i].resize(buffer_size);
        result_binds_[i].buffer = result_buffers_[i].data();
    }
}

const char* get_field_value(int column_index) const {
    if (is_stmt_result_) {
        if (is_nulls_[column_index]) return nullptr;
        // 直接返回buffer地址，或者构建string
        return result_buffers_[column_index].data();
    }
}
```

---

## 相关文件位置

- 测试程序: `/home/yelin/code/fox-mysql/test_prepared_debug.cpp`
- 主要头文件: `/home/yelin/code/fox-mysql/include/fox-mysql/`
- 实现文件: `/home/yelin/code/fox-mysql/src/`
- 构建目录: `/home/yelin/code/fox-mysql/build/`

---

## 编译和测试命令

```bash
# 完全重新编译
rm -rf build/*
mkdir -p build && cd build
cmake .. -DBUILD_TESTS=ON
make -j4

# 编译测试程序
cd ..
g++ -std=c++17 -I ./include ./test_prepared_debug.cpp -L./build -lfox-mysql -lmysqlclient -o ./test_prepared_debug

# 运行测试
export LD_LIBRARY_PATH=./build
./test_prepared_debug

# 验证数据库数据
mysql -u test -ptest test -e "SELECT id, name, LENGTH(name), description, LENGTH(description) FROM debug_test"

# 清理测试表
mysql -u test -ptest test -e "DROP TABLE IF EXISTS debug_test"
```

---

## 总结

- ✅ 发现并修复了2个严重的内存安全问题（Bug #1, #2）
- ⚠️ 识别了1个数据正确性问题（Bug #3），临时修复不完美
- 📝 需要完整重构buffer管理来彻底解决Bug #3
- ⏰ 估计完成时间：2-3小时（包括测试）

**重要提示**: Bug #1和#2的修复是安全且必要的，应该立即提交。Bug #3需要更多时间来正确实现。
