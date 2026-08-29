cmake_minimum_required(VERSION 3.15)

project(ScratchDb LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler Flags
if(MSVC)
    add_compile_options(
        /W4
    )
else()
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

# Common Database engine
add_library(db_engine
    src/catalog/catalog.cpp
    src/catalog/column.cpp
    src/catalog/schema.cpp
    src/record/row.cpp
    src/record/serializer.cpp
    src/record/value.cpp
    src/storage/page.cpp
    src/storage/table_file.cpp
)
target_include_directories(db_engine PUBLIC include)

# Database usage utility terminal
add_executable(db_cli
    src/main.cpp
)
target_link_libraries(db_cli PRIVATE db_engine)

# Copy data folder from soruce dir to build dir
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/data DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
endif()

# Databse testing suite
enable_testing()
add_executable(page_tests
    tests/page_test.cpp
)
target_link_libraries(page_tests PRIVATE db_engine)
add_test(NAME PageTests COMMAND page_tests)

add_executable(table_file_tests
    tests/table_file_test.cpp
)
target_link_libraries(table_file_tests PRIVATE db_engine)
add_test(NAME TableFileTests COMMAND table_file_tests)

add_executable(catalog_tests
    tests/catalog_test.cpp
)
target_link_libraries(catalog_tests PRIVATE db_engine)
add_test(NAME CatalogTests COMMAND catalog_tests)

add_executable(record_tests
    tests/record_test.cpp
)
target_link_libraries(record_tests PRIVATE db_engine)
add_test(NAME RecordTests COMMAND record_tests)
