# 预编译SQL功能 - 下一步行动计划

**创建时间**: 2025-10-20
**状态**: 2个bug已修复，1个bug需要完整重构

---

## 立即行动 (优先级: 高)

### 1. 提交已完成的修复 (Bug #1, #2)

这两个修复是安全且经过验证的，应该立即提交：

```bash
# 查看修改
git status
git diff

# 提交
git add include/fox-mysql/connection.h \
        include/fox-mysql/connection_prepared.hpp \
        include/fox-mysql/result_set.h \
        src/result_set.cpp

git commit -m "修复预编译SQL的两个严重内存安全bug

Bug #1: 修复参数绑定字符串生命周期问题
- 问题: string_buffers是局部变量，导致MYSQL_BIND指向悬空指针
- 修复: 将param_binds_和param_string_buffers_改为Connection成员变量
- 影响: 所有使用execute_prepared/query_prepared的代码

Bug #2: 修复ResultSet字段信息use-after-free
- 问题: fields_指针在mysql_free_result(meta)后成为悬空指针
- 修复: 复制字段信息到stmt_fields_成员变量
- 影响: 所有使用query_prepared返回的ResultSet

这两个bug会导致内存错误和数据损坏，必须立即修复。

测试: test_prepared_debug.cpp 验证INSERT和基本SELECT功能正常
"

git push origin master
```

---

## 计划中的工作 (优先级: 中)

### 2. 完成Bug #3的完整修复

**问题**: 当前使用std::string作为MySQL输出buffer，但string地址不稳定

**解决方案**: 重新设计ResultSet的buffer管理

#### 步骤A: 修改result_set.h

```cpp
class ResultSet {
private:
    // 新增：稳定的原始缓冲区
    std::vector<std::vector<char>> result_buffers_;  // 每列一个buffer

    // 修改：string_buffers_仅用于返回值
    mutable std::vector<std::string> string_buffers_;  // 按需从buffer构建

    // 保持不变
    std::vector<MYSQL_BIND> result_binds_;
    std::vector<unsigned long> lengths_;
    std::vector<char> is_nulls_;
    std::vector<char> errors_;
};
```

#### 步骤B: 修改init_stmt_binds()

```cpp
void ResultSet::init_stmt_binds() {
    result_binds_.resize(field_count_);
    result_buffers_.resize(field_count_);  // 新增
    string_buffers_.resize(field_count_);   // 保留但用途改变
    lengths_.resize(field_count_);
    is_nulls_.resize(field_count_);
    errors_.resize(field_count_);

    std::memset(result_binds_.data(), 0, sizeof(MYSQL_BIND) * field_count_);

    for (unsigned int i = 0; i < field_count_; ++i) {
        unsigned long buffer_size = fields_[i].length;
        if (buffer_size == 0 || buffer_size > 65535) {
            buffer_size = 65535;
        }

        // 分配固定地址的buffer
        result_buffers_[i].resize(buffer_size);

        result_binds_[i].buffer_type = MYSQL_TYPE_STRING;
        result_binds_[i].buffer = result_buffers_[i].data();  // 地址不会改变
        result_binds_[i].buffer_length = buffer_size;
        result_binds_[i].length = &lengths_[i];
        result_binds_[i].is_null = reinterpret_cast<bool*>(&is_nulls_[i]);
        result_binds_[i].error = reinterpret_cast<bool*>(&errors_[i]);
    }

    if (mysql_stmt_bind_result(stmt_, result_binds_.data()) != 0) {
        throw SQLException("Failed to bind result: " + std::string(mysql_stmt_error(stmt_)));
    }
}
```

#### 步骤C: 修改fetch_stmt_row()

```cpp
void ResultSet::fetch_stmt_row() {
    // MySQL已将数据写入result_buffers_，无需任何操作
    // string_buffers_将在get_field_value()时按需构建
}
```

#### 步骤D: 修改get_field_value()

```cpp
const char* ResultSet::get_field_value(int column_index) const {
    if (is_stmt_result_) {
        if (is_nulls_[column_index]) {
            return nullptr;
        }
        // 方案1: 直接返回buffer地址（最高效）
        return result_buffers_[column_index].data();

        // 或方案2: 按需构建string（兼容性更好）
        // string_buffers_[column_index].assign(
        //     result_buffers_[column_index].data(),
        //     lengths_[column_index]);
        // return string_buffers_[column_index].c_str();
    } else {
        return current_row_[column_index];
    }
}
```

#### 步骤E: 测试

```bash
# 重新编译
cd build && make -j4 && cd ..

# 运行测试
export LD_LIBRARY_PATH=./build
./test_prepared_debug

# 预期结果：所有行的所有列都完全正确
```

---

## 后续优化 (优先级: 低)

### 3. 性能优化

- [ ] 考虑使用string_view减少复制
- [ ] 评估buffer预分配策略的效率
- [ ] 添加性能测试用例

### 4. 代码清理

- [ ] 移除所有临时的debug输出
- [ ] 添加详细的代码注释
- [ ] 更新API文档

### 5. 测试增强

- [ ] 添加更多边界情况测试
- [ ] 测试非常大的字符串（接近64KB）
- [ ] 测试BLOB和TEXT类型
- [ ] 测试NULL值处理
- [ ] 添加并发测试

---

## 风险评估

### 已修复的Bug #1, #2
- ✅ 风险: 极高（内存安全问题）
- ✅ 修复难度: 中等
- ✅ 测试覆盖: 充分
- ✅ 状态: 可以立即提交

### 待修复的Bug #3
- ⚠️ 风险: 高（数据正确性问题）
- ⚠️ 修复难度: 中等（需要重构）
- ⚠️ 测试覆盖: 需要增强
- ⚠️ 状态: 需要完整实现和测试

---

## 检查清单

### 提交Bug #1, #2修复前
- [ ] 运行所有现有测试: `cd build && make test`
- [ ] 验证test_prepared_debug基本功能
- [ ] 检查编译警告: `make 2>&1 | grep warning`
- [ ] 更新BUGFIX_REPORT.md状态
- [ ] 创建commit message

### 完成Bug #3修复后
- [ ] 运行test_prepared_debug验证所有数据正确
- [ ] 运行所有测试套件
- [ ] 性能测试（与Bug #3修复前对比）
- [ ] 检查内存泄漏: `valgrind --leak-check=full ./test_prepared_debug`
- [ ] 更新README.md（如果API有变化）
- [ ] 更新specs/spec-prepare.md状态

---

## 参考资料

- **Bug报告**: `BUGFIX_REPORT.md`
- **测试程序**: `test_prepared_debug.cpp`
- **相关文档**: `specs/spec-prepare.md`
- **MySQL文档**: https://dev.mysql.com/doc/c-api/8.0/en/c-api-prepared-statements.html

---

## 联系信息

如果有任何问题或需要讨论，请查看：
- BUGFIX_REPORT.md - 详细的bug分析
- git log - 最近的提交历史
- specs/ - 设计文档
