cmake_minimum_required(VERSION 3.16)
project(fox-mysql VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)

# 使用 pkg-config 创建 IMPORTED_TARGET (PkgConfig::mysqlclient), 这样:
# 1. 同机器构建/安装/消费时无需硬编码 libmysqlclient 的绝对路径；
# 2. 安装后被消费时, fox-mysqlConfig.cmake 会在消费方机器上重新解析,
#    避免不同发行版库布局差异 (/usr/lib/x86_64-linux-gnu vs /usr/lib64) 引发的链接失败。
find_package(PkgConfig REQUIRED)
pkg_check_modules(mysqlclient REQUIRED IMPORTED_TARGET mysqlclient)

file(GLOB_RECURSE SOURCES "src/*.cpp")

add_library(fox-mysql ${SOURCES})
add_library(fox-mysql::fox-mysql ALIAS fox-mysql)

target_include_directories(fox-mysql PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(fox-mysql PUBLIC PkgConfig::mysqlclient)

option(BUILD_TESTS "Build tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(BUILD_EXAMPLES "Build examples" OFF)
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

install(TARGETS fox-mysql
    EXPORT fox-mysqlTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT fox-mysqlTargets
    FILE fox-mysqlTargets.cmake
    NAMESPACE fox-mysql::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fox-mysql
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/fox-mysqlConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fox-mysqlConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/fox-mysqlConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fox-mysql
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/fox-mysqlConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/fox-mysqlConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fox-mysql
)
