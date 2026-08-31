#include "database/database.hpp"

#include <chrono>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "record/serializer.hpp"
#include "storage/index_file.hpp"
#include "storage/table_file.hpp"

namespace {
    constexpr std::chrono::milliseconds TABLE_LOCK_TIMEOUT{100};

    void append_int64_key(std::vector<uint8_t>& key, int64_t value) {
        uint64_t sortable = static_cast<uint64_t>(value) ^ (uint64_t{1} << 63);
        for (int shift = 56; shift >= 0; shift -= 8) {
            key.push_back(static_cast<uint8_t>((sortable >> shift) & 0xff));
        }
    }

    void append_uint32_key(std::vector<uint8_t>& key, uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            key.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
        }
    }

    struct MemoryTableStore {
        uint32_t next_page_id = 1;
        std::vector<std::optional<Row>> rows;
    };

    std::mutex memory_tables_mutex;
    std::unordered_map<std::string, MemoryTableStore> memory_tables;
}

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

bool Database::drop_database() {
    if (in_transaction()) {
        return false;
    }
    wal_manager_.reset();
    {
        std::lock_guard<std::mutex> lock(memory_tables_mutex);
        const std::string prefix = (data_root_ / database_name_).lexically_normal().string() + "/";
        for (auto it = memory_tables.begin(); it != memory_tables.end();) {
            if (it->first.rfind(prefix, 0) == 0) {
                it = memory_tables.erase(it);
            } else {
                ++it;
            }
        }
    }
    return catalog_.drop_database(database_name_);
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
    if (!acquire_transaction_lock(schema.table_name(), TableLockMode::exclusive)) {
        return false;
    }
    LockManager::TableLock table_lock = acquire_statement_lock(schema.table_name(), TableLockMode::exclusive);
    WalManager* wal = wal_manager();
    const uint64_t transaction_id = current_transaction_id();
    if (!in_transaction() && wal != nullptr && wal->begin_transaction(transaction_id) == 0) {
        return false;
    }
    if (wal != nullptr && wal->log_create_table(transaction_id, table_directory_path(schema.table_name()), serialize_schema(schema)) == 0) {
        abort_statement(transaction_id);
        return false;
    }

    if (!catalog_.create_table(database_name_, schema)) {
        abort_statement(transaction_id);
        return false;
    }

    const bool finished = finish_statement(transaction_id);
    if (finished && schema.storage_mode() == TableStorageMode::memory) {
        std::lock_guard<std::mutex> lock(memory_tables_mutex);
        memory_tables.erase(memory_table_key(schema.table_name()));
    }
    return finished;
}

bool Database::drop_table(const std::string& table_name) {
    if (!acquire_transaction_lock(table_name, TableLockMode::exclusive)) {
        return false;
    }
    LockManager::TableLock table_lock = acquire_statement_lock(table_name, TableLockMode::exclusive);
    if (!catalog_.table_exists(database_name_, table_name)) {
        return false;
    }

    const uint64_t transaction_id = current_transaction_id();
    WalManager* wal = wal_manager();
    if (!in_transaction() && wal != nullptr && wal->begin_transaction(transaction_id) == 0) {
        return false;
    }
    if (wal != nullptr && wal->log_drop_table(transaction_id, table_directory_path(table_name)) == 0) {
        abort_statement(transaction_id);
        return false;
    }
    if (!catalog_.drop_table(database_name_, table_name)) {
        abort_statement(transaction_id);
        return false;
    }
    const bool finished = finish_statement(transaction_id);
    if (finished) {
        std::lock_guard<std::mutex> lock(memory_tables_mutex);
        memory_tables.erase(memory_table_key(table_name));
    }
    return finished;
}

/// @brief Check whether a table exists in the selected database
/// @param table_name The table name to check
/// @return True if the table folder, schema catalog, and data file exist
bool Database::table_exists(const std::string& table_name) const {
    if (!acquire_transaction_lock(table_name, TableLockMode::shared)) {
        return false;
    }
    LockManager::TableLock table_lock = acquire_statement_lock(table_name, TableLockMode::shared);
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
    if (!acquire_transaction_lock(table_name, TableLockMode::shared)) {
        return std::nullopt;
    }
    LockManager::TableLock table_lock = acquire_statement_lock(table_name, TableLockMode::shared);
    return catalog_.load_schema(database_name_, table_name);
}

/// @brief Insert a row into a table
/// @param table_name The table to insert into
/// @param row The row to insert
/// @return RecordId of the inserted row, or std::nullopt on failure
std::optional<RecordId> Database::insert_row(const std::string& table_name, const Row& row) {
    if (!acquire_transaction_lock(table_name, TableLockMode::exclusive)) {
        return std::nullopt;
    }
    LockManager::TableLock table_lock = acquire_statement_lock(table_name, TableLockMode::exclusive);
    std::optional<std::vector<uint8_t>> record = make_record(table_name, row);
    if (!record.has_value()) {
        return std::nullopt;
    }
    std::optional<Schema> schema = catalog_.load_schema(database_name_, table_name);
    if (schema.has_value() && schema->storage_mode() == TableStorageMode::memory) {
        return insert_memory_row(table_name, row);
    }

    const uint64_t transaction_id = current_transaction_id();
    WalManager* wal = wal_manager();
    if (!in_transaction() && wal != nullptr && wal->begin_transaction(transaction_id) == 0) {
        return std::nullopt;
    }

    try {
        TableFile table_file(catalog_.table_file_path(database_name_, table_name), wal, transaction_id);
        std::optional<RecordId> record_id = table_file.insert_record(*record);
        if (record_id.has_value() && schema.has_value()) {
            for (const IndexDefinition& index : schema->indexes()) {
                const int column_index = schema->column_index(index.columns[0]);
                const Column* column = schema->column(static_cast<uint16_t>(column_index));
                const Value* value = row.value(static_cast<uint16_t>(column_index));
                std::optional<std::vector<uint8_t>> key = column == nullptr || value == nullptr ? std::nullopt : make_index_key(*column, *value);
                if (!key.has_value() || !IndexFile(index_file_path(table_name, index), wal, transaction_id).insert(*key, *record_id, index.unique)) {
                    record_id = std::nullopt;
                    break;
                }
            }
        }
        if (!record_id.has_value() || !finish_statement(transaction_id)) {
            abort_statement(transaction_id);
            table_file.discard_cache();
            return std::nullopt;
        }
        return record_id;
    } catch (const std::exception&) {
        abort_statement(transaction_id);
        return std::nullopt;
    }
}

/// @brief Read a row from a table
/// @param table_name The table to read from
/// @param record_id The physical record identifier in the table file
/// @return Row if the record exists and can be deserialized by the table schema
std::optional<Row> Database::read_row(const std::string& table_name, RecordId record_id) const {
    if (!acquire_transaction_lock(table_name, TableLockMode::shared)) {
        return std::nullopt;
    }
    LockManager::TableLock table_lock = acquire_statement_lock(table_name, TableLockMode::shared);
    std::optional<Schema> schema = catalog_.load_schema(database_name_, table_name);
    if (!schema.has_value()) {
        return std::nullopt;
    }

    std::vector<uint8_t> record;
    try {
        if (schema->storage_mode() == TableStorageMode::memory) {
            return read_memory_row(table_name, record_id);
        }
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
    if (!acquire_transaction_lock(table_name, TableLockMode::shared)) {
        return false;
    }
    LockManager::TableLock table_lock = acquire_statement_lock(table_name, TableLockMode::shared);
    std::optional<Schema> schema = catalog_.load_schema(database_name_, table_name);
    if (!schema.has_value()) {
        return false;
    }

    try {
        if (schema->storage_mode() == TableStorageMode::memory) {
            return scan_memory_rows(table_name, callback);
        }
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
    if (!acquire_transaction_lock(table_name, TableLockMode::exclusive)) {
        return false;
    }
    LockManager::TableLock table_lock = acquire_statement_lock(table_name, TableLockMode::exclusive);
    if (!catalog_.table_exists(database_name_, table_name)) {
        return false;
    }
    std::optional<Schema> schema = catalog_.load_schema(database_name_, table_name);
    if (schema.has_value() && schema->storage_mode() == TableStorageMode::memory) {
        return delete_memory_row(table_name, record_id);
    }

    const uint64_t transaction_id = current_transaction_id();
    WalManager* wal = wal_manager();
    if (!in_transaction() && wal != nullptr && wal->begin_transaction(transaction_id) == 0) {
        return false;
    }

    try {
        TableFile table_file(catalog_.table_file_path(database_name_, table_name), wal, transaction_id);
        std::optional<Row> old_row;
        std::vector<uint8_t> old_record;
        if (schema.has_value() && table_file.read_record(record_id, old_record)) {
            Row decoded;
            if (RecordSerializer::deserialize(*schema, old_record, decoded)) {
                old_row = decoded;
            }
        }
        if (!table_file.delete_record(record_id)) {
            abort_statement(transaction_id);
            table_file.discard_cache();
            return false;
        }
        if (schema.has_value() && old_row.has_value()) {
            for (const IndexDefinition& index : schema->indexes()) {
                const int column_index = schema->column_index(index.columns[0]);
                const Column* column = schema->column(static_cast<uint16_t>(column_index));
                const Value* value = old_row->value(static_cast<uint16_t>(column_index));
                std::optional<std::vector<uint8_t>> key = column == nullptr || value == nullptr ? std::nullopt : make_index_key(*column, *value);
                if (key.has_value()) {
                    IndexFile(index_file_path(table_name, index), wal, transaction_id).remove(*key, record_id);
                }
            }
        }
        return finish_statement(transaction_id);
    } catch (const std::exception&) {
        abort_statement(transaction_id);
        return false;
    }
}

/// @brief Update a row in a table
/// @param table_name The table to update
/// @param record_id The physical record identifier, updated if the row is moved
/// @param row The replacement row
/// @return False if the table, record, or replacement row is invalid
bool Database::update_row(const std::string& table_name, RecordId& record_id, const Row& row) {
    if (!acquire_transaction_lock(table_name, TableLockMode::exclusive)) {
        return false;
    }
    LockManager::TableLock table_lock = acquire_statement_lock(table_name, TableLockMode::exclusive);
    std::optional<std::vector<uint8_t>> record = make_record(table_name, row);
    if (!record.has_value()) {
        return false;
    }
    std::optional<Schema> schema = catalog_.load_schema(database_name_, table_name);
    if (schema.has_value() && schema->storage_mode() == TableStorageMode::memory) {
        return update_memory_row(table_name, record_id, row);
    }

    const uint64_t transaction_id = current_transaction_id();
    WalManager* wal = wal_manager();
    if (!in_transaction() && wal != nullptr && wal->begin_transaction(transaction_id) == 0) {
        return false;
    }

    try {
        TableFile table_file(catalog_.table_file_path(database_name_, table_name), wal, transaction_id);
        RecordId old_record_id = record_id;
        std::optional<Row> old_row;
        std::vector<uint8_t> old_record;
        if (schema.has_value() && table_file.read_record(record_id, old_record)) {
            Row decoded;
            if (RecordSerializer::deserialize(*schema, old_record, decoded)) {
                old_row = decoded;
            }
        }
        if (!table_file.update_record(record_id, *record)) {
            abort_statement(transaction_id);
            table_file.discard_cache();
            return false;
        }
        if (schema.has_value()) {
            for (const IndexDefinition& index : schema->indexes()) {
                const int column_index = schema->column_index(index.columns[0]);
                const Column* column = schema->column(static_cast<uint16_t>(column_index));
                if (column == nullptr) {
                    continue;
                }
                if (old_row.has_value()) {
                    const Value* old_value = old_row->value(static_cast<uint16_t>(column_index));
                    std::optional<std::vector<uint8_t>> old_key = old_value == nullptr ? std::nullopt : make_index_key(*column, *old_value);
                    if (old_key.has_value()) {
                        IndexFile(index_file_path(table_name, index), wal, transaction_id).remove(*old_key, old_record_id);
                    }
                }
                const Value* new_value = row.value(static_cast<uint16_t>(column_index));
                std::optional<std::vector<uint8_t>> new_key = new_value == nullptr ? std::nullopt : make_index_key(*column, *new_value);
                if (!new_key.has_value() || !IndexFile(index_file_path(table_name, index), wal, transaction_id).insert(*new_key, record_id, index.unique)) {
                    abort_statement(transaction_id);
                    table_file.discard_cache();
                    return false;
                }
            }
        }
        return finish_statement(transaction_id);
    } catch (const std::exception&) {
        abort_statement(transaction_id);
        return false;
    }
}

bool Database::add_constraint(const std::string& table_name, const ConstraintDefinition& constraint) {
    if (!acquire_transaction_lock(table_name, TableLockMode::exclusive)) {
        return false;
    }
    LockManager::TableLock table_lock = acquire_statement_lock(table_name, TableLockMode::exclusive);

    std::optional<Schema> schema = catalog_.load_schema(database_name_, table_name);
    if (!schema.has_value()) {
        return false;
    }

    uint64_t next_constraint_id = 1;
    std::vector<ConstraintDefinition> constraints = schema->constraints();
    for (const ConstraintDefinition& existing : constraints) {
        if (existing.id >= next_constraint_id) {
            next_constraint_id = existing.id + 1;
        }
    }

    ConstraintDefinition new_constraint = constraint;
    new_constraint.id = next_constraint_id;
    constraints.push_back(std::move(new_constraint));

    try {
        Schema updated_schema(schema->table_name(), schema->columns(), constraints, schema->indexes(), schema->storage_mode());
        const uint64_t transaction_id = current_transaction_id();
        WalManager* wal = wal_manager();
        if (!in_transaction() && wal != nullptr && wal->begin_transaction(transaction_id) == 0) {
            return false;
        }
        if (wal != nullptr && wal->log_schema_update(
            transaction_id,
            table_directory_path(table_name),
            serialize_schema(*schema),
            serialize_schema(updated_schema)
        ) == 0) {
            abort_statement(transaction_id);
            return false;
        }
        if (!catalog_.replace_table_schema(database_name_, updated_schema)) {
            abort_statement(transaction_id);
            return false;
        }
        return finish_statement(transaction_id);
    } catch (const std::exception&) {
        return false;
    }
}

bool Database::drop_constraint(const std::string& table_name, uint64_t constraint_id) {
    if (!acquire_transaction_lock(table_name, TableLockMode::exclusive)) {
        return false;
    }
    LockManager::TableLock table_lock = acquire_statement_lock(table_name, TableLockMode::exclusive);

    std::optional<Schema> schema = catalog_.load_schema(database_name_, table_name);
    if (!schema.has_value()) {
        return false;
    }

    std::vector<ConstraintDefinition> constraints;
    bool removed = false;
    for (const ConstraintDefinition& constraint : schema->constraints()) {
        if (constraint.id == constraint_id) {
            removed = true;
            continue;
        }
        constraints.push_back(constraint);
    }
    if (!removed) {
        return false;
    }

    try {
        Schema updated_schema(schema->table_name(), schema->columns(), constraints, schema->indexes(), schema->storage_mode());
        const uint64_t transaction_id = current_transaction_id();
        WalManager* wal = wal_manager();
        if (!in_transaction() && wal != nullptr && wal->begin_transaction(transaction_id) == 0) {
            return false;
        }
        if (wal != nullptr && wal->log_schema_update(
            transaction_id,
            table_directory_path(table_name),
            serialize_schema(*schema),
            serialize_schema(updated_schema)
        ) == 0) {
            abort_statement(transaction_id);
            return false;
        }
        if (!catalog_.replace_table_schema(database_name_, updated_schema)) {
            abort_statement(transaction_id);
            return false;
        }
        return finish_statement(transaction_id);
    } catch (const std::exception&) {
        return false;
    }
}

bool Database::create_index(const std::string& table_name, const std::string& index_name, const std::string& column_name, bool unique) {
    if (!acquire_transaction_lock(table_name, TableLockMode::exclusive)) {
        return false;
    }
    LockManager::TableLock table_lock = acquire_statement_lock(table_name, TableLockMode::exclusive);
    std::optional<Schema> schema = catalog_.load_schema(database_name_, table_name);
    if (!schema.has_value() || schema->column_index(column_name) < 0) {
        return false;
    }

    uint64_t next_index_id = 1;
    std::vector<IndexDefinition> indexes = schema->indexes();
    for (const IndexDefinition& existing : indexes) {
        if (existing.name == index_name || existing.columns == std::vector<std::string>{column_name}) {
            return false;
        }
        if (existing.id >= next_index_id) {
            next_index_id = existing.id + 1;
        }
    }

    IndexDefinition index = IndexDefinition::make(next_index_id, index_name, {column_name}, unique);
    indexes.push_back(index);
    try {
        Schema updated_schema(schema->table_name(), schema->columns(), schema->constraints(), indexes, schema->storage_mode());
        const uint64_t transaction_id = current_transaction_id();
        WalManager* wal = wal_manager();
        if (!in_transaction() && wal != nullptr && wal->begin_transaction(transaction_id) == 0) {
            return false;
        }
        if (schema->storage_mode() == TableStorageMode::disk && !rebuild_index(table_name, *schema, index, wal, transaction_id)) {
            abort_statement(transaction_id);
            return false;
        }
        if (wal != nullptr && wal->log_schema_update(
            transaction_id,
            table_directory_path(table_name),
            serialize_schema(*schema),
            serialize_schema(updated_schema)
        ) == 0) {
            abort_statement(transaction_id);
            return false;
        }
        if (!catalog_.replace_table_schema(database_name_, updated_schema)) {
            abort_statement(transaction_id);
            return false;
        }
        return finish_statement(transaction_id);
    } catch (const std::exception&) {
        return false;
    }
}

bool Database::drop_index(const std::string& table_name, const std::string& index_name) {
    if (!acquire_transaction_lock(table_name, TableLockMode::exclusive)) {
        return false;
    }
    LockManager::TableLock table_lock = acquire_statement_lock(table_name, TableLockMode::exclusive);
    std::optional<Schema> schema = catalog_.load_schema(database_name_, table_name);
    if (!schema.has_value()) {
        return false;
    }

    std::vector<IndexDefinition> indexes;
    std::optional<IndexDefinition> removed_index;
    for (const IndexDefinition& index : schema->indexes()) {
        if (index.name == index_name) {
            removed_index = index;
            continue;
        }
        indexes.push_back(index);
    }
    if (!removed_index.has_value()) {
        return false;
    }

    try {
        Schema updated_schema(schema->table_name(), schema->columns(), schema->constraints(), indexes, schema->storage_mode());
        const uint64_t transaction_id = current_transaction_id();
        WalManager* wal = wal_manager();
        if (!in_transaction() && wal != nullptr && wal->begin_transaction(transaction_id) == 0) {
            return false;
        }
        if (wal != nullptr && wal->log_schema_update(
            transaction_id,
            table_directory_path(table_name),
            serialize_schema(*schema),
            serialize_schema(updated_schema)
        ) == 0) {
            abort_statement(transaction_id);
            return false;
        }
        if (!catalog_.replace_table_schema(database_name_, updated_schema)) {
            abort_statement(transaction_id);
            return false;
        }
        return finish_statement(transaction_id);
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<std::vector<TableRow>> Database::find_rows_by_index(const std::string& table_name, const std::string& column_name, const Value& value) const {
    std::optional<Schema> schema = catalog_.load_schema(database_name_, table_name);
    if (!schema.has_value()) {
        return std::nullopt;
    }
    const int column_index = schema->column_index(column_name);
    if (column_index < 0) {
        return std::nullopt;
    }
    const Column* column = schema->column(static_cast<uint16_t>(column_index));
    if (column == nullptr) {
        return std::nullopt;
    }
    if (schema->storage_mode() == TableStorageMode::memory) {
        bool has_index = false;
        for (const IndexDefinition& index : schema->indexes()) {
            if (index.columns.size() == 1 && index.columns[0] == column_name) {
                has_index = true;
                break;
            }
        }
        if (!has_index) {
            return std::nullopt;
        }
        std::optional<std::vector<uint8_t>> key = make_index_key(*column, value);
        if (!key.has_value()) {
            return std::nullopt;
        }
        std::vector<TableRow> rows;
        scan_memory_rows(table_name, [this, column, column_index, &key, &rows](const TableRow& table_row) {
            const Value* stored_value = table_row.row.value(static_cast<uint16_t>(column_index));
            std::optional<std::vector<uint8_t>> stored_key = stored_value == nullptr ? std::nullopt : make_index_key(*column, *stored_value);
            if (stored_key.has_value() && *stored_key == *key) {
                rows.push_back(table_row);
            }
            return true;
        });
        return rows;
    }
    for (const IndexDefinition& index : schema->indexes()) {
        if (index.columns.size() != 1 || index.columns[0] != column_name) {
            continue;
        }
        std::optional<std::vector<uint8_t>> key = make_index_key(*column, value);
        if (!key.has_value()) {
            return std::nullopt;
        }
        std::vector<TableRow> rows;
        IndexFile index_file(index_file_path(table_name, index));
        for (RecordId record_id : index_file.find(*key)) {
            std::optional<Row> row = read_row(table_name, record_id);
            if (row.has_value()) {
                rows.push_back(TableRow{record_id, *row});
            }
        }
        return rows;
    }
    return std::nullopt;
}

std::optional<std::vector<TableRow>> Database::find_rows_by_index_range(
    const std::string& table_name,
    const std::string& column_name,
    const std::optional<Value>& lower_value,
    bool include_lower,
    const std::optional<Value>& upper_value,
    bool include_upper
) const {
    std::optional<Schema> schema = catalog_.load_schema(database_name_, table_name);
    if (!schema.has_value()) {
        return std::nullopt;
    }
    const int column_index = schema->column_index(column_name);
    if (column_index < 0) {
        return std::nullopt;
    }
    const Column* column = schema->column(static_cast<uint16_t>(column_index));
    if (column == nullptr) {
        return std::nullopt;
    }
    if (schema->storage_mode() == TableStorageMode::memory) {
        bool has_index = false;
        for (const IndexDefinition& index : schema->indexes()) {
            if (index.columns.size() == 1 && index.columns[0] == column_name) {
                has_index = true;
                break;
            }
        }
        if (!has_index) {
            return std::nullopt;
        }
        std::optional<std::vector<uint8_t>> lower_key = std::nullopt;
        std::optional<std::vector<uint8_t>> upper_key = std::nullopt;
        if (lower_value.has_value()) {
            lower_key = make_index_key(*column, *lower_value);
            if (!lower_key.has_value()) {
                return std::nullopt;
            }
        }
        if (upper_value.has_value()) {
            upper_key = make_index_key(*column, *upper_value);
            if (!upper_key.has_value()) {
                return std::nullopt;
            }
        }

        std::vector<TableRow> rows;
        scan_memory_rows(table_name, [this, column, column_index, &lower_key, include_lower, &upper_key, include_upper, &rows](const TableRow& table_row) {
            const Value* stored_value = table_row.row.value(static_cast<uint16_t>(column_index));
            std::optional<std::vector<uint8_t>> stored_key = stored_value == nullptr ? std::nullopt : make_index_key(*column, *stored_value);
            if (!stored_key.has_value()) {
                return true;
            }
            if (lower_key.has_value() && (*stored_key < *lower_key || (!include_lower && *stored_key == *lower_key))) {
                return true;
            }
            if (upper_key.has_value() && (*stored_key > *upper_key || (!include_upper && *stored_key == *upper_key))) {
                return true;
            }
            rows.push_back(table_row);
            return true;
        });
        return rows;
    }
    for (const IndexDefinition& index : schema->indexes()) {
        if (index.columns.size() != 1 || index.columns[0] != column_name) {
            continue;
        }
        std::optional<std::vector<uint8_t>> lower_key = std::nullopt;
        std::optional<std::vector<uint8_t>> upper_key = std::nullopt;
        if (lower_value.has_value()) {
            lower_key = make_index_key(*column, *lower_value);
            if (!lower_key.has_value()) {
                return std::nullopt;
            }
        }
        if (upper_value.has_value()) {
            upper_key = make_index_key(*column, *upper_value);
            if (!upper_key.has_value()) {
                return std::nullopt;
            }
        }

        std::vector<TableRow> rows;
        IndexFile index_file(index_file_path(table_name, index));
        for (RecordId record_id : index_file.find_range(lower_key, include_lower, upper_key, include_upper)) {
            std::optional<Row> row = read_row(table_name, record_id);
            if (row.has_value()) {
                rows.push_back(TableRow{record_id, *row});
            }
        }
        return rows;
    }
    return std::nullopt;
}

bool Database::begin_transaction() {
    WalManager* wal = wal_manager();
    uint64_t transaction_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_transaction_id_ != 0) {
            return false;
        }
        transaction_id = next_transaction_id_++;
        active_transaction_id_ = transaction_id;
        transaction_aborted_ = false;
    }

    if (wal != nullptr && wal->begin_transaction(transaction_id) == 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        active_transaction_id_ = 0;
        return false;
    }
    return true;
}

bool Database::commit_transaction() {
    uint64_t transaction_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_transaction_id_ == 0) {
            return false;
        }
        transaction_id = active_transaction_id_;
    }

    if (!commit_statement(transaction_id)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    active_locks_.clear();
    active_table_locks_.clear();
    active_transaction_id_ = 0;
    transaction_aborted_ = false;
    return true;
}

bool Database::rollback_transaction() {
    uint64_t transaction_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_transaction_id_ == 0) {
            return false;
        }
        transaction_id = active_transaction_id_;
    }

    if (!rollback_statement(transaction_id)) {
        return false;
    }
    rebuild_indexes();

    std::lock_guard<std::mutex> lock(mutex_);
    active_locks_.clear();
    active_table_locks_.clear();
    active_transaction_id_ = 0;
    transaction_aborted_ = false;
    return true;
}

bool Database::in_transaction() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_transaction_id_ != 0;
}

WalManager* Database::wal_manager() {
    std::lock_guard<std::mutex> lock(mutex_);
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

LockManager::TableLock Database::acquire_statement_lock(const std::string& table_name, TableLockMode mode) const {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_table_locks_.find(table_lock_key(table_name)) != active_table_locks_.end()) {
            return LockManager::TableLock();
        }
    }

    if (mode == TableLockMode::shared) {
        return lock_manager().lock_table_shared(table_lock_key(table_name));
    }
    return lock_manager().lock_table_exclusive(table_lock_key(table_name));
}

bool Database::acquire_transaction_lock(const std::string& table_name, TableLockMode mode) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_transaction_id_ == 0) {
        return true;
    }
    if (transaction_aborted_) {
        return false;
    }

    const std::string key = table_lock_key(table_name);
    auto found = active_table_locks_.find(key);
    if (found != active_table_locks_.end()) {
        if (found->second == TableLockMode::exclusive || mode == TableLockMode::shared) {
            return true;
        }
        active_locks_.erase(key);
        active_table_locks_.erase(found);
    }

    if (mode == TableLockMode::shared) {
        LockManager::TableLock table_lock = lock_manager().try_lock_table_shared_for(key, TABLE_LOCK_TIMEOUT);
        if (!table_lock) {
            return false;
        }
        active_locks_.emplace(key, std::move(table_lock));
    } else {
        LockManager::TableLock table_lock = lock_manager().try_lock_table_exclusive_for(key, TABLE_LOCK_TIMEOUT);
        if (!table_lock) {
            return false;
        }
        active_locks_.emplace(key, std::move(table_lock));
    }
    active_table_locks_[key] = mode;
    return true;
}

uint64_t Database::current_transaction_id() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_transaction_id_ != 0) {
        return active_transaction_id_;
    }
    return next_transaction_id_++;
}

bool Database::finish_statement(uint64_t transaction_id) {
    if (in_transaction()) {
        return true;
    }
    return commit_statement(transaction_id);
}

bool Database::abort_statement(uint64_t transaction_id) {
    if (in_transaction()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_transaction_id_ == transaction_id) {
            transaction_aborted_ = true;
        }
        return false;
    }
    return rollback_statement(transaction_id);
}

uint64_t Database::next_transaction_id() {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_transaction_id_++;
}

bool Database::commit_statement(uint64_t transaction_id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_transaction_id_ == transaction_id && transaction_aborted_) {
            return false;
        }
    }
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

std::string Database::table_lock_key(const std::string& table_name) const {
    return table_directory_path(table_name).lexically_normal().string();
}

std::string Database::memory_table_key(const std::string& table_name) const {
    return table_lock_key(table_name);
}

std::optional<RecordId> Database::insert_memory_row(const std::string& table_name, const Row& row) {
    std::lock_guard<std::mutex> lock(memory_tables_mutex);
    std::optional<Schema> schema = catalog_.load_schema(database_name_, table_name);
    if (!schema.has_value()) {
        return std::nullopt;
    }
    for (const IndexDefinition& index : schema->indexes()) {
        if (!index.unique) {
            continue;
        }
        const int column_index = schema->column_index(index.columns[0]);
        const Column* column = schema->column(static_cast<uint16_t>(column_index));
        const Value* value = row.value(static_cast<uint16_t>(column_index));
        std::optional<std::vector<uint8_t>> key = column == nullptr || value == nullptr ? std::nullopt : make_index_key(*column, *value);
        if (!key.has_value()) {
            return std::nullopt;
        }
        const MemoryTableStore& table = memory_tables[memory_table_key(table_name)];
        for (const std::optional<Row>& existing_row : table.rows) {
            if (!existing_row.has_value()) {
                continue;
            }
            const Value* existing_value = existing_row->value(static_cast<uint16_t>(column_index));
            std::optional<std::vector<uint8_t>> existing_key = existing_value == nullptr ? std::nullopt : make_index_key(*column, *existing_value);
            if (existing_key.has_value() && *existing_key == *key) {
                return std::nullopt;
            }
        }
    }
    MemoryTableStore& table = memory_tables[memory_table_key(table_name)];
    const RecordId record_id{table.next_page_id++, 0};
    table.rows.push_back(row);
    return record_id;
}

std::optional<Row> Database::read_memory_row(const std::string& table_name, RecordId record_id) const {
    std::lock_guard<std::mutex> lock(memory_tables_mutex);
    const auto found = memory_tables.find(memory_table_key(table_name));
    if (found == memory_tables.end() || record_id.page_id == 0 || record_id.slot_id != 0) {
        return std::nullopt;
    }
    const std::size_t index = static_cast<std::size_t>(record_id.page_id - 1);
    if (index >= found->second.rows.size() || !found->second.rows[index].has_value()) {
        return std::nullopt;
    }
    return *found->second.rows[index];
}

bool Database::scan_memory_rows(const std::string& table_name, const std::function<bool(const TableRow&)>& callback) const {
    std::vector<TableRow> snapshot;
    {
        std::lock_guard<std::mutex> lock(memory_tables_mutex);
        const auto found = memory_tables.find(memory_table_key(table_name));
        if (found != memory_tables.end()) {
            for (std::size_t i = 0; i < found->second.rows.size(); ++i) {
                if (found->second.rows[i].has_value()) {
                    snapshot.push_back(TableRow{RecordId{static_cast<uint32_t>(i + 1), 0}, *found->second.rows[i]});
                }
            }
        }
    }
    for (const TableRow& row : snapshot) {
        if (!callback(row)) {
            return false;
        }
    }
    return true;
}

bool Database::delete_memory_row(const std::string& table_name, RecordId record_id) {
    std::lock_guard<std::mutex> lock(memory_tables_mutex);
    auto found = memory_tables.find(memory_table_key(table_name));
    if (found == memory_tables.end() || record_id.page_id == 0 || record_id.slot_id != 0) {
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(record_id.page_id - 1);
    if (index >= found->second.rows.size() || !found->second.rows[index].has_value()) {
        return false;
    }
    found->second.rows[index].reset();
    return true;
}

bool Database::update_memory_row(const std::string& table_name, RecordId& record_id, const Row& row) {
    std::lock_guard<std::mutex> lock(memory_tables_mutex);
    std::optional<Schema> schema = catalog_.load_schema(database_name_, table_name);
    if (!schema.has_value()) {
        return false;
    }
    auto found = memory_tables.find(memory_table_key(table_name));
    if (found == memory_tables.end() || record_id.page_id == 0 || record_id.slot_id != 0) {
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(record_id.page_id - 1);
    if (index >= found->second.rows.size() || !found->second.rows[index].has_value()) {
        return false;
    }
    for (const IndexDefinition& index_definition : schema->indexes()) {
        if (!index_definition.unique) {
            continue;
        }
        const int column_index = schema->column_index(index_definition.columns[0]);
        const Column* column = schema->column(static_cast<uint16_t>(column_index));
        const Value* value = row.value(static_cast<uint16_t>(column_index));
        std::optional<std::vector<uint8_t>> key = column == nullptr || value == nullptr ? std::nullopt : make_index_key(*column, *value);
        if (!key.has_value()) {
            return false;
        }
        for (std::size_t i = 0; i < found->second.rows.size(); ++i) {
            if (i == index || !found->second.rows[i].has_value()) {
                continue;
            }
            const Value* existing_value = found->second.rows[i]->value(static_cast<uint16_t>(column_index));
            std::optional<std::vector<uint8_t>> existing_key = existing_value == nullptr ? std::nullopt : make_index_key(*column, *existing_value);
            if (existing_key.has_value() && *existing_key == *key) {
                return false;
            }
        }
    }
    found->second.rows[index] = row;
    return true;
}

LockManager& Database::lock_manager() {
    static LockManager manager;
    return manager;
}

std::string Database::serialize_schema(const Schema& schema) const {
    std::ostringstream out;
    out << "table " << schema.table_name() << '\n';
    out << "storage " << (schema.storage_mode() == TableStorageMode::memory ? "MEMORY" : "DISK") << '\n';
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
    for (const IndexDefinition& index : schema.indexes()) {
        out << "index " << index.serialized() << '\n';
    }
    return out.str();
}

std::filesystem::path Database::table_directory_path(const std::string& table_name) const {
    return data_root_ / database_name_ / table_name;
}

std::filesystem::path Database::index_file_path(const std::string& table_name, const IndexDefinition& index) const {
    return table_directory_path(table_name) / ("index_" + std::to_string(index.id) + ".idx");
}

std::optional<std::vector<uint8_t>> Database::make_index_key(const Column& column, const Value& value) const {
    if (!value.matches_column(column)) {
        return std::nullopt;
    }

    std::vector<uint8_t> key;
    key.push_back(static_cast<uint8_t>(value.is_null() ? ColumnType::null_type : column.type()));
    if (value.is_null()) {
        return key;
    }

    switch (column.type()) {
        case ColumnType::integer:
            append_int64_key(key, value.integer_data());
            break;
        case ColumnType::number:
            append_int64_key(key, value.number_data());
            break;
        case ColumnType::character:
        case ColumnType::string:
        case ColumnType::varstring:
        case ColumnType::text:
            key.insert(key.end(), value.string_data().begin(), value.string_data().end());
            break;
        case ColumnType::date:
            append_uint32_key(key, static_cast<uint32_t>(value.date_data().days_since_epoch()));
            break;
        case ColumnType::time:
            append_uint32_key(key, value.time_data().seconds_since_midnight());
            break;
        case ColumnType::datetime:
            append_int64_key(key, value.datetime_data().seconds_since_epoch());
            break;
        case ColumnType::null_type:
            return std::nullopt;
    }
    return key;
}

bool Database::rebuild_index(const std::string& table_name, const Schema& schema, const IndexDefinition& index, WalManager* wal, uint64_t transaction_id) const {
    const int column_index = schema.column_index(index.columns[0]);
    const Column* column = schema.column(static_cast<uint16_t>(column_index));
    if (column == nullptr) {
        return false;
    }
    IndexFile index_file(index_file_path(table_name, index), wal, transaction_id);
    if (!index_file.reset()) {
        return false;
    }
    bool ok = true;
    try {
        TableFile table_file(catalog_.table_file_path(database_name_, table_name), const_cast<Database*>(this)->wal_manager());
        table_file.scan_records([this, &schema, &index, &index_file, column, column_index, &ok](RecordId record_id, const std::vector<uint8_t>& record) {
            Row row;
            if (!RecordSerializer::deserialize(schema, record, row)) {
                return true;
            }
            const Value* value = row.value(static_cast<uint16_t>(column_index));
            std::optional<std::vector<uint8_t>> key = value == nullptr ? std::nullopt : make_index_key(*column, *value);
            if (!key.has_value() || !index_file.insert(*key, record_id, index.unique)) {
                ok = false;
                return false;
            }
            return true;
        });
    } catch (const std::exception&) {
        return false;
    }
    return ok && index_file.sync();
}

bool Database::rebuild_indexes() const {
    bool ok = true;
    for (const std::string& table_name : catalog_.list_tables(database_name_)) {
        std::optional<Schema> schema = catalog_.load_schema(database_name_, table_name);
        if (!schema.has_value()) {
            ok = false;
            continue;
        }
        for (const IndexDefinition& index : schema->indexes()) {
            if (!rebuild_index(table_name, *schema, index, nullptr, 0)) {
                ok = false;
            }
        }
    }
    return ok;
}

/// @brief Build record bytes from a row using a table's schema
/// @param table_name The table whose schema should be used
/// @param row The row to convert
/// @return Record bytes if the table exists and the row matches its schema
std::optional<std::vector<uint8_t>> Database::make_record(const std::string& table_name, const Row& row) const {
    std::optional<Schema> schema = catalog_.load_schema(database_name_, table_name);
    if (!schema.has_value()) {
        return std::nullopt;
    }

    std::vector<uint8_t> record;
    if (!RecordSerializer::serialize(*schema, row, record)) {
        return std::nullopt;
    }

    return record;
}
