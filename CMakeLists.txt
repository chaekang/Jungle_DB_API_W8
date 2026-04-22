cmake_minimum_required(VERSION 3.16)
project(jungle_mini_db C)

set(CMAKE_C_STANDARD 11)

add_executable(
        jungle_mini_db
        main.c
        parser.c
        executor.c
        db_index.c
        thirdparty/bplustree.c
)
target_compile_definitions(jungle_mini_db PRIVATE PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}")
