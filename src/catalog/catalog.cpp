#include "catalog/catalog.hpp"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace {
    constexpr const char* SCHEMA_FILE_NAME = "schema.catalog";
    constexpr const char* TABLE_FILE_NAME = "data.tbl";

    bool write_all(int fd, const std::string& text) {
        const char* data = text.data();
        std::size_t remaining = text.size();

        while (remaining > 0) {
            const ssize_t written = ::write(fd, data, remaining);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (written == 0) {
                return false;
            }

            data += written;
            remaining -= static_cast<std::size_t>(written);
        }

        return true;
    }

    bool fsync_directory(const std::filesystem::path& path) {
        const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd < 0) {
            return false;
        }

        bool ok = true;
        if (::fsync(fd) != 0) {
            ok = false;
        }
        if (::close(fd) != 0) {
            ok = false;
        }

        return ok;
    }

    bool create_directory_durable(const std::filesystem::path& path) {
        std::error_code error;
        if (!std::filesystem::create_directories(path, error) || error) {
            return false;
        }

        const std::filesystem::path parent = path.parent_path();
        return parent.empty() || fsync_directory(parent);
    }

    bool create_empty_file_durable(const std::filesystem::path& path) {
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            return false;
        }

        bool ok = true;
        if (::fsync(fd) != 0) {
            ok = false;
        }
        if (::close(fd) != 0) {
            ok = false;
        }
        if (!ok) {
            std::filesystem::remove(path);
            return false;
        }

        const std::filesystem::path parent = path.parent_path();
        return parent.empty() || fsync_directory(parent);
    }

    std::string serialize_schema(const Schema& schema) {
        std::ostringstream out;

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

        return out.str();
    }

    /// @brief Write a schema file by replacing it with a fully flushed temporary file.
    /// @param path The path to write the catalog file of the table
    /// @param schema The schema to write
    /// @return False if temp write, fsync, rename, or parent directory fsync fails
    bool write_schema_file(const std::filesystem::path& path, const Schema& schema) {
        const std::filesystem::path temp_path = path.string() + ".tmp";
        const std::string text = serialize_schema(schema);

        const int fd = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            return false;
        }

        bool ok = write_all(fd, text);
        if (ok && ::fsync(fd) != 0) {
            ok = false;
        }
        if (::close(fd) != 0) {
            ok = false;
        }
        if (!ok) {
            std::filesystem::remove(temp_path);
            return false;
        }

        if (::rename(temp_path.c_str(), path.c_str()) != 0) {
            std::filesystem::remove(temp_path);
            return false;
        }

        const std::filesystem::path parent = path.parent_path();
        return parent.empty() || fsync_directory(parent);
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

    return create_directory_durable(database_path(database_name));
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
    if (!create_directory_durable(table_dir)) {
        return false;
    }

    const auto cleanup_table_dir = [&]() {
        std::filesystem::remove_all(table_dir);
        fsync_directory(table_dir.parent_path());
    };

    if (!create_empty_file_durable(table_file_path(database_name, schema.table_name()))) {
        cleanup_table_dir();
        return false;
    }

    if (!write_schema_file(schema_file_path(database_name, schema.table_name()), schema)) {
        cleanup_table_dir();
        return false;
    }

    return true;
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
