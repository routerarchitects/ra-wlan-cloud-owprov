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

		void SetThrowOnDelete(bool shouldThrow) {
			throwOnDelete_ = shouldThrow;
		}

		void SetThrowOnInvalidateAll(bool shouldThrow) {
			throwOnInvalidateAll_ = shouldThrow;
		}

		bool InvalidateAllCalled() const {
			return invalidateAllCalled_;
		}

		void InvalidateAll() override {
			invalidateAllCalled_ = true;
			if (throwOnInvalidateAll_) {
				throw Poco::Exception("Mock InvalidateAll simulated failure");
			}
			cache_map_.clear();
		}

		void UpdateCache(const TestRecord &R) override {
			if (throwOnUpdateCache_) {
				throw Poco::Exception("Mock UpdateCache simulated failure");
			}
			cache_map_[R.id] = R;
		}

		void Delete(const std::string &FieldName, const std::string &Value) override {
			if (throwOnDelete_) {
				throw Poco::Exception("Mock Delete simulated failure");
			}
			if (FieldName == "id") {
				cache_map_.erase(Value);
			}
		}

		bool Contains(const std::string &id) const {
			return cache_map_.find(id) != cache_map_.end();
		}

		void Clear() {
			cache_map_.clear();
			throwOnUpdateCache_ = false;
			throwOnDelete_ = false;
			throwOnInvalidateAll_ = false;
			invalidateAllCalled_ = false;
		}

	  private:
		std::map<std::string, TestRecord> cache_map_;
		bool throwOnUpdateCache_ = false;
		bool throwOnDelete_ = false;
		bool throwOnInvalidateAll_ = false;
		bool invalidateAllCalled_ = false;
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

	// Clear leftover test records using direct SQL statement execution
	{
		Poco::Data::Session clearSession = pool.get();
		Poco::Data::Statement clearStmt(clearSession);
		clearStmt << "delete from test_records";
		clearStmt.execute();
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

		// 2. GetRecords(tx, ...) wrapper inside transaction
		TestDB::RecordVec insideTxRecords;
		TEST_ASSERT(db.GetRecords(tx, 0, 10, insideTxRecords) == true, "GetRecords failed inside transaction wrapper");
		TEST_ASSERT(insideTxRecords.size() == 2, "Expected 2 records inside transaction session");

		// 3. GetRecord(tx, ...) wrapper inside transaction
		OpenWifi::TestRecord insideTxRecA;
		TEST_ASSERT(db.GetRecord(tx, "id", "rec-101", insideTxRecA) == true, "GetRecord failed inside transaction wrapper");
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
	// Test 6: Mock DBCache Post-Commit Cache Invalidation Behavior
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test 6: Mock DBCache Post-Commit Cache Invalidation Behavior... " << std::flush;
		mockCache.Clear();

		// 6a: Transactional CreateRecord performs conservative post-commit invalidation when Cache_ exists
		{
			// Pre-populate cache with an entry
			OpenWifi::TestRecord recPre{"rec-pre", "Record Pre", "Val Pre"};
			{
				OpenWifi::DbTransaction txSetup(pool.get(), logger);
				TEST_ASSERT(db.CreateRecord(txSetup, recPre) == true, "Setup create recPre failed");
				TEST_ASSERT(txSetup.Commit() == true, "Setup commit recPre failed");
			}
			OpenWifi::TestRecord dummyPre;
			TEST_ASSERT(db.GetRecord("id", "rec-pre", dummyPre) == true, "GetRecord failed for recPre setup");
			TEST_ASSERT(mockCache.Contains("rec-pre") == true, "recPre setup missing from cache");

			OpenWifi::DbTransaction tx(pool.get(), logger);
			OpenWifi::TestRecord recH{"rec-108", "Record H", "Val H"};

			TEST_ASSERT(db.CreateRecord(tx, recH) == true, "Failed to create recH");
			// Cache MUST NOT be invalidated before commit
			TEST_ASSERT(mockCache.Contains("rec-pre") == true, "MockDBCache unexpectedly invalidated BEFORE commit!");

			TEST_ASSERT(tx.Commit() == true, "Commit failed in 6a");
			// Post-commit conservative invalidation MUST have cleared cache entries
			TEST_ASSERT(mockCache.InvalidateAllCalled() == true, "Post-commit InvalidateAll() was not called!");
			TEST_ASSERT(mockCache.Contains("rec-pre") == false, "Pre-existing cache entry was not cleared after commit!");

			// Subsequent GetRecord re-populates cache on miss
			OpenWifi::TestRecord fetched;
			TEST_ASSERT(db.GetRecord("id", "rec-108", fetched) == true, "GetRecord failed after commit");
			TEST_ASSERT(mockCache.Contains("rec-108") == true, "MockDBCache failed to populate on subsequent read miss");
		}

		// 6b: Transactional UpdateRecord INVALIDATES DBCache ONLY AFTER COMMIT
		{
			OpenWifi::DbTransaction tx(pool.get(), logger);
			OpenWifi::TestRecord recH_updated{"rec-108", "Record H", "Val H Updated Cache"};

			TEST_ASSERT(db.UpdateRecord(tx, "id", "rec-108", recH_updated) == true, "Failed to update recH");
			// Cache must hold initial value during transaction before commit
			OpenWifi::TestRecord cachedPre;
			TEST_ASSERT(mockCache.GetFromCache("id", "rec-108", cachedPre) == true, "recH missing from cache pre-commit");
			TEST_ASSERT(cachedPre.value == "Val H", "Cache mutated prematurely during transaction!");

			TEST_ASSERT(tx.Commit() == true, "Commit failed in 6b");
			// Cache MUST be invalidated post-commit, not directly updated
			TEST_ASSERT(mockCache.Contains("rec-108") == false, "Cache entry failed to invalidate post-commit!");

			// Subsequent GetRecord re-populates cache with fresh DB value
			OpenWifi::TestRecord cachedPost;
			TEST_ASSERT(db.GetRecord("id", "rec-108", cachedPost) == true, "GetRecord failed after update commit");
			TEST_ASSERT(cachedPost.value == "Val H Updated Cache", "GetRecord loaded incorrect value post-commit");
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

		// 6d: Explicit Rollback() does NOT mutate or invalidate DBCache
		{
			// Pre-populate cache
			OpenWifi::TestRecord recI{"rec-109", "Record I", "Val I"};
			{
				OpenWifi::DbTransaction txSetup(pool.get(), logger);
				TEST_ASSERT(db.CreateRecord(txSetup, recI) == true, "Setup create recI failed");
				TEST_ASSERT(txSetup.Commit() == true, "Setup commit recI failed");
			}
			OpenWifi::TestRecord dummy;
			TEST_ASSERT(db.GetRecord("id", "rec-109", dummy) == true, "GetRecord failed for recI setup");
			TEST_ASSERT(mockCache.Contains("rec-109") == true, "recI setup missing from cache");

			OpenWifi::DbTransaction tx(pool.get(), logger);
			OpenWifi::TestRecord recI_mod{"rec-109", "Record I", "Val I Mod"};
			TEST_ASSERT(db.UpdateRecord(tx, "id", "rec-109", recI_mod) == true, "Failed to update recI in transaction");
			TEST_ASSERT(tx.Rollback() == true, "Rollback failed in 6d");

			// Cache must still contain original recI entry unchanged
			OpenWifi::TestRecord cachedRolledBack;
			TEST_ASSERT(mockCache.GetFromCache("id", "rec-109", cachedRolledBack) == true, "Cache entry lost after rollback!");
			TEST_ASSERT(cachedRolledBack.value == "Val I", "Cache entry modified after rolled back transaction!");
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
	// Test 7: Targeted Cache Invalidation Fallback When Delete Fails
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test 7: Targeted Cache Invalidation Fallback on Delete Failure... " << std::flush;
		mockCache.Clear();

		// Pre-populate database & cache
		{
			OpenWifi::DbTransaction tx(pool.get(), logger);
			OpenWifi::TestRecord recK{"rec-111", "Record K", "Val K Initial"};
			OpenWifi::TestRecord recL{"rec-112", "Record L", "Val L Unrelated"};
			TEST_ASSERT(db.CreateRecord(tx, recK) == true, "Failed to create recK");
			TEST_ASSERT(db.CreateRecord(tx, recL) == true, "Failed to create recL");
			TEST_ASSERT(tx.Commit() == true, "Failed to commit initial records for Test 7");
		}

		// Populate cache via read
		OpenWifi::TestRecord dummyK, dummyL;
		TEST_ASSERT(db.GetRecord("id", "rec-111", dummyK) == true, "GetRecord recK failed");
		TEST_ASSERT(db.GetRecord("id", "rec-112", dummyL) == true, "GetRecord recL failed");
		TEST_ASSERT(mockCache.Contains("rec-111") == true, "recK missing from cache");
		TEST_ASSERT(mockCache.Contains("rec-112") == true, "recL missing from cache");

		// Perform update inside transaction with mockCache set to throw on Delete
		{
			OpenWifi::DbTransaction tx(pool.get(), logger);
			OpenWifi::TestRecord recK_updated{"rec-111", "Record K", "Val K Updated"};
			TEST_ASSERT(db.UpdateRecord(tx, "id", "rec-111", recK_updated) == true, "UpdateRecord failed inside tx");

			mockCache.SetThrowOnDelete(true);
			TEST_ASSERT(tx.Commit() == true, "Commit() must succeed even if post-commit Delete throws");
		}

		// Verify 1: DB update remains committed
		{
			Poco::Data::Session verifySession = pool.get();
			OpenWifi::TestRecord checkK;
			TEST_ASSERT(db.GetRecord(verifySession, "id", "rec-111", checkK) == true, "recK missing from DB");
			TEST_ASSERT(checkK.value == "Val K Updated", "recK value in DB did not update");
		}

		// Verify 2: InvalidateAll() was called after targeted Delete failure
		TEST_ASSERT(mockCache.InvalidateAllCalled() == true, "InvalidateAll() was not called after Delete failure!");

		// Verify 3: Stale cache entry rec-111 is gone
		TEST_ASSERT(mockCache.Contains("rec-111") == false, "Stale cache entry rec-111 was not invalidated!");

		// Verify 4: Subsequent GetRecord re-populates cache with fresh DB value
		mockCache.SetThrowOnDelete(false);
		OpenWifi::TestRecord reFetchedRec;
		TEST_ASSERT(db.GetRecord("id", "rec-111", reFetchedRec) == true, "GetRecord failed after cache invalidation");
		TEST_ASSERT(reFetchedRec.value == "Val K Updated", "GetRecord returned incorrect value after re-populating cache");
		TEST_ASSERT(mockCache.Contains("rec-111") == true, "Cache was not re-populated after GetRecord miss");

		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test 8: Transactional Delete Post-Commit Delete Failure Fallback to InvalidateAll()
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test 8: Transactional Delete Post-Commit Delete Failure Fallback to InvalidateAll()... " << std::flush;
		mockCache.Clear();

		// Pre-populate DB & Cache with 2 records
		{
			OpenWifi::DbTransaction tx(pool.get(), logger);
			OpenWifi::TestRecord recM{"rec-113", "Record M", "Val M"};
			OpenWifi::TestRecord recN{"rec-114", "Record N", "Val N"};
			TEST_ASSERT(db.CreateRecord(tx, recM) == true, "Failed to create recM");
			TEST_ASSERT(db.CreateRecord(tx, recN) == true, "Failed to create recN");
			TEST_ASSERT(tx.Commit() == true, "Failed to commit initial records for Test 8");
		}

		OpenWifi::TestRecord dummyM, dummyN;
		TEST_ASSERT(db.GetRecord("id", "rec-113", dummyM) == true, "GetRecord recM failed");
		TEST_ASSERT(db.GetRecord("id", "rec-114", dummyN) == true, "GetRecord recN failed");
		TEST_ASSERT(mockCache.Contains("rec-113") == true, "recM missing from cache");
		TEST_ASSERT(mockCache.Contains("rec-114") == true, "recN missing from cache");

		// Perform transactional DeleteRecord with mockCache set to throw on Delete
		{
			OpenWifi::DbTransaction tx(pool.get(), logger);
			TEST_ASSERT(db.DeleteRecord(tx, "id", "rec-113") == true, "DeleteRecord failed inside tx");

			mockCache.SetThrowOnDelete(true);
			TEST_ASSERT(tx.Commit() == true, "Commit() must succeed even if post-commit Delete throws");
		}

		// Verify 1: DB deletion remains committed and unrelated DB records persist
		{
			Poco::Data::Session verifySession = pool.get();
			OpenWifi::TestRecord checkM, checkN;
			TEST_ASSERT(db.GetRecord(verifySession, "id", "rec-113", checkM) == false, "recM unexpectedly exists in DB after deletion");
			TEST_ASSERT(db.GetRecord(verifySession, "id", "rec-114", checkN) == true, "recN unexpectedly missing from DB after cache invalidation fallback");
		}

		// Verify 2: InvalidateAll() was called as last-resort fallback
		TEST_ASSERT(mockCache.InvalidateAllCalled() == true, "InvalidateAll() was not called after Delete failure!");

		// Verify 3: Stale target cache entry rec-113 is gone
		TEST_ASSERT(mockCache.Contains("rec-113") == false, "Stale deleted entry rec-113 was served from cache!");

		// Verify 4: Unrelated cache entry rec-114 is also cleared because full invalidation occurred
		TEST_ASSERT(mockCache.Contains("rec-114") == false, "Unrelated entry rec-114 was not cleared during InvalidateAll()!");

		// Verify 5: Subsequent GetRecord for deleted recM returns false (does not return stale cache data)
		mockCache.SetThrowOnDelete(false);
		OpenWifi::TestRecord reFetchedRecM;
		TEST_ASSERT(db.GetRecord("id", "rec-113", reFetchedRecM) == false, "GetRecord returned stale data for deleted row");

		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test 9: Transactional Delete Missing Row Failure Causes Caller Rollback
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test 9: Transactional Delete Missing Row Failure... " << std::flush;
		{
			OpenWifi::DbTransaction tx(pool.get(), logger);

			OpenWifi::TestRecord recO{"rec-115", "Record O", "Val O"};
			TEST_ASSERT(db.CreateRecord(tx, recO) == true, "CreateRecord failed in Test 9");

			// Attempt deleting a non-existent row inside the transaction
			TEST_ASSERT(db.DeleteRecord(tx, "id", "does-not-exist") == false, "DeleteRecord unexpectedly succeeded for missing row");

			// Transaction failure flag is set
			TEST_ASSERT(tx.HasFailed() == true, "tx.HasFailed() was not set after failed operation!");

			// Caller rolls back due to failure of required delete step
			TEST_ASSERT(tx.Rollback() == true, "Rollback failed after DeleteRecord failure");
		}

		// Verify earlier write (recO) was rolled back from DB as well
		{
			Poco::Data::Session verifySession = pool.get();
			OpenWifi::TestRecord checkO;
			TEST_ASSERT(db.GetRecord(verifySession, "id", "rec-115", checkO) == false, "Earlier write remained in DB after rollback");
		}
		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test 10: Post-Commit Double Cache Failure (Delete & InvalidateAll Throw)
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test 10: Post-Commit Double Cache Failure (Delete & InvalidateAll Throw)... " << std::flush;
		mockCache.Clear();

		// Pre-populate DB & Cache with 1 record
		{
			OpenWifi::DbTransaction tx(pool.get(), logger);
			OpenWifi::TestRecord recP{"rec-116", "Record P", "Val P"};
			TEST_ASSERT(db.CreateRecord(tx, recP) == true, "Failed to create recP");
			TEST_ASSERT(tx.Commit() == true, "Failed to commit initial record for Test 10");
		}

		OpenWifi::TestRecord dummyP;
		TEST_ASSERT(db.GetRecord("id", "rec-116", dummyP) == true, "GetRecord recP failed");
		TEST_ASSERT(mockCache.Contains("rec-116") == true, "recP missing from cache");

		// Perform transactional DeleteRecord with both Delete and InvalidateAll set to throw
		{
			OpenWifi::DbTransaction tx(pool.get(), logger);
			TEST_ASSERT(db.DeleteRecord(tx, "id", "rec-116") == true, "DeleteRecord failed inside tx");

			mockCache.SetThrowOnDelete(true);
			mockCache.SetThrowOnInvalidateAll(true);

			// DB commit MUST still return true even when both post-commit cache operations throw
			TEST_ASSERT(tx.Commit() == true, "DB commit must remain successful when cache recovery fails");
		}

		// Verify 1: DB deletion remains committed
		{
			Poco::Data::Session verifySession = pool.get();
			OpenWifi::TestRecord checkP;
			TEST_ASSERT(db.GetRecord(verifySession, "id", "rec-116", checkP) == false, "recP unexpectedly exists in DB after deletion");
		}

		// Verify 2: InvalidateAll() was attempted
		TEST_ASSERT(mockCache.InvalidateAllCalled() == true, "InvalidateAll() was not attempted after Delete failure!");

		// Verify 3: Stale cache entry remains in cache when both Delete and InvalidateAll throw (best-effort behavior)
		TEST_ASSERT(mockCache.Contains("rec-116") == true, "Expected stale cache entry to remain when both Delete and InvalidateAll throw");

		mockCache.Clear();
		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test 11: DB Session Released Before AfterCommit Callback Execution
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test 11: DB Session Released Before AfterCommit Callback... " << std::flush;
		const auto usedBefore = pool.used();
		int usedInsideCallback = -1;

		{
			OpenWifi::DbTransaction tx(pool.get(), logger);

			TEST_ASSERT(pool.used() == usedBefore + 1, "Transaction did not acquire one pooled session");

			tx.AfterCommit([&]() {
				usedInsideCallback = pool.used();
			});

			TEST_ASSERT(tx.Commit() == true, "Commit failed in Test 11");
		}

		TEST_ASSERT(usedInsideCallback == usedBefore, "DB session was still checked out during AfterCommit callback");
		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test 12: Unsafe Bulk Delete Overload Invalidation & Rollback Requirements
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test 12: Transactional Bulk DeleteRecords Invalidation & Rollback... " << std::flush;
		bool invalidationCallbackRan = false;

		{
			OpenWifi::DbTransaction tx(pool.get(), logger);
			OpenWifi::TestRecord recQ{"rec-117", "Record Q", "Val Q"};
			TEST_ASSERT(db.CreateRecord(tx, recQ) == true, "CreateRecord failed in Test 12");
			TEST_ASSERT(tx.Commit() == true, "Commit failed in Test 12 setup");
		}

		// Pre-populate cache for rec-117
		OpenWifi::TestRecord dummyQ;
		TEST_ASSERT(db.GetRecord("id", "rec-117", dummyQ) == true, "GetRecord recQ failed");
		TEST_ASSERT(mockCache.Contains("rec-117") == true, "recQ missing from cache pre-delete");

		// 1. Bulk delete without post-commit invalidation callback fails gracefully and marks tx as failed
		{
			OpenWifi::DbTransaction tx(pool.get(), logger);
			TEST_ASSERT(db.DeleteRecords(tx, "id='rec-117'", nullptr) == false, "DeleteRecords unexpectedly succeeded without invalidation callback");
			TEST_ASSERT(tx.HasFailed() == true, "tx.HasFailed() was not set after missing callback!");
			TEST_ASSERT(tx.Commit() == false, "tx.Commit() unexpectedly succeeded on failed transaction!");
		}

		// 2. Bulk delete with explicit post-commit invalidation callback succeeds and clears cache
		{
			OpenWifi::DbTransaction tx(pool.get(), logger);
			TEST_ASSERT(db.DeleteRecords(tx, "id='rec-117'", [&]() {
				invalidationCallbackRan = true;
				mockCache.Clear();
			}) == true, "DeleteRecords failed with explicit invalidation callback");
			TEST_ASSERT(invalidationCallbackRan == false, "Invalidation callback ran BEFORE commit!");
			TEST_ASSERT(tx.Commit() == true, "Commit failed for bulk delete");
			TEST_ASSERT(invalidationCallbackRan == true, "Invalidation callback failed to run AFTER commit!");
			TEST_ASSERT(mockCache.Contains("rec-117") == false, "Cache entry rec-117 still in cache after bulk delete commit!");
		}

		// Verify record Q is gone from DB
		{
			Poco::Data::Session verifySession = pool.get();
			OpenWifi::TestRecord checkQ;
			TEST_ASSERT(db.GetRecord(verifySession, "id", "rec-117", checkQ) == false, "recQ still exists in DB after bulk delete");
		}

		// 3. Rollback case: callback must NOT run and cache must NOT be invalidated
		{
			OpenWifi::DbTransaction txSetup(pool.get(), logger);
			OpenWifi::TestRecord recR{"rec-118", "Record R", "Val R"};
			TEST_ASSERT(db.CreateRecord(txSetup, recR) == true, "CreateRecord recR failed");
			TEST_ASSERT(txSetup.Commit() == true, "Commit recR failed");
		}
		OpenWifi::TestRecord dummyR;
		TEST_ASSERT(db.GetRecord("id", "rec-118", dummyR) == true, "GetRecord recR failed");
		TEST_ASSERT(mockCache.Contains("rec-118") == true, "recR missing from cache");

		{
			bool rollbackCallbackRan = false;
			OpenWifi::DbTransaction tx(pool.get(), logger);
			TEST_ASSERT(db.DeleteRecords(tx, "id='rec-118'", [&]() {
				rollbackCallbackRan = true;
				mockCache.Clear();
			}) == true, "DeleteRecords failed");

			TEST_ASSERT(tx.Rollback() == true, "Rollback failed");
			TEST_ASSERT(rollbackCallbackRan == false, "Callback unexpectedly ran after Rollback()!");
			TEST_ASSERT(mockCache.Contains("rec-118") == true, "Cache entry rec-118 was unexpectedly invalidated after Rollback()!");
		}

		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test 13: Transaction Failure Flag Enforces Rollback on Commit Attempt
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test 13: Transaction Failure Flag Enforces Rollback on Commit Attempt... " << std::flush;
		{
			OpenWifi::DbTransaction tx(pool.get(), logger);
			OpenWifi::TestRecord recS{"rec-119", "Record S", "Val S"};
			TEST_ASSERT(db.CreateRecord(tx, recS) == true, "CreateRecord recS failed");

			// Trigger a failure inside transaction
			TEST_ASSERT(db.DeleteRecord(tx, "id", "non-existent-id") == false, "DeleteRecord unexpectedly succeeded");
			TEST_ASSERT(tx.HasFailed() == true, "tx.HasFailed() was not set after failed operation");

			// Caller attempts to commit despite operation failure
			TEST_ASSERT(tx.Commit() == false, "tx.Commit() unexpectedly succeeded on failed transaction!");
		}

		// Verify record S was rolled back from DB because tx.Commit() refused to commit
		{
			Poco::Data::Session verifySession = pool.get();
			OpenWifi::TestRecord checkS;
			TEST_ASSERT(db.GetRecord(verifySession, "id", "rec-119", checkS) == false, "recS exists in DB after failed transaction commit attempt");
		}
		std::cout << "PASSED" << std::endl;
	}

	std::cout << "[Framework Unit Test] All DbTransaction & Transaction-Aware ORM Tests Passed Successfully!" << std::endl;
	return 0;
}

