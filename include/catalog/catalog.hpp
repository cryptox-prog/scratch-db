#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "catalog/schema.hpp"

class Catalog {
public:
    explicit Catalog(const std::filesystem::path& data_root);

    bool create_database(const std::string& database_name);
    bool drop_database(const std::string& database_name);
    bool database_exists(const std::string& database_name) const;
    std::vector<std::string> list_databases() const;

    bool create_table(const std::string& database_name, const Schema& schema);
    bool drop_table(const std::string& database_name, const std::string& table_name);
    bool replace_table_schema(const std::string& database_name, const Schema& schema);
    bool table_exists(const std::string& database_name, const std::string& table_name) const;
    bool is_valid_new_table_name(
        const std::string& database_name,
        const std::string& table_name
    ) const;
    std::vector<std::string> list_tables(const std::string& database_name) const;

    std::optional<Schema> load_schema(
        const std::string& database_name,
        const std::string& table_name
    ) const;

    std::filesystem::path table_file_path(
        const std::string& database_name,
        const std::string& table_name
    ) const;

private:
    std::filesystem::path database_path(const std::string& database_name) const;
    std::filesystem::path table_path(
        const std::string& database_name,
        const std::string& table_name
    ) const;
    std::filesystem::path schema_file_path(
        const std::string& database_name,
        const std::string& table_name
    ) const;

    std::filesystem::path data_root_;
};
