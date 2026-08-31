cmake_minimum_required(VERSION 3.15)

project(ScratchDb LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Threads REQUIRED)

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
    src/cli/table_printer.cpp
    src/concurrency/lock_manager.cpp
    src/catalog/catalog.cpp
    src/catalog/column.cpp
    src/catalog/schema.cpp
    src/database/database.cpp
    src/db_types/date_time.cpp
    src/query/query_executor.cpp
    src/query/query_parser.cpp
    src/query/value_parser.cpp
    src/record/row.cpp
    src/record/serializer.cpp
    src/record/value.cpp
    src/storage/page.cpp
    src/storage/page_cache.cpp
    src/storage/index_file.cpp
    src/storage/table_file.cpp
    src/storage/wal_manager.cpp
)
target_include_directories(db_engine PUBLIC include)
target_link_libraries(db_engine PUBLIC Threads::Threads)

# Database usage utility terminal
add_executable(db_cli
    src/main.cpp
)
target_link_libraries(db_cli PRIVATE db_engine)

add_executable(db_server
    src/server.cpp
)
target_link_libraries(db_server PRIVATE db_engine)

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

add_executable(page_cache_tests
    tests/page_cache_test.cpp
)
target_link_libraries(page_cache_tests PRIVATE db_engine)
add_test(NAME PageCacheTests COMMAND page_cache_tests)

add_executable(index_file_tests
    tests/index_file_test.cpp
)
target_link_libraries(index_file_tests PRIVATE db_engine)
add_test(NAME IndexFileTests COMMAND index_file_tests)

add_executable(wal_manager_tests
    tests/wal_manager_test.cpp
)
target_link_libraries(wal_manager_tests PRIVATE db_engine)
add_test(NAME WalManagerTests COMMAND wal_manager_tests)

add_executable(lock_manager_tests
    tests/lock_manager_test.cpp
)
target_link_libraries(lock_manager_tests PRIVATE db_engine)
add_test(NAME LockManagerTests COMMAND lock_manager_tests)

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

add_executable(database_tests
    tests/database_test.cpp
)
target_link_libraries(database_tests PRIVATE db_engine)
add_test(NAME DatabaseTests COMMAND database_tests)

add_executable(db_types_tests
    tests/db_types_test.cpp
)
target_link_libraries(db_types_tests PRIVATE db_engine)
add_test(NAME DbTypesTests COMMAND db_types_tests)

add_executable(query_parser_tests
    tests/query_parser_test.cpp
)
target_link_libraries(query_parser_tests PRIVATE db_engine)
add_test(NAME QueryParserTests COMMAND query_parser_tests)

add_executable(value_parser_tests
    tests/value_parser_test.cpp
)
target_link_libraries(value_parser_tests PRIVATE db_engine)
add_test(NAME ValueParserTests COMMAND value_parser_tests)

add_executable(query_executor_tests
    tests/query_executor_test.cpp
)
target_link_libraries(query_executor_tests PRIVATE db_engine)
add_test(NAME QueryExecutorTests COMMAND query_executor_tests)

add_executable(table_printer_tests
    tests/table_printer_test.cpp
)
target_link_libraries(table_printer_tests PRIVATE db_engine)
add_test(NAME TablePrinterTests COMMAND table_printer_tests)

add_executable(server_tests
    tests/server_test.cpp
)
target_link_libraries(server_tests PRIVATE db_engine)
add_test(NAME ServerTests COMMAND server_tests)
