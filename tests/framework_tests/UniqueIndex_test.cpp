/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 *
 * PostgreSQL Unique-Index ORM Foundation Tests.
 *
 * Verifies that the ORM correctly generates and enforces normal vs unique indexes
 * through the existing ORM::Index / ORM::IndexVec abstraction.
 *
 * Scoped to PostgreSQL (the target multi-instance correctness backend) with explicit
 * rejection of unsupported backends (SQLite / MySQL) when Unique=true is requested.
 *
 * Does NOT apply uniqueness to any production OWPROV table or field.
 *
 * Comprehensive Test Suite:
 *   1. Unsupported backend rejection check (SQLite & MySQL return false when Unique=true).
 *   2. SQLite backward-compatibility check (normal indexes create and allow duplicates).
 *   3. Test A (PostgreSQL) - Normal index: duplicate indexed values are accepted.
 *   4. Test B (PostgreSQL) - Single-column unique index: duplicate value is rejected.
 *   5. Test C (PostgreSQL) - Composite unique index: duplicates rejected on (col_a, col_b) tuple;
 *                            non-duplicate combinations are accepted.
 *   6. Test D (PostgreSQL) - Repeated schema creation is idempotent (CREATE [UNIQUE] INDEX IF NOT EXISTS);
 *                            verifies unique constraint remains enforced after repeated Create().
 *   7. Test E (PostgreSQL) - Transaction-aware duplicate write marks tx failed; Commit() returns false.
 *   8. Test F (PostgreSQL) - DeleteRecord frees unique key for subsequent insertion.
 *   9. Test G (PostgreSQL) - Transaction Rollback frees unique key for subsequent insertion.
 *  10. Test H (PostgreSQL) - Concurrent race: simultaneous duplicate inserts allow exactly 1 winner.
 *  11. Test I (PostgreSQL) - Pre-existing duplicate rows cause Create() of declared unique index to fail.
 *  12. Test J (PostgreSQL) - Same-name non-unique index causes Unique=true Create() to return false.
 *  13. Test K (PostgreSQL) - Same-name unique index on wrong column causes Create() to return false.
 *  14. Test L (PostgreSQL) - Same-name partial unique index on expected column causes Create() to return false.
 *  15. Test M (PostgreSQL) - Multi-schema isolation: same-name objects in another schema do not cause false pass/fail.
 *  16. Test N (PostgreSQL) - Earlier normal index DDL failure causes Create() to return false (does not fall through to Upgrade()).
 *  17. Test O (PostgreSQL) - Mixed-case unique index name (DeviceUniqueIndex) normalizes and succeeds.
 *  18. Test P (PostgreSQL) - Overlength unique index name (> 63 chars) truncates and succeeds.
 *  19. Test Q (PostgreSQL) - Conflicting index with INCLUDE column causes Create() to return false.
 *  20. Test R (PostgreSQL) - Deferrable unique constraint causes Create() to return false.
 *  21. Test S (PostgreSQL) - Existing unique index on quoted "Value" instead of unquoted value causes Create() to return false.
 */

#include <iostream>
#include <cstdlib>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <future>
#include <vector>

#include "Poco/ConsoleChannel.h"
#include "Poco/AutoPtr.h"
#include "Poco/Data/SessionPool.h"
#include "Poco/Data/Session.h"
#include "Poco/Data/Statement.h"
#include "Poco/Data/SQLite/Connector.h"
#ifndef SMALL_BUILD
#include "Poco/Data/PostgreSQL/Connector.h"
#endif
#include "Poco/Logger.h"
#include "framework/DbTransaction.h"
#include "framework/StorageClass.h"
#include "framework/orm.h"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "TEST FAILURE [" << __FILE__ << ":" << __LINE__ << "]: " << msg << std::endl; \
            std::exit(1); \
        } \
    } while (0)

namespace OpenWifi {

struct UniTestRecord {
    std::string id;
    std::string name;
    std::string value;
    void to_json(Poco::JSON::Object &) const {}
    bool from_json(const Poco::JSON::Object::Ptr &) { return true; }
};
typedef Poco::Tuple<std::string, std::string, std::string> UniTestRecordTuple;

struct UniCompositeRecord {
    std::string id;
    std::string col_a;
    std::string col_b;
    void to_json(Poco::JSON::Object &) const {}
    bool from_json(const Poco::JSON::Object::Ptr &) { return true; }
};
typedef Poco::Tuple<std::string, std::string, std::string> UniCompositeRecordTuple;

} // namespace OpenWifi

template <>
void ORM::DB<OpenWifi::UniTestRecordTuple, OpenWifi::UniTestRecord>::Convert(
    const OpenWifi::UniTestRecordTuple &In, OpenWifi::UniTestRecord &Out) {
    Out.id    = In.get<0>();
    Out.name  = In.get<1>();
    Out.value = In.get<2>();
}
template <>
void ORM::DB<OpenWifi::UniTestRecordTuple, OpenWifi::UniTestRecord>::Convert(
    const OpenWifi::UniTestRecord &In, OpenWifi::UniTestRecordTuple &Out) {
    Out.set<0>(In.id);
    Out.set<1>(In.name);
    Out.set<2>(In.value);
}

template <>
void ORM::DB<OpenWifi::UniCompositeRecordTuple, OpenWifi::UniCompositeRecord>::Convert(
    const OpenWifi::UniCompositeRecordTuple &In, OpenWifi::UniCompositeRecord &Out) {
    Out.id    = In.get<0>();
    Out.col_a = In.get<1>();
    Out.col_b = In.get<2>();
}
template <>
void ORM::DB<OpenWifi::UniCompositeRecordTuple, OpenWifi::UniCompositeRecord>::Convert(
    const OpenWifi::UniCompositeRecord &In, OpenWifi::UniCompositeRecordTuple &Out) {
    Out.set<0>(In.id);
    Out.set<1>(In.col_a);
    Out.set<2>(In.col_b);
}

// DB with a normal (non-unique) index on "value"
class NormalIndexDB : public ORM::DB<OpenWifi::UniTestRecordTuple, OpenWifi::UniTestRecord> {
  public:
    NormalIndexDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L,
                  const char *TableName = "pg_uni_normal_test")
        : DB(T, TableName,
             ORM::FieldVec{
                 ORM::Field{"id",    ORM::FieldType::FT_TEXT, 0, true},
                 ORM::Field{"name",  ORM::FieldType::FT_TEXT},
                 ORM::Field{"value", ORM::FieldType::FT_TEXT}
             },
             ORM::IndexVec{
                 // Existing two-member declaration: { Name, Entries }
                 // Omitting the 3rd argument tests that it defaults to Unique=false (backward compatibility).
                 // Derive index name from TableName so every test table has a distinct index name.
                 {std::string(TableName) + "_normal_idx",
                  ORM::IndexEntryVec{{std::string("value"), ORM::Indextype::ASC}}}
             },
             P, L, "unt") {}
};

// DB with a UNIQUE index on "value"
class UniqueIndexDB : public ORM::DB<OpenWifi::UniTestRecordTuple, OpenWifi::UniTestRecord> {
  public:
    UniqueIndexDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L,
                  const char *TableName = "pg_uni_unique_test")
        : DB(T, TableName,
             ORM::FieldVec{
                 ORM::Field{"id",    ORM::FieldType::FT_TEXT, 0, true},
                 ORM::Field{"name",  ORM::FieldType::FT_TEXT},
                 ORM::Field{"value", ORM::FieldType::FT_TEXT}
             },
             ORM::IndexVec{
                 // Unique=true -- duplicate values rejected by PostgreSQL.
                 // Derive index name from TableName so every test table has a distinct index name.
                 {std::string(TableName) + "_unique_idx",
                  ORM::IndexEntryVec{{std::string("value"), ORM::Indextype::ASC}},
                  true}
             },
             P, L, "uut") {}
};

// DB with a UNIQUE composite index on (col_a, col_b)
class CompositeUniqueDB : public ORM::DB<OpenWifi::UniCompositeRecordTuple, OpenWifi::UniCompositeRecord> {
  public:
    CompositeUniqueDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L,
                      const char *TableName = "pg_uni_composite_test")
        : DB(T, TableName,
             ORM::FieldVec{
                 ORM::Field{"id",    ORM::FieldType::FT_TEXT, 0, true},
                 ORM::Field{"col_a", ORM::FieldType::FT_TEXT},
                 ORM::Field{"col_b", ORM::FieldType::FT_TEXT}
             },
             ORM::IndexVec{
                 // UNIQUE on the (col_a, col_b) tuple.
                 // Derive index name from TableName so every test table has a distinct index name.
                 {std::string(TableName) + "_composite_idx",
                  ORM::IndexEntryVec{
                      {std::string("col_a"), ORM::Indextype::ASC},
                      {std::string("col_b"), ORM::Indextype::ASC}
                  },
                  true}
             },
             P, L, "uct") {}
};

class NormalThenUniqueDB : public ORM::DB<OpenWifi::UniTestRecordTuple, OpenWifi::UniTestRecord> {
  public:
    NormalThenUniqueDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L,
                       const char *TableName = "pg_uni_normal_then_unique_test")
        : DB(T, TableName,
             ORM::FieldVec{
                 ORM::Field{"id",    ORM::FieldType::FT_TEXT, 0, true},
                 ORM::Field{"name",  ORM::FieldType::FT_TEXT},
                 ORM::Field{"value", ORM::FieldType::FT_TEXT}
             },
             ORM::IndexVec{
                 // Normal index on 'name' FIRST (non-unique)
                 {std::string(TableName) + "_norm_idx",
                  ORM::IndexEntryVec{{std::string("name"), ORM::Indextype::ASC}},
                  false},
                 // Required UNIQUE index on 'value' SECOND
                 {std::string(TableName) + "_unique_idx",
                  ORM::IndexEntryVec{{std::string("value"), ORM::Indextype::ASC}},
                  true}
             },
             P, L, "ntu") {}
};

class MixedCaseUniqueDB : public ORM::DB<OpenWifi::UniTestRecordTuple, OpenWifi::UniTestRecord> {
  public:
    MixedCaseUniqueDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L,
                      const char *TableName = "pg_uni_mixedcase_test")
        : DB(T, TableName,
             ORM::FieldVec{
                 ORM::Field{"id",    ORM::FieldType::FT_TEXT, 0, true},
                 ORM::Field{"name",  ORM::FieldType::FT_TEXT},
                 ORM::Field{"value", ORM::FieldType::FT_TEXT}
             },
             ORM::IndexVec{
                 // Mixed-case unquoted index identifier (folds to lowercase in PostgreSQL pg_class)
                 {"DeviceUniqueIndex",
                  ORM::IndexEntryVec{{std::string("value"), ORM::Indextype::ASC}},
                  true}
             },
             P, L, "mcu") {}
};

class OverlengthUniqueDB : public ORM::DB<OpenWifi::UniTestRecordTuple, OpenWifi::UniTestRecord> {
  public:
    OverlengthUniqueDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L,
                       const char *TableName = "pg_uni_overlength_test")
        : DB(T, TableName,
             ORM::FieldVec{
                 ORM::Field{"id",    ORM::FieldType::FT_TEXT, 0, true},
                 ORM::Field{"name",  ORM::FieldType::FT_TEXT},
                 ORM::Field{"value", ORM::FieldType::FT_TEXT}
             },
             ORM::IndexVec{
                 // Overlength index identifier (> 63 characters in PostgreSQL)
                 {"DeviceUniqueIndex_Overlength_1234567890_1234567890_1234567890_1234567890",
                  ORM::IndexEntryVec{{std::string("value"), ORM::Indextype::ASC}},
                  true}
             },
             P, L, "olu") {}
};

#ifndef SMALL_BUILD
static bool DropPgTable(Poco::Data::SessionPool &pool, const std::string &tableName) {
    try {
        Poco::Data::Session s = pool.get();
        Poco::Data::Statement dropStmt(s);
        dropStmt << "DROP TABLE IF EXISTS " + tableName + " CASCADE";
        dropStmt.execute();
        return true;
    } catch (const std::exception &e) {
        std::cerr << "DropPgTable exception for " << tableName << ": " << e.what() << std::endl;
        return false;
    }
}
#endif

int main() {
    std::cout << "[Framework Unit Test] Initializing PostgreSQL Unique-Index ORM Foundation Tests..." << std::endl;

    Poco::AutoPtr<Poco::ConsoleChannel> pChannel(new Poco::ConsoleChannel);
    Poco::Logger &logger = Poco::Logger::get("UniqueIndexTest");
    logger.setChannel(pChannel);

    // -------------------------------------------------------------------------
    // 1. Unsupported backend rejection check (SQLite & MySQL)
    // -------------------------------------------------------------------------
    {
        std::cout << "  - 1. Unsupported backend rejection check (SQLite & MySQL)... " << std::flush;
        Poco::Data::SQLite::Connector::registerConnector();
        Poco::Data::SessionPool dummyPool("SQLite", "dummy_unsupported_test.db");

        // 1a. SQLite with Unique=true must construct without throwing, but Create() must return false
        UniqueIndexDB unsupportedSqlite(OpenWifi::DBType::sqlite, dummyPool, logger, "unsupported_sqlite_table");
        TEST_ASSERT(!unsupportedSqlite.Create(), "Create() on SQLite with Unique=true must return false");

        // 1b. MySQL with Unique=true must construct without throwing, but Create() must return false
        UniqueIndexDB unsupportedMysql(OpenWifi::DBType::mysql, dummyPool, logger, "unsupported_mysql_table");
        TEST_ASSERT(!unsupportedMysql.Create(), "Create() on MySQL with Unique=true must return false");

        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // 2. SQLite backward compatibility check (normal indexes create and allow duplicates)
    // -------------------------------------------------------------------------
    {
        std::cout << "  - 2. SQLite backward compatibility (normal index allows duplicates)... " << std::flush;
        Poco::Data::SessionPool sqlitePool("SQLite", "norm_sqlite_compat.db");
        try {
            Poco::Data::Session s = sqlitePool.get();
            s << "DROP TABLE IF EXISTS norm_sqlite_compat", Poco::Data::Keywords::now;
        } catch (...) {}

        NormalIndexDB normalSqlite(OpenWifi::DBType::sqlite, sqlitePool, logger, "norm_sqlite_compat");
        TEST_ASSERT(normalSqlite.Create(), "Failed to create normal index table on SQLite");

        OpenWifi::UniTestRecord r1{"sq-1", "Name 1", "DUPLICATE_VAL"};
        OpenWifi::UniTestRecord r2{"sq-2", "Name 2", "DUPLICATE_VAL"};
        TEST_ASSERT(normalSqlite.CreateRecord(r1), "First insert on SQLite failed");
        TEST_ASSERT(normalSqlite.CreateRecord(r2), "Second insert with duplicate value on SQLite must succeed for normal index");

        try {
            Poco::Data::Session s = sqlitePool.get();
            s << "DROP TABLE IF EXISTS norm_sqlite_compat", Poco::Data::Keywords::now;
        } catch (...) {}

        std::cout << "PASSED" << std::endl;
    }

#ifdef SMALL_BUILD
    std::cout << "[Framework Unit Test] SMALL_BUILD enabled: skipping PostgreSQL tests." << std::endl;
    return 77;
#else
    // -------------------------------------------------------------------------
    // PostgreSQL environment setup
    // -------------------------------------------------------------------------
    const char *pgHost = std::getenv("PGHOST");
    if (!pgHost || std::string(pgHost).empty())
        pgHost = std::getenv("TEST_POSTGRES_HOST");

    if (!pgHost || std::string(pgHost).empty()) {
        std::cout << "[Framework Unit Test] PGHOST / TEST_POSTGRES_HOST not set." << std::endl;
        std::cout << "[Framework Unit Test] Skipping PostgreSQL tests with CTest skip code 77." << std::endl;
        return 77;
    }

    std::string host(pgHost);
    const char *pgPort = std::getenv("PGPORT");
    std::string port = (pgPort && *pgPort) ? pgPort : "5432";
    const char *pgUser = std::getenv("PGUSER");
    std::string user = (pgUser && *pgUser) ? pgUser : "postgres";
    const char *pgPass = std::getenv("PGPASSWORD");
    std::string pass = (pgPass && *pgPass) ? pgPass : "postgres";
    const char *pgDb = std::getenv("PGDATABASE");
    std::string db = (pgDb && *pgDb) ? pgDb : "owprov_test";

    std::string connStr = "host=" + host + " user=" + user + " password=" + pass +
                          " dbname=" + db + " port=" + port + " connect_timeout=5";

    Poco::Data::PostgreSQL::Connector::registerConnector();
    Poco::Data::SessionPool pgPool("PostgreSQL", connStr, 4, 32, 60);

    try {
        Poco::Data::Session testSession = pgPool.get();
        int val = 0;
        testSession << "SELECT 1", Poco::Data::Keywords::into(val), Poco::Data::Keywords::now;
        TEST_ASSERT(val == 1, "PostgreSQL ping SELECT 1 returned unexpected value");
    } catch (const std::exception &e) {
        std::cerr << "Failed to connect to PostgreSQL at " << host << ":" << port
                  << " (" << e.what() << ")" << std::endl;
        std::exit(1);
    }

    // -------------------------------------------------------------------------
    // Test A: Normal index allows duplicate indexed values (PostgreSQL)
    // -------------------------------------------------------------------------
    {
        std::cout << "  - Test A: Normal index allows duplicate indexed values (PostgreSQL)... " << std::flush;

        TEST_ASSERT(DropPgTable(pgPool, "pg_uni_normal_test"), "Failed to drop table before Test A");
        NormalIndexDB normalDb(OpenWifi::DBType::pgsql, pgPool, logger, "pg_uni_normal_test");
        TEST_ASSERT(normalDb.Create(), "Test A: Failed to create normal-index test table");

        OpenWifi::UniTestRecord r1{"id-n-1", "Record 1", "shared_value"};
        OpenWifi::UniTestRecord r2{"id-n-2", "Record 2", "shared_value"};

        TEST_ASSERT(normalDb.CreateRecord(r1), "Test A: First insert with shared_value failed");
        TEST_ASSERT(normalDb.CreateRecord(r2), "Test A: Second insert with shared_value failed -- normal index must NOT enforce uniqueness");

        TEST_ASSERT(DropPgTable(pgPool, "pg_uni_normal_test"), "Failed to drop table after Test A");
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test B: Single-column unique index rejects duplicates (PostgreSQL)
    // -------------------------------------------------------------------------
    {
        std::cout << "  - Test B: Single-column unique index rejects duplicate (PostgreSQL)... " << std::flush;

        TEST_ASSERT(DropPgTable(pgPool, "pg_uni_unique_test"), "Failed to drop table before Test B");
        UniqueIndexDB uniqueDb(OpenWifi::DBType::pgsql, pgPool, logger, "pg_uni_unique_test");
        TEST_ASSERT(uniqueDb.Create(), "Test B: Failed to create unique-index test table");

        OpenWifi::UniTestRecord r1{"id-u-1", "Record 1", "UNIQUE_VAL"};
        OpenWifi::UniTestRecord r2{"id-u-2", "Record 2", "UNIQUE_VAL"};  // duplicate -> must fail
        OpenWifi::UniTestRecord r3{"id-u-3", "Record 3", "DIFFERENT_VAL"};

        TEST_ASSERT( uniqueDb.CreateRecord(r1), "Test B: First insert with UNIQUE_VAL failed unexpectedly");
        TEST_ASSERT(!uniqueDb.CreateRecord(r2), "Test B: Duplicate insert must be rejected by PostgreSQL");
        TEST_ASSERT( uniqueDb.CreateRecord(r3), "Test B: Insert with DIFFERENT_VAL failed unexpectedly");

        TEST_ASSERT(DropPgTable(pgPool, "pg_uni_unique_test"), "Failed to drop table after Test B");
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test C: Composite unique index (col_a, col_b) semantics (PostgreSQL)
    // -------------------------------------------------------------------------
    {
        std::cout << "  - Test C: Composite unique index (col_a, col_b) semantics (PostgreSQL)... " << std::flush;

        TEST_ASSERT(DropPgTable(pgPool, "pg_uni_composite_test"), "Failed to drop table before Test C");
        CompositeUniqueDB compDb(OpenWifi::DBType::pgsql, pgPool, logger, "pg_uni_composite_test");
        TEST_ASSERT(compDb.Create(), "Test C: Failed to create composite-unique-index test table");

        OpenWifi::UniCompositeRecord r1{"id-c-1", "A", "B"};
        OpenWifi::UniCompositeRecord r2{"id-c-2", "A", "B"};  // exact duplicate -> must fail
        OpenWifi::UniCompositeRecord r3{"id-c-3", "A", "C"};  // same col_a, different col_b -> allowed
        OpenWifi::UniCompositeRecord r4{"id-c-4", "Z", "B"};  // different col_a, same col_b -> allowed

        TEST_ASSERT( compDb.CreateRecord(r1), "Test C: Initial insert (A,B) failed");
        TEST_ASSERT(!compDb.CreateRecord(r2), "Test C: Duplicate (A,B) insert must be rejected by PostgreSQL");
        TEST_ASSERT( compDb.CreateRecord(r3), "Test C: Insert (A,C) must succeed");
        TEST_ASSERT( compDb.CreateRecord(r4), "Test C: Insert (Z,B) must succeed");

        TEST_ASSERT(DropPgTable(pgPool, "pg_uni_composite_test"), "Failed to drop table after Test C");
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test D: Repeated schema creation is idempotent (CREATE [UNIQUE] INDEX IF NOT EXISTS)
    // -------------------------------------------------------------------------
    {
        std::cout << "  - Test D: Repeated schema creation is idempotent (PostgreSQL)... " << std::flush;

        TEST_ASSERT(DropPgTable(pgPool, "pg_uni_idempotent_test"), "Failed to drop table before Test D");
        UniqueIndexDB uniqueDb(OpenWifi::DBType::pgsql, pgPool, logger, "pg_uni_idempotent_test");
        TEST_ASSERT(uniqueDb.Create(), "Test D: Initial Create() failed");
        TEST_ASSERT(uniqueDb.Create(), "Test D: Second Create() failed -- IF NOT EXISTS must be safe");

        // Verify the unique index is intact and strictly enforces uniqueness after repeated Create()
        OpenWifi::UniTestRecord r1{"id-d-1", "First", "IDEMPOTENT_VAL"};
        OpenWifi::UniTestRecord r2{"id-d-2", "Second", "IDEMPOTENT_VAL"};
        TEST_ASSERT( uniqueDb.CreateRecord(r1), "Test D: First insert after idempotent Create() failed");
        TEST_ASSERT(!uniqueDb.CreateRecord(r2), "Test D: Second insert with duplicate value must be rejected after idempotent Create()");

        TEST_ASSERT(DropPgTable(pgPool, "pg_uni_idempotent_test"), "Failed to drop table after Test D");
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test E: Transaction-aware duplicate write marks tx.HasFailed() (PostgreSQL)
    // -------------------------------------------------------------------------
    {
        std::cout << "  - Test E: Transaction-aware unique violation marks tx failed (PostgreSQL)... " << std::flush;

        TEST_ASSERT(DropPgTable(pgPool, "pg_uni_tx_test"), "Failed to drop table before Test E");
        UniqueIndexDB pgTxDb(OpenWifi::DBType::pgsql, pgPool, logger, "pg_uni_tx_test");
        TEST_ASSERT(pgTxDb.Create(), "Test E: Failed to create PostgreSQL tx unique-index test table");

        OpenWifi::UniTestRecord seed{"id-e-seed", "Seed", "TX_UNIQUE_VAL"};
        TEST_ASSERT(pgTxDb.CreateRecord(seed), "Test E: Seed insert failed");

        OpenWifi::DbTransaction tx(pgPool.get(), logger);
        OpenWifi::UniTestRecord dup{"id-e-dup", "Dup", "TX_UNIQUE_VAL"};  // same value
        bool dupResult = pgTxDb.CreateRecord(tx, dup);
        TEST_ASSERT(!dupResult,     "Test E: Duplicate transactional insert must return false on PostgreSQL");
        TEST_ASSERT(tx.HasFailed(), "Test E: tx.HasFailed() must be true after unique constraint violation");
        TEST_ASSERT(!tx.Commit(),   "Test E: tx.Commit() must return false after failed transaction");

        TEST_ASSERT(DropPgTable(pgPool, "pg_uni_tx_test"), "Failed to drop table after Test E");
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test F: DeleteRecord frees unique key for subsequent insertion (PostgreSQL)
    // -------------------------------------------------------------------------
    {
        std::cout << "  - Test F: DeleteRecord frees unique key for reuse (PostgreSQL)... " << std::flush;

        TEST_ASSERT(DropPgTable(pgPool, "pg_uni_delete_test"), "Failed to drop table before Test F");
        UniqueIndexDB delDb(OpenWifi::DBType::pgsql, pgPool, logger, "pg_uni_delete_test");
        TEST_ASSERT(delDb.Create(), "Test F: Failed to create delete test table");

        OpenWifi::UniTestRecord r1{"id-del-1", "Del Record", "REUSE_KEY"};
        TEST_ASSERT(delDb.CreateRecord(r1), "Test F: Initial insert failed");

        // Duplicate insert is rejected
        OpenWifi::UniTestRecord r2{"id-del-2", "Duplicate Record", "REUSE_KEY"};
        TEST_ASSERT(!delDb.CreateRecord(r2), "Test F: Duplicate was unexpectedly allowed before delete");

        // Delete original record
        TEST_ASSERT(delDb.DeleteRecord("id", std::string("id-del-1")), "Test F: DeleteRecord failed");

        // Now inserting with the same unique value must succeed
        TEST_ASSERT(delDb.CreateRecord(r2), "Test F: Re-inserting unique key after deletion failed");

        TEST_ASSERT(DropPgTable(pgPool, "pg_uni_delete_test"), "Failed to drop table after Test F");
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test G: Transaction Rollback frees unique key for subsequent transaction (PostgreSQL)
    // -------------------------------------------------------------------------
    {
        std::cout << "  - Test G: Transaction Rollback frees unique key (PostgreSQL)... " << std::flush;

        TEST_ASSERT(DropPgTable(pgPool, "pg_uni_rollback_test"), "Failed to drop table before Test G");
        UniqueIndexDB rbDb(OpenWifi::DBType::pgsql, pgPool, logger, "pg_uni_rollback_test");
        TEST_ASSERT(rbDb.Create(), "Test G: Failed to create rollback test table");

        // Transaction 1 inserts unique key but then rolls back
        {
            OpenWifi::DbTransaction tx1(pgPool.get(), logger);
            OpenWifi::UniTestRecord rTx{"id-rb-1", "Rollback Rec", "ROLLBACK_KEY"};
            TEST_ASSERT(rbDb.CreateRecord(tx1, rTx), "Test G: Insert in tx1 failed");
            TEST_ASSERT(tx1.Rollback(), "Test G: Rollback of tx1 failed");
        }

        // Transaction 2 should now be able to insert the same key and commit
        {
            OpenWifi::DbTransaction tx2(pgPool.get(), logger);
            OpenWifi::UniTestRecord rTx2{"id-rb-2", "Committed Rec", "ROLLBACK_KEY"};
            TEST_ASSERT(rbDb.CreateRecord(tx2, rTx2), "Test G: Insert in tx2 failed after tx1 rollback");
            TEST_ASSERT(tx2.Commit(), "Test G: Commit of tx2 failed");
        }

        TEST_ASSERT(DropPgTable(pgPool, "pg_uni_rollback_test"), "Failed to drop table after Test G");
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test H: Concurrent duplicate inserts race condition (multi-threaded, PostgreSQL)
    // -------------------------------------------------------------------------
    {
        std::cout << "  - Test H: Concurrent duplicate inserts race condition (PostgreSQL)... " << std::flush;

        TEST_ASSERT(DropPgTable(pgPool, "pg_uni_race_test"), "Failed to drop table before Test H");
        UniqueIndexDB raceDb(OpenWifi::DBType::pgsql, pgPool, logger, "pg_uni_race_test");
        TEST_ASSERT(raceDb.Create(), "Test H: Failed to create race test table");

        const int numRacers = 4;
        std::atomic<int> successCount{0};
        std::atomic<int> failCount{0};
        std::atomic<bool> startGate{false};

        std::vector<std::future<void>> futures;
        for (int i = 0; i < numRacers; ++i) {
            futures.push_back(std::async(std::launch::async, [&, i]() {
                while (!startGate.load()) {
                    std::this_thread::yield();
                }
                OpenWifi::UniTestRecord racerRec{
                    "racer-" + std::to_string(i),
                    "Worker " + std::to_string(i),
                    "SHARED_CONCURRENT_KEY"
                };
                if (raceDb.CreateRecord(racerRec)) {
                    successCount++;
                } else {
                    failCount++;
                }
            }));
        }

        // Open start gate
        startGate.store(true);

        for (auto &f : futures) {
            f.get();
        }

        // Exactly 1 thread must succeed, and all other (numRacers - 1) threads must fail
        TEST_ASSERT(successCount.load() == 1,
                    ("Test H: Expected exactly 1 insert to succeed, got " + std::to_string(successCount.load())).c_str());
        TEST_ASSERT(failCount.load() == numRacers - 1,
                    ("Test H: Expected " + std::to_string(numRacers - 1) + " inserts to fail, got " + std::to_string(failCount.load())).c_str());

        TEST_ASSERT(DropPgTable(pgPool, "pg_uni_race_test"), "Failed to drop table after Test H");
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test I: Pre-existing duplicates prevent Create() of unique index (PostgreSQL)
    // -------------------------------------------------------------------------
    {
        std::cout << "  - Test I: Pre-existing duplicates cause Create() to fail (PostgreSQL)... " << std::flush;

        TEST_ASSERT(DropPgTable(pgPool, "pg_uni_preexist_dup_test"), "Failed to drop table before Test I");

        // Step 1: Create table with existing non-unique schema (using NormalIndexDB)
        NormalIndexDB preExistDb(OpenWifi::DBType::pgsql, pgPool, logger, "pg_uni_preexist_dup_test");
        TEST_ASSERT(preExistDb.Create(), "Test I: Failed to create initial normal table");

        // Step 2: Insert duplicate rows into the table
        OpenWifi::UniTestRecord d1{"dup-1", "First Dup", "DUPLICATE_KEY"};
        OpenWifi::UniTestRecord d2{"dup-2", "Second Dup", "DUPLICATE_KEY"};
        TEST_ASSERT(preExistDb.CreateRecord(d1), "Test I: First insert failed");
        TEST_ASSERT(preExistDb.CreateRecord(d2), "Test I: Second insert with duplicate value failed");

        // Step 3: Now attempt to initialize schema on the same table with UniqueIndexDB (Unique=true)
        UniqueIndexDB attemptUniqueDb(OpenWifi::DBType::pgsql, pgPool, logger, "pg_uni_preexist_dup_test");
        bool createResult = attemptUniqueDb.Create();

        // Step 4: Verify Create() returns false because PostgreSQL rejects creating unique index on duplicate data
        TEST_ASSERT(!createResult, "Test I: Create() must return false when unique index cannot be created due to existing duplicates");

        TEST_ASSERT(DropPgTable(pgPool, "pg_uni_preexist_dup_test"), "Failed to drop table after Test I");
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test J: Same-name non-unique index causes Unique=true Create() to return false (PostgreSQL)
    //
    // Covers the case where IF NOT EXISTS silently skips creation because an index
    // with the requested name already exists as a normal (non-unique) index.
    // Without the pg_index verification added to ORM::DB::Create(), this would
    // falsely succeed while leaving uniqueness unenforced.
    // -------------------------------------------------------------------------
    {
        std::cout << "  - Test J: Same-name non-unique index causes Unique=true Create() to fail (PostgreSQL)... " << std::flush;

        const char *kTableName = "pg_uni_samename_test";
        TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop table before Test J");

        // Step 1: Create the table with a normal (non-unique) index under the name that
        //         UniqueIndexDB will later declare as unique.
        //         UniqueIndexDB generates index name = TableName + "_unique_idx", so create
        //         a normal index with that exact name using a raw SQL statement.
        {
            Poco::Data::Session s = pgPool.get();
            s << "CREATE TABLE " + std::string(kTableName) +
                 " (id TEXT PRIMARY KEY, name TEXT, value TEXT)",
                Poco::Data::Keywords::now;
            // Normal (non-unique) index using the same name UniqueIndexDB would use
            s << "CREATE INDEX IF NOT EXISTS " + std::string(kTableName) + "_unique_idx"
                 " ON " + std::string(kTableName) + " (value ASC)",
                Poco::Data::Keywords::now;
        }

        // Step 2: Initialize UniqueIndexDB over the same table — it will attempt
        //         CREATE UNIQUE INDEX IF NOT EXISTS <name>_unique_idx, which PostgreSQL
        //         will skip because the name is taken. ORM must detect the surviving
        //         index is non-unique and return false.
        UniqueIndexDB sameNameDb(OpenWifi::DBType::pgsql, pgPool, logger, kTableName);
        bool createResult = sameNameDb.Create();
        TEST_ASSERT(!createResult,
                    "Test J: Create() must return false when a same-name non-unique index already exists");

        TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop table after Test J");
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test K: Same-name unique index on a WRONG column causes Create() to return false.
    //
    // Covers the gap that Test J does not: an existing index has the same name,
    // IS unique, but covers a different column than the one declared in IndexEntryVec.
    // After CREATE UNIQUE INDEX IF NOT EXISTS skips creation, the ORM must verify
    // that the surviving index covers the exact declared columns -- not just that it
    // happens to be some unique index with the right name.
    // -------------------------------------------------------------------------
    {
        std::cout << "  - Test K: Same-name unique index on wrong column causes Create() to fail (PostgreSQL)... " << std::flush;

        const char *kTableName = "pg_uni_wrongcol_test";
        TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop table before Test K");

        // Step 1: Create the table and plant a UNIQUE index on 'name', using the same
        //         index name that UniqueIndexDB would generate for 'value'.
        //         UniqueIndexDB generates: <TableName>_unique_idx on (value ASC).
        //         We pre-create a unique index with the same name but on 'name' instead.
        {
            Poco::Data::Session s = pgPool.get();
            s << "CREATE TABLE " + std::string(kTableName) +
                 " (id TEXT PRIMARY KEY, name TEXT, value TEXT)",
                Poco::Data::Keywords::now;
            // Unique index using the name UniqueIndexDB would claim, but on 'name' not 'value'.
            s << "CREATE UNIQUE INDEX IF NOT EXISTS " + std::string(kTableName) + "_unique_idx"
                 " ON " + std::string(kTableName) + " (name ASC)",
                Poco::Data::Keywords::now;
        }

        // Step 2: Initialize UniqueIndexDB which declares Unique on 'value'.
        //         PostgreSQL will skip creation (name taken). The ORM must detect
        //         the column mismatch (existing covers 'name', declared covers 'value')
        //         and return false.
        UniqueIndexDB wrongColDb(OpenWifi::DBType::pgsql, pgPool, logger, kTableName);
        bool createResult = wrongColDb.Create();
        TEST_ASSERT(!createResult,
                    "Test K: Create() must return false when a same-name unique index covers a different column");

        TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop table after Test K");
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test L: Same-name partial unique index on expected column causes Create() to return false.
    //
    // Covers the case where an existing index has the same name, is UNIQUE, and covers
    // the declared column ('value'), but has a WHERE predicate (partial index).
    // The ORM requires an unconditional, full-table unique index. CREATE UNIQUE INDEX
    // IF NOT EXISTS skips creation, and the ORM must verify that the surviving index
    // is not partial (indpred is null) and return false.
    // -------------------------------------------------------------------------
    {
        std::cout << "  - Test L: Same-name partial unique index causes Create() to fail (PostgreSQL)... " << std::flush;

        const char *kTableName = "pg_uni_partial_test";
        TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop table before Test L");

        // Step 1: Create the table and plant a PARTIAL UNIQUE index on 'value'
        //         using the name UniqueIndexDB would claim: <TableName>_unique_idx.
        {
            Poco::Data::Session s = pgPool.get();
            s << "CREATE TABLE " + std::string(kTableName) +
                 " (id TEXT PRIMARY KEY, name TEXT, value TEXT)",
                Poco::Data::Keywords::now;
            // Partial unique index with a WHERE clause
            s << "CREATE UNIQUE INDEX IF NOT EXISTS " + std::string(kTableName) + "_unique_idx"
                 " ON " + std::string(kTableName) + " (value ASC) WHERE value IS NOT NULL",
                Poco::Data::Keywords::now;
        }

        // Step 2: Initialize UniqueIndexDB which declares an unconditional unique index on 'value'.
        //         PostgreSQL will skip creation (name taken). The ORM must detect
        //         that the existing index is partial (indpred IS NOT NULL) and return false.
        UniqueIndexDB partialDb(OpenWifi::DBType::pgsql, pgPool, logger, kTableName);
        bool createResult = partialDb.Create();
        TEST_ASSERT(!createResult,
                    "Test L: Create() must return false when a same-name unique index is partial (has WHERE clause)");

        TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop table after Test L");
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test M: Multi-schema isolation: same-name table and index in another schema
    //         cannot cause active schema unique-index verification to falsely pass or fail.
    // -------------------------------------------------------------------------
    {
        std::cout << "  - Test M: Multi-schema isolation (PostgreSQL)... " << std::flush;

        const char *kTableName = "pg_uni_multischema_test";
        const std::string altSchema = "alt_uni_test_schema";

        // Cleanup before test
        TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop table before Test M");
        {
            Poco::Data::Session s = pgPool.get();
            s << "DROP SCHEMA IF EXISTS " + altSchema + " CASCADE", Poco::Data::Keywords::now;
            s << "CREATE SCHEMA " + altSchema, Poco::Data::Keywords::now;
        }

        // Sub-test M1: Incompatible (non-unique on wrong column) same-name table and index
        //              pre-created in altSchema must NOT cause Create() in the active schema to fail.
        {
            {
                Poco::Data::Session s = pgPool.get();
                // Create table in altSchema with same table name
                s << "CREATE TABLE " + altSchema + "." + std::string(kTableName) +
                     " (id TEXT PRIMARY KEY, name TEXT, value TEXT)",
                    Poco::Data::Keywords::now;
                // Create a non-unique index with the same name that UniqueIndexDB will use
                s << "CREATE INDEX " + std::string(kTableName) + "_unique_idx"
                     " ON " + altSchema + "." + std::string(kTableName) + " (name ASC)",
                    Poco::Data::Keywords::now;
            }

            // Create UniqueIndexDB in active/default search path (public).
            // Under un-scoped relname matching, this would see altSchema's non-unique index
            // and falsely fail. With schema/OID scoping, it must succeed.
            UniqueIndexDB uniDbM1(OpenWifi::DBType::pgsql, pgPool, logger, kTableName);
            bool createResultM1 = uniDbM1.Create();
            TEST_ASSERT(createResultM1,
                        "Test M1: Create() unexpectedly failed due to same-name incompatible index in another schema");

            // Verify active schema table enforces uniqueness
            OpenWifi::UniTestRecord r1{"m1-1", "Name 1", "DUP_VAL"};
            OpenWifi::UniTestRecord r2{"m1-2", "Name 2", "DUP_VAL"};
            TEST_ASSERT(uniDbM1.CreateRecord(r1), "Test M1: First insert failed");
            TEST_ASSERT(!uniDbM1.CreateRecord(r2), "Test M1: Duplicate insert must be rejected in active schema");

            TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop active table after Test M1");
            {
                Poco::Data::Session s = pgPool.get();
                s << "DROP TABLE IF EXISTS " + altSchema + "." + std::string(kTableName) + " CASCADE",
                    Poco::Data::Keywords::now;
            }
        }

        // Sub-test M2: A valid unique index in altSchema must NOT mask a defective
        //              (non-unique) index in the active schema.
        {
            // Plant a defective (non-unique) index in active schema
            {
                Poco::Data::Session s = pgPool.get();
                s << "CREATE TABLE " + std::string(kTableName) +
                     " (id TEXT PRIMARY KEY, name TEXT, value TEXT)",
                    Poco::Data::Keywords::now;
                s << "CREATE INDEX IF NOT EXISTS " + std::string(kTableName) + "_unique_idx"
                     " ON " + std::string(kTableName) + " (value ASC)",
                    Poco::Data::Keywords::now;
            }

            // Plant a valid unique index in altSchema with the same name
            {
                Poco::Data::Session s = pgPool.get();
                s << "CREATE TABLE " + altSchema + "." + std::string(kTableName) +
                     " (id TEXT PRIMARY KEY, name TEXT, value TEXT)",
                    Poco::Data::Keywords::now;
                s << "CREATE UNIQUE INDEX IF NOT EXISTS " + std::string(kTableName) + "_unique_idx"
                     " ON " + altSchema + "." + std::string(kTableName) + " (value ASC)",
                    Poco::Data::Keywords::now;
            }

            // Attempt UniqueIndexDB::Create() on active schema table.
            // Active schema index is non-unique, so Create() must FAIL.
            // Scoping ensures altSchema's valid unique index does not cause a false pass.
            UniqueIndexDB uniDbM2(OpenWifi::DBType::pgsql, pgPool, logger, kTableName);
            bool createResultM2 = uniDbM2.Create();
            TEST_ASSERT(!createResultM2,
                        "Test M2: Create() unexpectedly passed despite active schema index being non-unique (masked by altSchema)");

            TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop active table after Test M2");
            {
                Poco::Data::Session s = pgPool.get();
                s << "DROP SCHEMA IF EXISTS " + altSchema + " CASCADE", Poco::Data::Keywords::now;
            }
        }

        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test N: Earlier normal index DDL failure causes Create() to return false (PostgreSQL)
    //
    // Regression test: failure during earlier normal index DDL must NOT be swallowed
    // by the outer PostgreSQL catch block and fall through to Upgrade() success.
    // Pre-creates a physical table with an older schema (missing column 'name').
    // NormalThenUniqueDB declares:
    //   1. Normal index on 'name' FIRST (fails because 'name' is missing physically)
    //   2. Required UNIQUE index on 'value' SECOND (never reached/created)
    // Create() must return false and must not report schema creation success.
    // -------------------------------------------------------------------------
    {
        std::cout << "  - Test N: Normal index DDL failure causes Create() to fail (PostgreSQL)... " << std::flush;

        const char *kTableName = "pg_uni_ddl_fail_test";
        TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop table before Test N");

        // Step 1: Pre-create physical table with an older/incomplete schema
        //         Contains 'id' and 'value', but MISSING column 'name'.
        {
            Poco::Data::Session s = pgPool.get();
            s << "CREATE TABLE " + std::string(kTableName) +
                 " (id TEXT PRIMARY KEY, value TEXT)",
                Poco::Data::Keywords::now;
        }

        // Step 2: Initialize NormalThenUniqueDB which declares a normal index on 'name'
        //         FIRST, followed by a unique index on 'value' SECOND.
        //         Table creation (CREATE TABLE IF NOT EXISTS) succeeds, but normal index
        //         creation on 'name' fails (column 'name' does not exist in physical table).
        //         Create() must propagate this failure and return false (not fall through to Upgrade()).
        NormalThenUniqueDB failDb(OpenWifi::DBType::pgsql, pgPool, logger, kTableName);
        bool createResult = failDb.Create();
        TEST_ASSERT(!createResult,
                    "Test N: Create() must return false when earlier normal index DDL fails, and must not fall through to Upgrade() success");

        TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop table after Test N");
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test O: Mixed-case unique index name (PostgreSQL)
    //
    // Verifies that an unquoted mixed-case index identifier (e.g. DeviceUniqueIndex)
    // is folded to lowercase by PostgreSQL, matches during ORM verification via
    // PostgreSQL's identifier normalization rules, and enforces uniqueness.
    // -------------------------------------------------------------------------
    {
        std::cout << "  - Test O: Mixed-case unique index name (PostgreSQL)... " << std::flush;

        const char *kTableName = "pg_uni_mixedcase_test";
        TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop table before Test O");

        MixedCaseUniqueDB mixedDb(OpenWifi::DBType::pgsql, pgPool, logger, kTableName);
        TEST_ASSERT(mixedDb.Create(), "Test O: Failed to create table with mixed-case unique index");

        OpenWifi::UniTestRecord r1{"id-mc-1", "Record 1", "MC_UNIQUE_VAL"};
        OpenWifi::UniTestRecord r2{"id-mc-2", "Record 2", "MC_UNIQUE_VAL"}; // duplicate -> must fail
        OpenWifi::UniTestRecord r3{"id-mc-3", "Record 3", "MC_DIFFERENT_VAL"};

        TEST_ASSERT( mixedDb.CreateRecord(r1), "Test O: First insert failed unexpectedly");
        TEST_ASSERT(!mixedDb.CreateRecord(r2), "Test O: Duplicate insert must be rejected by PostgreSQL");
        TEST_ASSERT( mixedDb.CreateRecord(r3), "Test O: Non-duplicate insert failed unexpectedly");

        TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop table after Test O");
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test P: Overlength unique index name (PostgreSQL)
    //
    // Verifies that an index identifier longer than PostgreSQL's identifier limit
    // (NAMEDATALEN - 1 = 63 bytes) is truncated by PostgreSQL, matches during ORM
    // verification via PostgreSQL's identifier normalization rules, and enforces uniqueness.
    // -------------------------------------------------------------------------
    {
        std::cout << "  - Test P: Overlength unique index name (PostgreSQL)... " << std::flush;

        const char *kTableName = "pg_uni_overlength_test";
        TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop table before Test P");

        OverlengthUniqueDB overlengthDb(OpenWifi::DBType::pgsql, pgPool, logger, kTableName);
        TEST_ASSERT(overlengthDb.Create(), "Test P: Failed to create table with overlength unique index");

        OpenWifi::UniTestRecord r1{"id-ol-1", "Record 1", "OL_UNIQUE_VAL"};
        OpenWifi::UniTestRecord r2{"id-ol-2", "Record 2", "OL_UNIQUE_VAL"}; // duplicate -> must fail
        OpenWifi::UniTestRecord r3{"id-ol-3", "Record 3", "OL_DIFFERENT_VAL"};

        TEST_ASSERT( overlengthDb.CreateRecord(r1), "Test P: First insert failed unexpectedly");
        TEST_ASSERT(!overlengthDb.CreateRecord(r2), "Test P: Duplicate insert must be rejected by PostgreSQL");
        TEST_ASSERT( overlengthDb.CreateRecord(r3), "Test P: Non-duplicate insert failed unexpectedly");

        TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop table after Test P");
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test Q: Conflicting index with INCLUDE column causes Create() to return false (PostgreSQL)
    //
    // Verifies that an existing index with matching key name but defined as:
    //   CREATE UNIQUE INDEX <index_name> ON <table> (col_a) INCLUDE (col_b)
    // is rejected by Create() for an ORM declaration expecting:
    //   UNIQUE (col_a, col_b)
    // indnkeyatts is 1 while Decl.Entries is 2, and indnatts != indnkeyatts.
    // -------------------------------------------------------------------------
    {
        std::cout << "  - Test Q: Conflicting index with INCLUDE column causes Create() to fail (PostgreSQL)... " << std::flush;

        const char *kTableName = "pg_uni_include_conflict_test";
        TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop table before Test Q");

        // Step 1: Pre-create physical table with columns id, col_a, col_b.
        {
            Poco::Data::Session s = pgPool.get();
            s << "CREATE TABLE " + std::string(kTableName) +
                 " (id TEXT PRIMARY KEY, col_a TEXT, col_b TEXT)",
                Poco::Data::Keywords::now;
            // Step 2: Pre-create same-name conflicting index with INCLUDE (col_b) instead of key (col_a, col_b).
            s << "CREATE UNIQUE INDEX " + std::string(kTableName) + "_composite_idx"
                 " ON " + std::string(kTableName) + " (col_a) INCLUDE (col_b)",
                Poco::Data::Keywords::now;
        }

        // Step 3: Initialize CompositeUniqueDB which expects UNIQUE on (col_a, col_b).
        //         Create() must detect that indnkeyatts != 2 (or indnatts != indnkeyatts)
        //         and return false.
        CompositeUniqueDB conflictDb(OpenWifi::DBType::pgsql, pgPool, logger, kTableName);
        bool createResult = conflictDb.Create();
        TEST_ASSERT(!createResult,
                    "Test Q: Create() must return false when existing index has INCLUDE (col_b) instead of key (col_a, col_b)");

        TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop table after Test Q");
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test R: Deferrable unique constraint causes Create() to return false (PostgreSQL)
    //
    // Verifies that an existing unique constraint declared as DEFERRABLE INITIALLY DEFERRED
    // is rejected by Create() because pg_index.indimmediate is false, which does not match
    // the immediate uniqueness enforcement expected by the ORM.
    // -------------------------------------------------------------------------
    {
        std::cout << "  - Test R: Deferrable unique constraint causes Create() to fail (PostgreSQL)... " << std::flush;

        const char *kTableName = "pg_uni_deferrable_test";
        TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop table before Test R");

        // Step 1: Pre-create physical table.
        {
            Poco::Data::Session s = pgPool.get();
            s << "CREATE TABLE " + std::string(kTableName) +
                 " (id TEXT PRIMARY KEY, name TEXT, value TEXT)",
                Poco::Data::Keywords::now;
            // Step 2: Add deferrable unique constraint using the index name UniqueIndexDB expects.
            s << "ALTER TABLE " + std::string(kTableName) +
                 " ADD CONSTRAINT " + std::string(kTableName) + "_unique_idx"
                 " UNIQUE (value) DEFERRABLE INITIALLY DEFERRED",
                Poco::Data::Keywords::now;
        }

        // Step 3: Initialize UniqueIndexDB which expects immediate UNIQUE on 'value'.
        //         Create() must reject the deferrable constraint (indimmediate = false).
        UniqueIndexDB defDb(OpenWifi::DBType::pgsql, pgPool, logger, kTableName);
        bool createResult = defDb.Create();
        TEST_ASSERT(!createResult,
                    "Test R: Create() must return false when existing constraint is DEFERRABLE INITIALLY DEFERRED");

        TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop table after Test R");
        std::cout << "PASSED" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test S: Quoted column "Value" unique index causes Create() to return false (PostgreSQL)
    //
    // Verifies that when an existing table has both unquoted value and quoted "Value",
    // and an existing unique index with the expected name covers "Value",
    // Create() rejects the index because exact catalog attname "Value" does not match
    // the expected normalized column name "value".
    // -------------------------------------------------------------------------
    {
        std::cout << "  - Test S: Unique index on quoted \"Value\" causes Create() to fail (PostgreSQL)... " << std::flush;

        const char *kTableName = "pg_uni_quoted_col_conflict_test";
        TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop table before Test S");

        // Step 1: Pre-create physical table with both 'value' and '"Value"'.
        {
            Poco::Data::Session s = pgPool.get();
            s << "CREATE TABLE " + std::string(kTableName) +
                 " (id TEXT PRIMARY KEY, name TEXT, value TEXT, \"Value\" TEXT)",
                Poco::Data::Keywords::now;
            // Step 2: Pre-create unique index on quoted "Value" using expected index name.
            s << "CREATE UNIQUE INDEX " + std::string(kTableName) + "_unique_idx"
                 " ON " + std::string(kTableName) + " (\"Value\")",
                Poco::Data::Keywords::now;
        }

        // Step 3: Initialize UniqueIndexDB which expects unique index on unquoted 'value'.
        //         Create() must reject the existing index on "Value".
        UniqueIndexDB quotedColDb(OpenWifi::DBType::pgsql, pgPool, logger, kTableName);
        bool createResult = quotedColDb.Create();
        TEST_ASSERT(!createResult,
                    "Test S: Create() must return false when existing unique index covers quoted \"Value\" instead of unquoted value");

        TEST_ASSERT(DropPgTable(pgPool, kTableName), "Failed to drop table after Test S");
        std::cout << "PASSED" << std::endl;
    }

    std::cout << "[Framework Unit Test] All PostgreSQL Unique-Index ORM Foundation Tests Passed Successfully!" << std::endl;
    // Total: 2 backend-agnostic + 19 PostgreSQL tests (A-S).
    return 0;
#endif
}
