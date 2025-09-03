add_executable(compile_test compile_test.cpp)
target_link_libraries(compile_test yxmysql)

add_executable(pool_compile_test pool_compile_test.cpp)
target_link_libraries(pool_compile_test yxmysql)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_libraries(pool_compile_test pthread)
endif()

add_executable(prepared_test prepared_test.cpp)
target_link_libraries(prepared_test yxmysql)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_libraries(prepared_test pthread)
endif()