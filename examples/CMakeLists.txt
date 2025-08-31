add_executable(basic_example basic_example.cpp)
target_link_libraries(basic_example yxmysql)

add_executable(pool_example pool_example.cpp)
target_link_libraries(pool_example yxmysql)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_libraries(pool_example pthread)
endif()