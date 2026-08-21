/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 */

#include <iostream>
#include <cstdlib>
#include <map>

#include "Poco/Data/SessionPool.h"
#include "Poco/Data/SQLite/Connector.h"
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

	// Minimal record struct for testing session-aware ORM operations
	struct TestRecord {
		std::string id;
		std::string name;
		std::string value;

		void to_json(Poco::JSON::Object &) const {}
		bool from_json(const Poco::JSON::Object::Ptr &) { return true; }
	};

	typedef Poco::Tuple<std::string, std::string, std::string> TestRecordTuple;

	// Mock DBCache for testing post-commit cache synchronization
	class MockTestDBCache : public ORM::DBCache<TestRecord> {
	  public:
		MockTestDBCache() : DBCache<TestRecord>(100, 600) {}

		void Create(const TestRecord &R) override {
			cache_map_[R.id] = R;
		}

		bool GetFromCache(const std::string &FieldName, const std::string &Value, TestRecord &R) override {
			if (FieldName == "id") {
				auto it = cache_map_.find(Value);
				if (it != cache_map_.end()) {
					R = it->second;
					return true;
				}
			}
			return false;
		}

		void SetThrowOnUpdateCache(bool shouldThrow) {
			throwOnUpdateCache_ = shouldThrow;
		}

		void UpdateCache(const TestRecord &R) override {
			if (throwOnUpdateCache_) {
				throw Poco::Exception("Mock UpdateCache simulated failure");
			}
			cache_map_[R.id] = R;
		}

		void Delete(const std::string &FieldName, const std::string &Value) override {
			if (FieldName == "id") {
				cache_map_.erase(Value);
			}
		}

		bool Contains(const std::string &id) const {
			return cache_map_.find(id) != cache_map_.end();
		}

		void Clear() {
			cache_map_.clear();
		}

	  private:
		std::map<std::string, TestRecord> cache_map_;
		bool throwOnUpdateCache_ = false;
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
	TestDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L, ORM::DBCache<OpenWifi::TestRecord> *Cache = nullptr)
		: DB(T, "test_records",
			 ORM::FieldVec{
				 ORM::Field{"id", ORM::FieldType::FT_TEXT, 0, true},
				 ORM::Field{"name", ORM::FieldType::FT_TEXT},
				 ORM::Field{"value", ORM::FieldType::FT_TEXT}
			 },
			 ORM::IndexVec{}, P, L, "tst", Cache) {}
};

int main() {
	std::cout << "[Framework Unit Test] Initializing DbTransaction & Transaction-Aware ORM Tests..." << std::endl;

	Poco::Data::SQLite::Connector::registerConnector();
	Poco::Data::SessionPool pool("SQLite", "db_transaction_unittest.db");
	Poco::Logger &logger = Poco::Logger::get("DbTransactionTest");

	OpenWifi::MockTestDBCache mockCache;
	TestDB db(OpenWifi::DBType::sqlite, pool, logger, &mockCache);
	TEST_ASSERT(db.Create(), "Failed to create transaction test table");

	// Clear leftover test records using low-level DeleteRecords on session
	{
		Poco::Data::Session clearSession = pool.get();
		TEST_ASSERT(db.DeleteRecords(clearSession, "1=1"), "Failed to clear transaction test table");
		mockCache.Clear();
	}

	// -------------------------------------------------------------------------
	// Test 1: Commit Persists Multi-Operation Transactional Operations
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test 1: Commit Persists Multi-Operation Transactional Operations... " << std::flush;
		OpenWifi::DbTransaction tx(pool.get(), logger);

		OpenWifi::TestRecord recA{"rec-101", "Record A", "Val A Initial"};
		OpenWifi::TestRecord recB{"rec-102", "Record B", "Val B"};

		// 1. CreateRecord(tx, ...)
		TEST_ASSERT(db.CreateRecord(tx, recA) == true, "Failed to create recA in transaction");
		TEST_ASSERT(db.CreateRecord(tx, recB) == true, "Failed to create recB in transaction");

		// 2. GetRecords(session, ...) inside transaction
		TestDB::RecordVec insideTxRecords;
		TEST_ASSERT(db.GetRecords(tx.Session(), 0, 10, insideTxRecords) == true, "GetRecords failed inside transaction");
		TEST_ASSERT(insideTxRecords.size() == 2, "Expected 2 records inside transaction session");

		// 3. GetRecord(session, ...) inside transaction
		OpenWifi::TestRecord insideTxRecA;
		TEST_ASSERT(db.GetRecord(tx.Session(), "id", "rec-101", insideTxRecA) == true, "GetRecord failed inside transaction");
		TEST_ASSERT(insideTxRecA.value == "Val A Initial", "recA value mismatch inside transaction");

		// 4. UpdateRecord(tx, ...) inside transaction
		insideTxRecA.value = "Val A Updated";
		TEST_ASSERT(db.UpdateRecord(tx, "id", "rec-101", insideTxRecA) == true, "UpdateRecord failed inside transaction");

		// 5. DeleteRecord(tx, ...) inside transaction for recB
		TEST_ASSERT(db.DeleteRecord(tx, "id", "rec-102") == true, "DeleteRecord failed inside transaction for recB");

		TEST_ASSERT(tx.Commit() == true, "Failed to commit transaction");

		// Verify directly against database via session (bypassing Cache_)
		{
			Poco::Data::Session verifySession = pool.get();
			OpenWifi::TestRecord checkA, checkB;
			TEST_ASSERT(db.GetRecord(verifySession, "id", "rec-101", checkA) == true, "recA missing from database after commit");
			TEST_ASSERT(checkA.value == "Val A Updated", "recA updated value did not persist to database");
			TEST_ASSERT(db.GetRecord(verifySession, "id", "rec-102", checkB) == false, "recB unexpectedly exists in database after DeleteRecord commit");
		}
		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test 2: Multi-Operation Explicit Rollback Discards Uncommitted Writes
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test 2: Multi-Operation Explicit Rollback... " << std::flush;
		OpenWifi::DbTransaction tx(pool.get(), logger);

		OpenWifi::TestRecord recD{"rec-104", "Record D", "Val D"};
		OpenWifi::TestRecord recE{"rec-105", "Record E", "Val E"};

		TEST_ASSERT(db.CreateRecord(tx, recD) == true, "Failed to create recD in transaction");
		TEST_ASSERT(db.CreateRecord(tx, recE) == true, "Failed to create recE in transaction");

		TEST_ASSERT(tx.Rollback() == true, "Failed to rollback transaction");

		// Verify neither record exists in database after explicit rollback
		{
			Poco::Data::Session verifySession = pool.get();
			OpenWifi::TestRecord checkD, checkE;
			TEST_ASSERT(db.GetRecord(verifySession, "id", "rec-104", checkD) == false, "recD exists in database after explicit rollback");
			TEST_ASSERT(db.GetRecord(verifySession, "id", "rec-105", checkE) == false, "recE exists in database after explicit rollback");
		}
		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test 3: RAII Scope Exit Auto-Rollback (Destructor)
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test 3: RAII Scope Exit Auto-Rollback... " << std::flush;
		{
			OpenWifi::DbTransaction tx(pool.get(), logger);
			OpenWifi::TestRecord recF{"rec-106", "Record F", "Val F"};
			TEST_ASSERT(db.CreateRecord(tx, recF) == true, "Failed to create recF in transaction");
			// Intentionally exit scope without Commit() or Rollback()
		}

		// Verify record F was rolled back from database on destructor scope exit
		{
			Poco::Data::Session verifySession = pool.get();
			OpenWifi::TestRecord checkF;
			TEST_ASSERT(db.GetRecord(verifySession, "id", "rec-106", checkF) == false, "recF exists in database after scope exit auto-rollback");
		}
		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test 4: Multi-Operation Failure Atomicity
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test 4: Multi-Operation Failure Atomicity... " << std::flush;
		{
			OpenWifi::DbTransaction tx(pool.get(), logger);
			OpenWifi::TestRecord recG{"rec-107", "Record G", "Val G"};
			TEST_ASSERT(db.CreateRecord(tx, recG) == true, "Failed to create recG in transaction");

			// Attempt duplicate primary key insertion on same tx session
			OpenWifi::TestRecord recG_dup{"rec-107", "Record G Dup", "Val G Dup"};
			bool secondWriteResult = db.CreateRecord(tx, recG_dup);
			TEST_ASSERT(secondWriteResult == false, "Duplicate primary key write unexpectedly succeeded");

			// Exit scope without commit due to failed second operation
		}

		// Verify recG was completely rolled back from database due to second write failure
		{
			Poco::Data::Session verifySession = pool.get();
			OpenWifi::TestRecord checkG;
			TEST_ASSERT(db.GetRecord(verifySession, "id", "rec-107", checkG) == false, "recG exists in database after second write failure atomicity rollback");
		}
		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test 5: Inactive Transaction Safety
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test 5: Inactive Transaction Safety... " << std::flush;
		OpenWifi::DbTransaction tx(pool.get(), logger);
		TEST_ASSERT(tx.Commit() == true, "Failed initial commit");

		// Subsequent operations on committed/inactive transaction must fail gracefully
		TEST_ASSERT(tx.Commit() == false, "Second commit unexpectedly succeeded on inactive transaction");
		TEST_ASSERT(tx.Rollback() == false, "Rollback unexpectedly succeeded on inactive transaction");

		bool threwException = false;
		try {
			tx.Session();
		} catch (const Poco::IllegalStateException &) {
			threwException = true;
		}
		TEST_ASSERT(threwException == true, "Session() failed to throw IllegalStateException on inactive transaction");
		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test 6: Mock DBCache Post-Commit Synchronization Guarantees
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test 6: Mock DBCache Post-Commit Synchronization Guarantees... " << std::flush;
		mockCache.Clear();

		// 6a: Transactional CreateRecord updates DBCache ONLY AFTER COMMIT
		{
			OpenWifi::DbTransaction tx(pool.get(), logger);
			OpenWifi::TestRecord recH{"rec-108", "Record H", "Val H"};

			TEST_ASSERT(db.CreateRecord(tx, recH) == true, "Failed to create recH");
			// Cache MUST NOT be updated during open transaction before commit
			TEST_ASSERT(mockCache.Contains("rec-108") == false, "MockDBCache unexpectedly updated BEFORE commit!");

			TEST_ASSERT(tx.Commit() == true, "Commit failed in 6a");
			// Cache MUST be updated AFTER successful commit
			TEST_ASSERT(mockCache.Contains("rec-108") == true, "MockDBCache failed to update AFTER commit!");
		}

		// 6b: Transactional UpdateRecord updates DBCache ONLY AFTER COMMIT
		{
			OpenWifi::DbTransaction tx(pool.get(), logger);
			OpenWifi::TestRecord recH_updated{"rec-108", "Record H", "Val H Updated Cache"};

			TEST_ASSERT(db.UpdateRecord(tx, "id", "rec-108", recH_updated) == true, "Failed to update recH");
			// Cache must hold initial value during transaction before commit
			OpenWifi::TestRecord cachedPre;
			TEST_ASSERT(mockCache.GetFromCache("id", "rec-108", cachedPre) == true, "recH missing from cache");
			TEST_ASSERT(cachedPre.value == "Val H", "Cache mutated prematurely during transaction!");

			TEST_ASSERT(tx.Commit() == true, "Commit failed in 6b");
			// Cache updated post-commit
			OpenWifi::TestRecord cachedPost;
			TEST_ASSERT(mockCache.GetFromCache("id", "rec-108", cachedPost) == true, "recH missing from cache post-commit");
			TEST_ASSERT(cachedPost.value == "Val H Updated Cache", "Cache failed to update post-commit!");
		}

		// 6c: Transactional DeleteRecord deletes from DBCache ONLY AFTER COMMIT
		{
			OpenWifi::DbTransaction tx(pool.get(), logger);

			TEST_ASSERT(db.DeleteRecord(tx, "id", "rec-108") == true, "Failed to delete recH");
			// Cache entry must still exist during transaction before commit
			TEST_ASSERT(mockCache.Contains("rec-108") == true, "Cache entry deleted prematurely before commit!");

			TEST_ASSERT(tx.Commit() == true, "Commit failed in 6c");
			// Cache entry deleted post-commit
			TEST_ASSERT(mockCache.Contains("rec-108") == false, "Cache entry failed to delete post-commit!");
		}

		// 6d: Explicit Rollback() does NOT mutate DBCache
		{
			OpenWifi::DbTransaction tx(pool.get(), logger);
			OpenWifi::TestRecord recI{"rec-109", "Record I", "Val I"};

			TEST_ASSERT(db.CreateRecord(tx, recI) == true, "Failed to create recI in transaction");
			TEST_ASSERT(tx.Rollback() == true, "Rollback failed in 6d");

			// Cache must NOT contain recI
			TEST_ASSERT(mockCache.Contains("rec-109") == false, "Cache mutated after rolled back transaction!");
		}

		// 6e: Destructor scope-exit auto-rollback does NOT mutate DBCache
		{
			{
				OpenWifi::DbTransaction tx(pool.get(), logger);
				OpenWifi::TestRecord recJ{"rec-110", "Record J", "Val J"};
				TEST_ASSERT(db.CreateRecord(tx, recJ) == true, "Failed to create recJ in transaction");
				// Intentionally exit scope without Commit() or Rollback()
			}

			// Cache must NOT contain recJ
			TEST_ASSERT(mockCache.Contains("rec-110") == false, "Cache mutated after scope-exit auto-rollback!");
		}

		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test 7: Targeted Cache Invalidation Fallback When UpdateCache() Fails
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test 7: Targeted Cache Invalidation Fallback on UpdateCache() Failure... " << std::flush;
		mockCache.Clear();
		mockCache.SetThrowOnUpdateCache(false);

		// Pre-populate database & cache
		{
			OpenWifi::DbTransaction tx(pool.get(), logger);
			OpenWifi::TestRecord recK{"rec-111", "Record K", "Val K Initial"};
			OpenWifi::TestRecord recL{"rec-112", "Record L", "Val L Unrelated"};
			TEST_ASSERT(db.CreateRecord(tx, recK) == true, "Failed to create recK");
			TEST_ASSERT(db.CreateRecord(tx, recL) == true, "Failed to create recL");
			TEST_ASSERT(tx.Commit() == true, "Failed to commit initial records for Test 7");
		}

		TEST_ASSERT(mockCache.Contains("rec-111") == true, "recK missing from cache");
		TEST_ASSERT(mockCache.Contains("rec-112") == true, "recL missing from cache");

		// Perform update inside transaction with mockCache set to throw on UpdateCache
		{
			OpenWifi::DbTransaction tx(pool.get(), logger);
			OpenWifi::TestRecord recK_updated{"rec-111", "Record K", "Val K Updated"};
			TEST_ASSERT(db.UpdateRecord(tx, "id", "rec-111", recK_updated) == true, "UpdateRecord failed inside tx");

			mockCache.SetThrowOnUpdateCache(true);
			TEST_ASSERT(tx.Commit() == true, "Commit() must succeed even if post-commit UpdateCache throws");
		}

		// Verify 1: DB update remains committed
		{
			Poco::Data::Session verifySession = pool.get();
			OpenWifi::TestRecord checkK;
			TEST_ASSERT(db.GetRecord(verifySession, "id", "rec-111", checkK) == true, "recK missing from DB");
			TEST_ASSERT(checkK.value == "Val K Updated", "recK value in DB did not update");
		}

		// Verify 2: Target cache entry was invalidated due to UpdateCache failure
		TEST_ASSERT(mockCache.Contains("rec-111") == false, "Stale cache entry rec-111 was not invalidated after UpdateCache failure!");

		// Verify 3: Unrelated cache entry remains intact
		TEST_ASSERT(mockCache.Contains("rec-112") == true, "Unrelated cache entry rec-112 was unexpectedly removed!");

		// Verify 4: Subsequent GetRecord re-populates cache with fresh DB value
		mockCache.SetThrowOnUpdateCache(false);
		OpenWifi::TestRecord reFetchedRec;
		TEST_ASSERT(db.GetRecord("id", "rec-111", reFetchedRec) == true, "GetRecord failed after cache invalidation");
		TEST_ASSERT(reFetchedRec.value == "Val K Updated", "GetRecord returned incorrect value after re-populating cache");
		TEST_ASSERT(mockCache.Contains("rec-111") == true, "Cache was not re-populated after GetRecord miss");

		OpenWifi::TestRecord refreshedCachedRec;
		TEST_ASSERT(mockCache.GetFromCache("id", "rec-111", refreshedCachedRec) == true, "rec-111 missing from cache after re-population");
		TEST_ASSERT(refreshedCachedRec.value == "Val K Updated", "Cache was re-populated with stale value");

		std::cout << "PASSED" << std::endl;
	}

	std::cout << "[Framework Unit Test] All DbTransaction & Transaction-Aware ORM Tests Passed Successfully!" << std::endl;
	return 0;
}

