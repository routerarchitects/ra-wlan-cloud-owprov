/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 */

#include <iostream>
#include <cstdlib>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <future>
#include <map>

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

	// Minimal record struct for testing row-level locking
	struct TestRecord {
		std::string id;
		std::string name;
		std::string value;

		void to_json(Poco::JSON::Object &) const {}
		bool from_json(const Poco::JSON::Object::Ptr &) { return true; }
	};

	typedef Poco::Tuple<std::string, std::string, std::string> TestRecordTuple;

	// Mock DBCache to verify that GetRecordForUpdate strictly bypasses cache reads and writes
	class MockTestDBCache : public ORM::DBCache<TestRecord> {
	  public:
		MockTestDBCache() : DBCache<TestRecord>(100, 600) {}

		void Create(const TestRecord &R) override {
			createCalled_ = true;
			cache_map_[R.id] = R;
		}

		bool GetFromCache(const std::string &FieldName, const std::string &Value, TestRecord &R) override {
			getFromCacheCalled_ = true;
			if (FieldName == "id") {
				auto it = cache_map_.find(Value);
				if (it != cache_map_.end()) {
					R = it->second;
					return true;
				}
			}
			return false;
		}

		void UpdateCache(const TestRecord &R) override {
			updateCacheCalled_ = true;
			cache_map_[R.id] = R;
		}

		void Delete(const std::string &FieldName, const std::string &Value) override {
			if (FieldName == "id") {
				cache_map_.erase(Value);
			}
		}

		void InvalidateAll() override {
			cache_map_.clear();
		}

		void Put(const TestRecord &R) {
			cache_map_[R.id] = R;
		}

		const TestRecord &Get(const std::string &id) const {
			return cache_map_.at(id);
		}

		bool GetFromCacheCalled() const { return getFromCacheCalled_; }
		bool UpdateCacheCalled() const { return updateCacheCalled_; }
		bool CreateCalled() const { return createCalled_; }

		void ResetFlags() {
			getFromCacheCalled_ = false;
			updateCacheCalled_ = false;
			createCalled_ = false;
		}

	  private:
		std::map<std::string, TestRecord> cache_map_;
		bool getFromCacheCalled_ = false;
		bool updateCacheCalled_ = false;
		bool createCalled_ = false;
	};

} // namespace OpenWifi

template <>
void ORM::DB<OpenWifi::TestRecordTuple, OpenWifi::TestRecord>::Convert(
	const OpenWifi::TestRecordTuple &In, OpenWifi::TestRecord &Out) {
	Out.id = In.get<0>();
	Out.name = In.get<1>();
	Out.value = In.get<2>();
}

template <>
void ORM::DB<OpenWifi::TestRecordTuple, OpenWifi::TestRecord>::Convert(
	const OpenWifi::TestRecord &In, OpenWifi::TestRecordTuple &Out) {
	Out.set<0>(In.id);
	Out.set<1>(In.name);
	Out.set<2>(In.value);
}

class TestDB : public ORM::DB<OpenWifi::TestRecordTuple, OpenWifi::TestRecord> {
  public:
	TestDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L,
	       ORM::DBCache<OpenWifi::TestRecord> *Cache = nullptr,
	       const char *TableName = "rowlock_test_records")
		: DB(T, TableName,
			 ORM::FieldVec{
				 ORM::Field{"id", ORM::FieldType::FT_TEXT, 0, true},
				 ORM::Field{"name", ORM::FieldType::FT_TEXT},
				 ORM::Field{"value", ORM::FieldType::FT_TEXT}
			 },
			 ORM::IndexVec{}, P, L, "rlt", Cache) {}
};

#ifndef SMALL_BUILD
// Verifies via PostgreSQL pg_blocking_pids() that targetPid (tx2) is specifically
// blocked by expectedBlockerPid (tx1). This deterministically proves lock contention
// between tx1 and tx2 rather than an unrelated lock wait.
static bool WaitForBlockerPid(Poco::Data::SessionPool &pool, uint64_t targetPid, uint64_t expectedBlockerPid, int timeoutMs = 4000) {
	auto start = std::chrono::steady_clock::now();
	std::string sql = "SELECT CASE WHEN " + std::to_string(expectedBlockerPid) +
	                  " = ANY(pg_blocking_pids(" + std::to_string(targetPid) + ")) THEN 1 ELSE 0 END";
	while (std::chrono::duration_cast<std::chrono::milliseconds>(
	           std::chrono::steady_clock::now() - start).count() < timeoutMs) {
		try {
			Poco::Data::Session monSession = pool.get();
			Poco::Data::Statement stmt(monSession);
			int isBlocked = 0;
			stmt << sql,
			        Poco::Data::Keywords::into(isBlocked),
			        Poco::Data::Keywords::now;
			if (isBlocked == 1) {
				return true;
			}
		} catch (const std::exception &e) {
			std::cerr << "WaitForBlockerPid exception: " << e.what() << std::endl;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

static bool DropTestTable(Poco::Data::SessionPool &pool) {
	try {
		Poco::Data::Session cleanSession = pool.get();
		Poco::Data::Statement dropStmt(cleanSession);
		dropStmt << "DROP TABLE IF EXISTS rowlock_test_records";
		dropStmt.execute();
		return true;
	} catch (const std::exception &e) {
		std::cerr << "DropTestTable exception: " << e.what() << std::endl;
		return false;
	}
}
#endif

int main() {
	std::cout << "[Framework Unit Test] Initializing Row-Level Locking Tests..." << std::endl;
	Poco::AutoPtr<Poco::ConsoleChannel> pChannel(new Poco::ConsoleChannel);
	Poco::Logger &logger = Poco::Logger::get("RowLevelLockingTest");
	logger.setChannel(pChannel);

	// -------------------------------------------------------------------------
	// Test E: Unsupported backend rejection check (SQLite)
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test E: Unsupported backend rejection (SQLite)... " << std::flush;
		Poco::Data::SQLite::Connector::registerConnector();
		Poco::Data::SessionPool sqlitePool("SQLite", "rowlock_sqlite_unittest.db");

		TestDB sqliteDb(OpenWifi::DBType::sqlite, sqlitePool, logger, nullptr, "sqlite_rowlock_test_records");
		OpenWifi::DbTransaction txSqlite(sqlitePool.get(), logger);
		OpenWifi::TestRecord outRec;
		bool result = sqliteDb.GetRecordForUpdate(txSqlite, "id", std::string("test-id"), outRec);
		TEST_ASSERT(!result, "GetRecordForUpdate on SQLite unexpectedly succeeded");
		TEST_ASSERT(txSqlite.HasFailed(), "tx.HasFailed() was not set after GetRecordForUpdate on SQLite!");
		TEST_ASSERT(!txSqlite.Commit(), "tx.Commit() unexpectedly succeeded on failed SQLite transaction!");

		std::cout << "PASSED" << std::endl;
	}

#ifdef SMALL_BUILD
	std::cout << "[Framework Unit Test] SMALL_BUILD enabled: skipping PostgreSQL tests." << std::endl;
	return 77;
#else
	// -------------------------------------------------------------------------
	// Check for PostgreSQL environment configuration
	// -------------------------------------------------------------------------
	const char *pgHost = std::getenv("PGHOST");
	if (!pgHost || std::string(pgHost).empty()) {
		pgHost = std::getenv("TEST_POSTGRES_HOST");
	}

	if (!pgHost || std::string(pgHost).empty()) {
		std::cout << "[Framework Unit Test] PGHOST / TEST_POSTGRES_HOST not set." << std::endl;
		std::cout << "[Framework Unit Test] Skipping live PostgreSQL tests (Tests A-D, F, G) with CTest skip code 77." << std::endl;
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
	Poco::Data::SessionPool pool("PostgreSQL", connStr, 4, 32, 60);

	// Verify database connectivity
	try {
		Poco::Data::Session testSession = pool.get();
		Poco::Data::Statement ping(testSession);
		int val = 0;
		ping << "SELECT 1", Poco::Data::Keywords::into(val), Poco::Data::Keywords::now;
		TEST_ASSERT(val == 1, "Ping SELECT 1 returned unexpected value");
	} catch (const std::exception &e) {
		std::cerr << "Failed to connect to PostgreSQL at " << host << ":" << port
		          << " (" << e.what() << ")" << std::endl;
		std::exit(1);
	}

	// Drop table if it already exists so the test always creates its expected schema from a clean state.
	// Initial setup MUST fail if PostgreSQL cleanup cannot be performed.
	TEST_ASSERT(DropTestTable(pool), "Initial cleanup failed: could not execute DROP TABLE IF EXISTS on PostgreSQL");

	TestDB pgDbInstance(OpenWifi::DBType::pgsql, pool, logger, nullptr, "rowlock_test_records");
	TEST_ASSERT(pgDbInstance.Create(), "Failed to create PostgreSQL test table");

	// -------------------------------------------------------------------------
	// Test A: Locked row is serialized (deterministic PostgreSQL lock contention)
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test A: Locked row is serialized (deterministic lock contention)... " << std::flush;
		OpenWifi::TestRecord recA{"rec-lock-001", "Row A", "Initial Value"};
		TEST_ASSERT(pgDbInstance.CreateRecord(recA), "Failed to insert initial record recA");

		OpenWifi::DbTransaction tx1(pool.get(), logger);
		uint64_t tx1Pid = 0;
		Poco::Data::Statement pid1Stmt(tx1.Session());
		pid1Stmt << "SELECT pg_backend_pid()", Poco::Data::Keywords::into(tx1Pid), Poco::Data::Keywords::now;
		TEST_ASSERT(tx1Pid > 0, "Failed to retrieve tx1 backend PID");

		OpenWifi::TestRecord recA_tx1;
		TEST_ASSERT(pgDbInstance.GetRecordForUpdate(tx1, "id", std::string("rec-lock-001"), recA_tx1),
		            "tx1 failed to acquire lock on recA");
		TEST_ASSERT(recA_tx1.value == "Initial Value", "recA initial value mismatch");

		// Part A1: Verify PostgreSQL lock_timeout behavior on conflicting row lock.
		// A session with lock_timeout set must abort on GetRecordForUpdate() while tx1 holds the lock.
		{
			OpenWifi::DbTransaction txTimeout(pool.get(), logger);
			Poco::Data::Statement setStmt(txTimeout.Session());
			setStmt << "SET LOCAL lock_timeout = '150ms'", Poco::Data::Keywords::now;

			OpenWifi::TestRecord recA_timedOut;
			bool acquired = pgDbInstance.GetRecordForUpdate(txTimeout, "id", std::string("rec-lock-001"), recA_timedOut);
			TEST_ASSERT(!acquired, "txTimeout unexpectedly acquired row lock that was held by tx1!");
			TEST_ASSERT(txTimeout.HasFailed(), "txTimeout was not marked failed on PostgreSQL lock timeout!");
			TEST_ASSERT(!txTimeout.Commit(), "txTimeout commit succeeded after lock timeout failure!");
		}

		// Part A2: Verify that tx2 is specifically blocked by tx1 using pg_blocking_pids(), and serialization across commit.
		std::atomic<uint64_t> tx2Pid{0};
		std::atomic<bool> tx2Acquired{false};
		OpenWifi::TestRecord recA_tx2;

		auto fut2 = std::async(std::launch::async, [&]() {
			try {
				OpenWifi::DbTransaction tx2(pool.get(), logger);
				Poco::Data::Statement pidStmt(tx2.Session());
				uint64_t pid = 0;
				pidStmt << "SELECT pg_backend_pid()", Poco::Data::Keywords::into(pid), Poco::Data::Keywords::now;
				tx2Pid = pid;

				bool ok = pgDbInstance.GetRecordForUpdate(tx2, "id", std::string("rec-lock-001"), recA_tx2);
				if (ok) {
					tx2Acquired = true;
					return tx2.Commit();
				}
			} catch (const std::exception &e) {
				std::cerr << "tx2 exception in Test A: " << e.what() << std::endl;
			} catch (...) {
				std::cerr << "tx2 unknown exception in Test A" << std::endl;
			}
			return false;
		});

		// Wait until tx2 session PID is established with a bounded timeout and premature failure check
		auto startWaitA = std::chrono::steady_clock::now();
		while (tx2Pid.load() == 0) {
			if (fut2.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
				TEST_ASSERT(false, "tx2 async worker terminated prematurely before publishing its backend PID!");
			}
			if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startWaitA).count() > 5) {
				TEST_ASSERT(false, "Timed out waiting for tx2 to publish its backend PID!");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		// Explicitly observe that tx2's PID is specifically blocked by tx1's PID in PostgreSQL
		bool lockContentionObserved = WaitForBlockerPid(pool, tx2Pid.load(), tx1Pid, 4000);
		TEST_ASSERT(lockContentionObserved,
		            "PostgreSQL did not observe tx2 being blocked specifically by tx1 via pg_blocking_pids()!");
		TEST_ASSERT(!tx2Acquired.load(),
		            "tx2 acquired row before tx1 committed (lock contention was not held)!");

		// tx1 modifies and updates the record
		recA_tx1.value = "Updated By TX1";
		TEST_ASSERT(pgDbInstance.UpdateRecord(tx1, "id", std::string("rec-lock-001"), recA_tx1),
		            "tx1 UpdateRecord failed");

		// tx1 commits, releasing the PostgreSQL row lock
		TEST_ASSERT(tx1.Commit(), "tx1 Commit failed");

		// tx2 must now unblock and acquire the updated row
		auto status = fut2.wait_for(std::chrono::seconds(5));
		TEST_ASSERT(status == std::future_status::ready, "Timed out waiting for tx2 after tx1 Commit");
		bool tx2Success = fut2.get();
		TEST_ASSERT(tx2Success, "tx2 failed to commit after acquiring row");
		TEST_ASSERT(tx2Acquired.load(), "tx2 failed to acquire row after tx1 committed");
		TEST_ASSERT(recA_tx2.value == "Updated By TX1", "tx2 did not observe committed update from tx1");

		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test B: Rollback releases lock (deterministic contention verification)
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test B: Rollback releases lock (deterministic contention)... " << std::flush;
		OpenWifi::TestRecord recB{"rec-lock-002", "Row B", "Initial Value B"};
		TEST_ASSERT(pgDbInstance.CreateRecord(recB), "Failed to insert initial record recB");

		OpenWifi::DbTransaction tx1(pool.get(), logger);
		uint64_t tx1Pid = 0;
		Poco::Data::Statement pid1Stmt(tx1.Session());
		pid1Stmt << "SELECT pg_backend_pid()", Poco::Data::Keywords::into(tx1Pid), Poco::Data::Keywords::now;
		TEST_ASSERT(tx1Pid > 0, "Failed to retrieve tx1 backend PID");

		OpenWifi::TestRecord recB_tx1;
		TEST_ASSERT(pgDbInstance.GetRecordForUpdate(tx1, "id", std::string("rec-lock-002"), recB_tx1),
		            "tx1 failed to acquire lock on recB");

		std::atomic<uint64_t> tx2Pid{0};
		std::atomic<bool> tx2Acquired{false};
		OpenWifi::TestRecord recB_tx2;

		auto fut2 = std::async(std::launch::async, [&]() {
			try {
				OpenWifi::DbTransaction tx2(pool.get(), logger);
				Poco::Data::Statement pidStmt(tx2.Session());
				uint64_t pid = 0;
				pidStmt << "SELECT pg_backend_pid()", Poco::Data::Keywords::into(pid), Poco::Data::Keywords::now;
				tx2Pid = pid;

				bool ok = pgDbInstance.GetRecordForUpdate(tx2, "id", std::string("rec-lock-002"), recB_tx2);
				if (ok) {
					tx2Acquired = true;
					return tx2.Rollback();
				}
			} catch (const std::exception &e) {
				std::cerr << "tx2 exception in Test B: " << e.what() << std::endl;
			} catch (...) {
				std::cerr << "tx2 unknown exception in Test B" << std::endl;
			}
			return false;
		});

		// Wait until tx2 session PID is established with a bounded timeout and premature failure check
		auto startWaitB = std::chrono::steady_clock::now();
		while (tx2Pid.load() == 0) {
			if (fut2.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
				TEST_ASSERT(false, "tx2 async worker terminated prematurely before publishing its backend PID!");
			}
			if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startWaitB).count() > 5) {
				TEST_ASSERT(false, "Timed out waiting for tx2 to publish its backend PID!");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		// Explicitly observe that tx2's PID is specifically blocked by tx1's PID in PostgreSQL before rolling back tx1
		bool lockContentionObserved = WaitForBlockerPid(pool, tx2Pid.load(), tx1Pid, 4000);
		TEST_ASSERT(lockContentionObserved,
		            "PostgreSQL did not observe tx2 being blocked specifically by tx1 via pg_blocking_pids()!");
		TEST_ASSERT(!tx2Acquired.load(), "tx2 unexpectedly acquired row while tx1 holds lock before rollback!");

		// tx1 rolls back without updating
		TEST_ASSERT(tx1.Rollback(), "tx1 Rollback failed");

		// tx2 must now unblock
		auto status = fut2.wait_for(std::chrono::seconds(5));
		TEST_ASSERT(status == std::future_status::ready, "Timed out waiting for tx2 after tx1 Rollback");
		bool tx2Success = fut2.get();
		TEST_ASSERT(tx2Success, "tx2 failed after lock released by rollback");
		TEST_ASSERT(tx2Acquired.load(), "tx2 failed to acquire row after tx1 rolled back");
		TEST_ASSERT(recB_tx2.value == "Initial Value B", "recB value unexpectedly altered after rollback");

		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test C: Not-found does not poison transaction
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test C: Not-found does not poison transaction... " << std::flush;
		OpenWifi::DbTransaction tx(pool.get(), logger);
		OpenWifi::TestRecord notFoundRec;
		bool found = pgDbInstance.GetRecordForUpdate(tx, "id", std::string("non-existent-id-99999"), notFoundRec);
		TEST_ASSERT(!found, "GetRecordForUpdate unexpectedly returned true for non-existent row");
		TEST_ASSERT(!tx.HasFailed(), "tx.HasFailed() was set after normal not-found GetRecordForUpdate!");
		TEST_ASSERT(tx.Commit(), "tx.Commit() failed after not-found GetRecordForUpdate!");
		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test D: SQL/DB failure poisons transaction
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test D: SQL/DB failure poisons transaction... " << std::flush;
		// Ensure non-existent table truly does not exist
		{
			Poco::Data::Session session = pool.get();
			Poco::Data::Statement dropStmt(session);
			dropStmt << "drop table if exists nonexistent_table_for_rowlock_test";
			dropStmt.execute();
		}

		OpenWifi::DbTransaction tx(pool.get(), logger);
		TestDB badDb(OpenWifi::DBType::pgsql, pool, logger, nullptr, "nonexistent_table_for_rowlock_test");
		OpenWifi::TestRecord badRec;
		bool found = badDb.GetRecordForUpdate(tx, "id", std::string("rec-xyz"), badRec);
		TEST_ASSERT(!found, "GetRecordForUpdate on bad DB unexpectedly returned true");
		TEST_ASSERT(tx.HasFailed(), "tx.HasFailed() was not set by GetRecordForUpdate catch block on DB exception!");
		TEST_ASSERT(!tx.Commit(), "tx.Commit() succeeded despite tx being marked failed from GetRecordForUpdate error!");
		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test F: Cache-bypass verification with mock DBCache
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test F: Cache-bypass verification with mock DBCache... " << std::flush;
		OpenWifi::MockTestDBCache mockCache;
		TestDB cachedDb(OpenWifi::DBType::pgsql, pool, logger, &mockCache, "rowlock_test_records");

		// 1. Put stale data in the cache and fresh data in PostgreSQL
		OpenWifi::TestRecord freshDbRec{"rec-cache-001", "Row Cache", "FRESH_DB_VALUE"};
		TEST_ASSERT(cachedDb.CreateRecord(freshDbRec), "Failed to create fresh record in DB");

		OpenWifi::TestRecord staleCacheRec{"rec-cache-001", "Row Cache", "STALE_CACHE_VALUE"};
		mockCache.Put(staleCacheRec);
		mockCache.ResetFlags();

		// 2. Call GetRecordForUpdate — MUST read from DB, NOT from cache, and NOT mutate cache
		OpenWifi::DbTransaction tx(pool.get(), logger);
		OpenWifi::TestRecord fetchedRec;
		bool ok = cachedDb.GetRecordForUpdate(tx, "id", std::string("rec-cache-001"), fetchedRec);

		TEST_ASSERT(ok, "GetRecordForUpdate failed on cachedDb");
		TEST_ASSERT(fetchedRec.value == "FRESH_DB_VALUE",
		            "GetRecordForUpdate returned stale cache value instead of fresh PostgreSQL DB value!");
		TEST_ASSERT(!mockCache.GetFromCacheCalled(),
		            "GetRecordForUpdate unexpectedly read from Cache_->GetFromCache()!");
		TEST_ASSERT(!mockCache.UpdateCacheCalled(),
		            "GetRecordForUpdate unexpectedly mutated cache via UpdateCache()!");
		TEST_ASSERT(!mockCache.CreateCalled(),
		            "GetRecordForUpdate unexpectedly mutated cache via Create()!");
		TEST_ASSERT(mockCache.Get("rec-cache-001").value == "STALE_CACHE_VALUE",
		            "Cache entry was unexpectedly altered by GetRecordForUpdate");

		TEST_ASSERT(tx.Commit(), "Failed to commit tx after GetRecordForUpdate");

		// 3. Negative check: row exists ONLY in cache, NOT in DB
		OpenWifi::TestRecord ghostRec{"ghost-in-cache", "Ghost", "GHOST_VALUE"};
		mockCache.Put(ghostRec);
		mockCache.ResetFlags();

		OpenWifi::DbTransaction txGhost(pool.get(), logger);
		OpenWifi::TestRecord ghostFetched;
		bool ghostOk = cachedDb.GetRecordForUpdate(txGhost, "id", std::string("ghost-in-cache"), ghostFetched);

		TEST_ASSERT(!ghostOk,
		            "GetRecordForUpdate returned true for a record that only exists in Cache_ and not in PostgreSQL!");
		TEST_ASSERT(!mockCache.GetFromCacheCalled(),
		            "GetRecordForUpdate read from Cache_ for ghost record!");
		TEST_ASSERT(txGhost.Commit(), "Failed to commit txGhost after not-found");

		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test G: Unique field lookup behavior
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test G: Unique field lookup behavior... " << std::flush;
		// G1: Successful lookup and lock acquisition by field 'id'
		{
			OpenWifi::TestRecord recG{"rec-lock-007", "Row G", "Value G"};
			TEST_ASSERT(pgDbInstance.CreateRecord(recG), "Failed to create recG");

			OpenWifi::DbTransaction tx(pool.get(), logger);
			OpenWifi::TestRecord outRec;
			bool result = pgDbInstance.GetRecordForUpdate(tx, "id", std::string("rec-lock-007"), outRec);
			TEST_ASSERT(result, "GetRecordForUpdate failed to find record by id");
			TEST_ASSERT(outRec.id == "rec-lock-007" && outRec.name == "Row G", "Fetched record data mismatch");
			TEST_ASSERT(tx.Commit(), "Failed to commit tx after ID lookup");
		}

		// G2: Empty ID lookup safely returns false without poisoning transaction
		{
			OpenWifi::DbTransaction txEmpty(pool.get(), logger);
			OpenWifi::TestRecord recEmpty;
			bool resultEmpty = pgDbInstance.GetRecordForUpdate(txEmpty, "id", std::string(""), recEmpty);
			TEST_ASSERT(!resultEmpty, "GetRecordForUpdate unexpectedly found empty ID");
			TEST_ASSERT(!txEmpty.HasFailed(), "txEmpty.HasFailed() was set after empty ID lookup!");
			TEST_ASSERT(txEmpty.Commit(), "txEmpty.Commit() failed after empty ID lookup!");
		}
		std::cout << "PASSED" << std::endl;
	}

	// Clean up PostgreSQL test table on normal completion after all transactions have ended
	DropTestTable(pool);

	std::cout << "[Framework Unit Test] All PostgreSQL Row-Level Locking Tests Passed Successfully!" << std::endl;
	return 0;
#endif
}
