#include "catalog/catalog.hpp"

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace {
    constexpr const char* SCHEMA_FILE_NAME = "schema.catalog";
    constexpr const char* TABLE_FILE_NAME = "data.tbl";

    /// @brief Write a given schema file to the given path
    /// @param path The path to write the catalog file of the table
    /// @param schema The schema to write
    /// @return False if failed to create write stream for path
    bool write_schema_file(const std::filesystem::path& path, const Schema& schema) {
        std::ofstream out(path);
        if (!out) {
            return false;
        }

        out << "table " << schema.table_name() << '\n';
        for (const Column& column : schema.columns()) {
            out << "column "
                << column.name() << ' '
                << Column::type_to_string(column.type()) << ' '
                << (column.nullable() ? 1 : 0) << ' '
                << column.max_size() << ' '
                << static_cast<int>(column.precision()) << ' '
                << static_cast<int>(column.scale()) << '\n'
            ;
        }
        for (const ConstraintDefinition& constraint : schema.constraints()) {
            out << "constraint " << constraint.serialized() << '\n';
        }

        return static_cast<bool>(out);
    }

    /// @brief Read the schema file from given file
    /// @param path The path to the catalog file of the table
    /// @return Nullptr if unable to read file ro invalid file format
    /// @exception If Schema creation Fails from read data
    std::optional<Schema> read_schema_file(const std::filesystem::path& path) {
        std::ifstream in(path);
        if (!in) {
            return std::nullopt;
        }

        std::string line;
        std::string word;
        std::string table_name;
        if (!std::getline(in, line)) {
            return std::nullopt;
        }

        std::istringstream table_line(line);
        if (!(table_line >> word >> table_name) || word != "table") {
            return std::nullopt;
        }

        std::vector<Column> columns;
        std::vector<ConstraintDefinition> constraints;
        while (std::getline(in, line)) {
            if (line.empty()) {
                continue;
            }

            std::istringstream line_stream(line);
            if (!(line_stream >> word)) {
                return std::nullopt;
            }
            if (word == "column") {
                std::string column_name;
                std::string type_text;
                int nullable = 0;
                uint16_t max_size = 0;
                int precision = 0;
                int scale = 0;
                if (!(line_stream >> column_name >> type_text >> nullable >> max_size)) {
                    return std::nullopt;
                }
                if (line_stream >> precision) {
                    if (!(line_stream >> scale)) {
                        return std::nullopt;
                    }
                }

                ColumnType type;
                if (!Column::type_from_string(type_text, type)) {
                    return std::nullopt;
                }

                try {
                    columns.push_back(Column::from_catalog(
                        column_name,
                        type,
                        nullable != 0,
                        max_size,
                        static_cast<uint8_t>(precision),
                        static_cast<uint8_t>(scale)
                    ));
                } catch (const std::invalid_argument&) {
                    return std::nullopt;
                }
            } else if (word == "constraint") {
                std::string constraint_text;
                if (!(line_stream >> constraint_text)) {
                    return std::nullopt;
                }
                constraints.push_back(ConstraintDefinition::from_serialized(constraint_text));
            } else {
                return std::nullopt;
            }
        }

        try {
            return Schema(table_name, columns, constraints);
        } catch (const std::invalid_argument&) {
            return std::nullopt;
        }
    }

    /// @brief Get all the directories in a given folder
    /// @param path The path of the folder to list the directories from
    /// @return Empty string if path does not exist or if no subdirectories in folder
    std::vector<std::string> list_directory_names(const std::filesystem::path& path) {
        std::vector<std::string> names;
        if (!std::filesystem::exists(path)) {
            return names;
        }

        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            if (entry.is_directory()) {
                names.push_back(entry.path().filename().string());
            }
        }

        std::sort(names.begin(), names.end());
        return names;
    }
}  // namespace

/// @brief Constructor for Catalog
/// @param data_root The path to the data folder, create if does not exist
Catalog::Catalog(const std::filesystem::path& data_root) : data_root_(data_root) {
    std::filesystem::create_directories(data_root_);
}

/// @brief Create a database by creating a folder by its name
/// @param database_name The name to set for the database
/// @return False if invalid DB name
bool Catalog::create_database(const std::string& database_name) {
    if (!Schema::is_valid_table_name(database_name)) {
        return false;
    }

    return std::filesystem::create_directories(database_path(database_name));
}

/// @brief Check if DB of same name already exists
/// @param database_name The name of the databse to look for
/// @return True if exists false otherwise
bool Catalog::database_exists(const std::string& database_name) const {
    return std::filesystem::is_directory(database_path(database_name));
}

/// @brief List all the databses
/// @return List of the names of all folders in the data root
std::vector<std::string> Catalog::list_databases() const {
    return list_directory_names(data_root_);
}

/// @brief Create a new table in the given database
/// @param database_name Name of the DB in which to create the table
/// @param schema The schema of the new table
/// @return False if invalid DB or schema or table name or failed to create table folder or file True otherwise
bool Catalog::create_table(const std::string& database_name, const Schema& schema) {
    if (!database_exists(database_name) ||
        !schema.is_valid() ||
        !is_valid_new_table_name(database_name, schema.table_name())) {
        return false;
    }

    const std::filesystem::path table_dir = table_path(database_name, schema.table_name());
    if (!std::filesystem::create_directories(table_dir)) {
        return false;
    }

    std::ofstream table_file(table_file_path(database_name, schema.table_name()), std::ios::binary);
    if (!table_file) {
        return false;
    }
    table_file.close();

    return write_schema_file(schema_file_path(database_name, schema.table_name()), schema);
}

/// @brief Check if table of given name already exists in a DB
/// @param database_name The DB in which to look for the table
/// @param table_name The table name to look for
/// @return True if table exists false otherwise
bool Catalog::table_exists(const std::string& database_name, const std::string& table_name) const {
    return
        std::filesystem::is_directory(table_path(database_name, table_name)) &&
        std::filesystem::is_regular_file(schema_file_path(database_name, table_name)) &&
        std::filesystem::is_regular_file(table_file_path(database_name, table_name));
}

/// @brief Check if the given table name is valid
/// @param database_name The name of the DB in which you want to create the table
/// @param table_name The name of the table to validate
/// @return False if invalid name True otherwise
bool Catalog::is_valid_new_table_name(const std::string& database_name, const std::string& table_name) const {
    return 
        database_exists(database_name) &&
        Schema::is_valid_table_name(table_name) &&
        !std::filesystem::exists(table_path(database_name, table_name));
}

/// @brief Get the tables in a given DB
/// @param database_name The database whose table names should be listed
/// @return List of table folder names in the database
std::vector<std::string> Catalog::list_tables(const std::string& database_name) const {
    return list_directory_names(database_path(database_name));
}

/// @brief Load the schema for a table
/// @param database_name The database that contains the table
/// @param table_name The table whose schema should be loaded
/// @return Schema if the table exists and schema file is valid, std::nullopt otherwise
std::optional<Schema> Catalog::load_schema(const std::string& database_name, const std::string& table_name) const {
    if (!table_exists(database_name, table_name)) {
        return std::nullopt;
    }
    return read_schema_file(schema_file_path(database_name, table_name));
}

/// @brief Get the data file path for a table
/// @param database_name The database that contains the table
/// @param table_name The table whose data file path is needed
/// @return Path to the table data file
std::filesystem::path Catalog::table_file_path(const std::string& database_name, const std::string& table_name) const {
    return table_path(database_name, table_name) / TABLE_FILE_NAME;
}

/// @brief Get the database folder path
/// @param database_name The database name
/// @return Path to the database folder
std::filesystem::path Catalog::database_path(const std::string& database_name) const {
    return data_root_ / database_name;
}

/// @brief Get the table folder path
/// @param database_name The database that contains the table
/// @param table_name The table name
/// @return Path to the table folder
std::filesystem::path Catalog::table_path(const std::string& database_name, const std::string& table_name) const {
    return database_path(database_name) / table_name;
}

/// @brief Get the schema catalog file path for a table
/// @param database_name The database that contains the table
/// @param table_name The table name
/// @return Path to the table schema catalog file
std::filesystem::path Catalog::schema_file_path(const std::string& database_name, const std::string& table_name) const {
    return table_path(database_name, table_name) / SCHEMA_FILE_NAME;
}
