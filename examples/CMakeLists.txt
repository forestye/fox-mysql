add_executable(basic_example basic_example.cpp)
target_link_libraries(basic_example fox-mysql)

add_executable(pool_example pool_example.cpp)
target_link_libraries(pool_example fox-mysql)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_libraries(pool_example pthread)
endif()