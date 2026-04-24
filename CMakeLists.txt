cmake_minimum_required(VERSION 3.16)
project(fox-mysql VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_path(MYSQL_INCLUDE_DIR mysql/mysql.h
    HINTS /usr/include /usr/local/include
)

find_library(MYSQL_LIBRARY NAMES mysqlclient
    HINTS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu
)

if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
    message(STATUS "MySQL not found, creating header-only version for testing")
    set(MYSQL_FOUND FALSE)
else()
    set(MYSQL_FOUND TRUE)
    set(MYSQL_INCLUDE_DIRS ${MYSQL_INCLUDE_DIR})
    set(MYSQL_LIBRARIES ${MYSQL_LIBRARY})
endif()

include_directories(include)

file(GLOB_RECURSE SOURCES "src/*.cpp")

if(MYSQL_FOUND)
    add_library(fox-mysql ${SOURCES})
    target_include_directories(fox-mysql PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        ${MYSQL_INCLUDE_DIRS}
    )
    target_link_libraries(fox-mysql ${MYSQL_LIBRARIES})
else()
    # Header-only version for testing compilation
    add_library(fox-mysql INTERFACE)
    target_include_directories(fox-mysql INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
endif()

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
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include)

install(EXPORT fox-mysqlTargets
    FILE fox-mysqlTargets.cmake
    NAMESPACE fox-mysql::
    DESTINATION lib/cmake/fox-mysql
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "fox-mysqlConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fox-mysqlConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/fox-mysqlConfig.cmake"
    INSTALL_DESTINATION lib/cmake/fox-mysql
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/fox-mysqlConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/fox-mysqlConfigVersion.cmake"
    DESTINATION lib/cmake/fox-mysql
)