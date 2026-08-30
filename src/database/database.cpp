#include "database/database.hpp"

#include <sstream>
#include <stdexcept>

#include "record/serializer.hpp"
#include "storage/table_file.hpp"

/// @brief Constructor for the database engine facade
/// @param data_root The root folder where all databases are stored
/// @param database_name The selected database name for table operations
Database::Database(const std::filesystem::path& data_root, std::string database_name)
    : data_root_(data_root), catalog_(data_root), database_name_(std::move(database_name)) {}

/// @brief Create the selected database
/// @return False if the database name is invalid or already exists
bool Database::create_database() {
    return catalog_.create_database(database_name_);
}

/// @brief Check whether the selected database exists
/// @return True if the selected database folder exists
bool Database::database_exists() const {
    return catalog_.database_exists(database_name_);
}

/// @brief List all databases under the data root
/// @return Sorted list of database names
std::vector<std::string> Database::list_databases() const {
    return catalog_.list_databases();
}

/// @brief Create a table in the selected database
/// @param schema The schema of the table to create
/// @return False if the selected database does not exist or the schema/table is invalid
bool Database::create_table(const Schema& schema) {
    WalManager* wal = wal_manager();
    const uint64_t transaction_id = next_transaction_id();
    if (wal != nullptr && wal->begin_transaction(transaction_id) == 0) {
        return false;
    }
    if (wal != nullptr && wal->log_create_table(transaction_id, table_directory_path(schema.table_name()), serialize_schema(schema)) == 0) {
        rollback_statement(transaction_id);
        return false;
    }

    if (!catalog_.create_table(database_name_, schema)) {
        rollback_statement(transaction_id);
        return false;
    }

    return commit_statement(transaction_id);
}

/// @brief Check whether a table exists in the selected database
/// @param table_name The table name to check
/// @return True if the table folder, schema catalog, and data file exist
bool Database::table_exists(const std::string& table_name) const {
    return catalog_.table_exists(database_name_, table_name);
}

/// @brief List tables in the selected database
/// @return Sorted list of table names, or empty list if database does not exist
std::vector<std::string> Database::list_tables() const {
    return catalog_.list_tables(database_name_);
}

/// @brief Load the schema for a table in the selected database
/// @param table_name The table whose schema should be loaded
/// @return Schema if the table exists and has a valid schema catalog
std::optional<Schema> Database::load_schema(const std::string& table_name) const {
    return catalog_.load_schema(database_name_, table_name);
}

/// @brief Insert a row into a table
/// @param table_name The table to insert into
/// @param row The row to insert
/// @return RecordId of the inserted row, or std::nullopt on failure
std::optional<RecordId> Database::insert_row(const std::string& table_name, const Row& row) {
    std::optional<std::vector<uint8_t>> record = make_record(table_name, row);
    if (!record.has_value()) {
        return std::nullopt;
    }

    const uint64_t transaction_id = next_transaction_id();
    WalManager* wal = wal_manager();
    if (wal != nullptr && wal->begin_transaction(transaction_id) == 0) {
        return std::nullopt;
    }

    try {
        TableFile table_file(catalog_.table_file_path(database_name_, table_name), wal, transaction_id);
        std::optional<RecordId> record_id = table_file.insert_record(*record);
        if (!record_id.has_value() || !commit_statement(transaction_id)) {
            rollback_statement(transaction_id);
            table_file.discard_cache();
            return std::nullopt;
        }
        return record_id;
    } catch (const std::exception&) {
        rollback_statement(transaction_id);
        return std::nullopt;
    }
}

/// @brief Read a row from a table
/// @param table_name The table to read from
/// @param record_id The physical record identifier in the table file
/// @return Row if the record exists and can be deserialized by the table schema
std::optional<Row> Database::read_row(const std::string& table_name, RecordId record_id) const {
    std::optional<Schema> schema = load_schema(table_name);
    if (!schema.has_value()) {
        return std::nullopt;
    }

    std::vector<uint8_t> record;
    try {
        TableFile table_file(catalog_.table_file_path(database_name_, table_name), const_cast<Database*>(this)->wal_manager());
        if (!table_file.read_record(record_id, record)) {
            return std::nullopt;
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }

    Row row;
    if (!RecordSerializer::deserialize(*schema, record, row)) {
        return std::nullopt;
    }

    return row;
}

/// @brief Scan all rows from a table
/// @param table_name The table to scan
/// @param callback Called once for each decoded row. Return false to stop scanning early.
/// @return False if the table cannot be scanned or the callback stops the scan
bool Database::scan_rows(const std::string& table_name, const std::function<bool(const TableRow&)>& callback) const {
    std::optional<Schema> schema = load_schema(table_name);
    if (!schema.has_value()) {
        return false;
    }

    try {
        TableFile table_file(catalog_.table_file_path(database_name_, table_name), const_cast<Database*>(this)->wal_manager());
        return table_file.scan_records([&schema, &callback](RecordId record_id, const std::vector<uint8_t>& record) {
            Row row;
            if (RecordSerializer::deserialize(*schema, record, row)) {
                return callback(TableRow{record_id, row});
            }
            return true;
        });
    } catch (const std::exception&) {
        return false;
    }
}

/// @brief Scan all rows from a table
/// @param table_name The table to scan
/// @return Rows with their physical record ids, skipping records that cannot be decoded
std::vector<TableRow> Database::scan_rows(const std::string& table_name) const {
    std::vector<TableRow> rows;
    scan_rows(table_name, [&rows](const TableRow& row) {
        rows.push_back(row);
        return true;
    });
    return rows;
}

/// @brief Delete a row from a table
/// @param table_name The table to delete from
/// @param record_id The physical record identifier to delete
/// @return False if the table or record does not exist
bool Database::delete_row(const std::string& table_name, RecordId record_id) {
    if (!table_exists(table_name)) {
        return false;
    }

    const uint64_t transaction_id = next_transaction_id();
    WalManager* wal = wal_manager();
    if (wal != nullptr && wal->begin_transaction(transaction_id) == 0) {
        return false;
    }

    try {
        TableFile table_file(catalog_.table_file_path(database_name_, table_name), wal, transaction_id);
        if (!table_file.delete_record(record_id)) {
            rollback_statement(transaction_id);
            table_file.discard_cache();
            return false;
        }
        return commit_statement(transaction_id);
    } catch (const std::exception&) {
        rollback_statement(transaction_id);
        return false;
    }
}

/// @brief Update a row in a table
/// @param table_name The table to update
/// @param record_id The physical record identifier, updated if the row is moved
/// @param row The replacement row
/// @return False if the table, record, or replacement row is invalid
bool Database::update_row(const std::string& table_name, RecordId& record_id, const Row& row) {
    std::optional<std::vector<uint8_t>> record = make_record(table_name, row);
    if (!record.has_value()) {
        return false;
    }

    const uint64_t transaction_id = next_transaction_id();
    WalManager* wal = wal_manager();
    if (wal != nullptr && wal->begin_transaction(transaction_id) == 0) {
        return false;
    }

    try {
        TableFile table_file(catalog_.table_file_path(database_name_, table_name), wal, transaction_id);
        if (!table_file.update_record(record_id, *record)) {
            rollback_statement(transaction_id);
            table_file.discard_cache();
            return false;
        }
        return commit_statement(transaction_id);
    } catch (const std::exception&) {
        rollback_statement(transaction_id);
        return false;
    }
}

WalManager* Database::wal_manager() {
    if (!database_exists()) {
        return nullptr;
    }
    if (wal_manager_ == nullptr) {
        wal_manager_ = std::make_unique<WalManager>(data_root_ / database_name_ / "database.wal");
        wal_manager_->recover();
        if (next_transaction_id_ <= wal_manager_->last_lsn()) {
            next_transaction_id_ = wal_manager_->last_lsn() + 1;
        }
    }
    return wal_manager_.get();
}

uint64_t Database::next_transaction_id() {
    return next_transaction_id_++;
}

bool Database::commit_statement(uint64_t transaction_id) {
    WalManager* wal = wal_manager();
    if (wal == nullptr) {
        return true;
    }
    const uint64_t commit_lsn = wal->commit_transaction(transaction_id);
    return commit_lsn != 0 && wal->flush_through(commit_lsn);
}

bool Database::rollback_statement(uint64_t transaction_id) {
    WalManager* wal = wal_manager();
    if (wal == nullptr) {
        return true;
    }
    return wal->rollback_transaction(transaction_id);
}

std::string Database::serialize_schema(const Schema& schema) const {
    std::ostringstream out;
    out << "table " << schema.table_name() << '\n';
    for (const Column& column : schema.columns()) {
        out << "column "
            << column.name() << ' '
            << Column::type_to_string(column.type()) << ' '
            << (column.nullable() ? 1 : 0) << ' '
            << column.max_size() << ' '
            << static_cast<int>(column.precision()) << ' '
            << static_cast<int>(column.scale()) << '\n';
    }
    for (const ConstraintDefinition& constraint : schema.constraints()) {
        out << "constraint " << constraint.serialized() << '\n';
    }
    return out.str();
}

std::filesystem::path Database::table_directory_path(const std::string& table_name) const {
    return data_root_ / database_name_ / table_name;
}

/// @brief Build record bytes from a row using a table's schema
/// @param table_name The table whose schema should be used
/// @param row The row to convert
/// @return Record bytes if the table exists and the row matches its schema
std::optional<std::vector<uint8_t>> Database::make_record(const std::string& table_name, const Row& row) const {
    std::optional<Schema> schema = load_schema(table_name);
    if (!schema.has_value()) {
        return std::nullopt;
    }

    std::vector<uint8_t> record;
    if (!RecordSerializer::serialize(*schema, row, record)) {
        return std::nullopt;
    }

    return record;
}
