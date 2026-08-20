/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 */

#include <iostream>
#include <cstdlib>

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
	TestDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L)
		: DB(T, "test_records",
			 ORM::FieldVec{
				 ORM::Field{"id", ORM::FieldType::FT_TEXT, 0, true},
				 ORM::Field{"name", ORM::FieldType::FT_TEXT},
				 ORM::Field{"value", ORM::FieldType::FT_TEXT}
			 },
			 ORM::IndexVec{}, P, L, "tst") {}
};

int main() {
	std::cout << "[Framework Unit Test] Initializing DbTransaction & Session-Aware ORM Tests..." << std::endl;

	Poco::Data::SQLite::Connector::registerConnector();
	Poco::Data::SessionPool pool("SQLite", "db_transaction_unittest.db");
	Poco::Logger &logger = Poco::Logger::get("DbTransactionTest");

	TestDB db(OpenWifi::DBType::sqlite, pool, logger);
	TEST_ASSERT(db.Create(), "Failed to create transaction test table");

	// Clear leftover test records using valid non-empty clause
	TEST_ASSERT(db.DeleteRecords("1=1"), "Failed to clear transaction test table");

	// -------------------------------------------------------------------------
	// Test 1: Commit Persists Multi-Operation Session-Aware Operations (All 6 Overloads)
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test 1: Commit Persists Multi-Operation Session-Aware Operations... " << std::flush;
		OpenWifi::DbTransaction tx(pool.get(), logger);

		OpenWifi::TestRecord recA{"rec-101", "Record A", "Val A Initial"};
		OpenWifi::TestRecord recB{"rec-102", "Record B", "Val B"};
		OpenWifi::TestRecord recC{"rec-103", "Record C", "Val C"};

		// 1. CreateRecord(session, ...)
		TEST_ASSERT(db.CreateRecord(tx.Session(), recA) == true, "Failed to create recA in transaction");
		TEST_ASSERT(db.CreateRecord(tx.Session(), recB) == true, "Failed to create recB in transaction");
		TEST_ASSERT(db.CreateRecord(tx.Session(), recC) == true, "Failed to create recC in transaction");

		// 2. GetRecords(session, ...) inside transaction
		TestDB::RecordVec insideTxRecords;
		TEST_ASSERT(db.GetRecords(tx.Session(), 0, 10, insideTxRecords) == true, "GetRecords failed inside transaction");
		TEST_ASSERT(insideTxRecords.size() == 3, "Expected 3 records inside transaction session");

		// 3. GetRecord(session, ...) inside transaction
		OpenWifi::TestRecord insideTxRecA;
		TEST_ASSERT(db.GetRecord(tx.Session(), "id", "rec-101", insideTxRecA) == true, "GetRecord failed inside transaction");
		TEST_ASSERT(insideTxRecA.value == "Val A Initial", "recA value mismatch inside transaction");

		// 4. UpdateRecord(session, ...) inside transaction
		insideTxRecA.value = "Val A Updated";
		TEST_ASSERT(db.UpdateRecord(tx.Session(), "id", "rec-101", insideTxRecA) == true, "UpdateRecord failed inside transaction");

		// 5. DeleteRecord(session, ...) inside transaction
		TEST_ASSERT(db.DeleteRecord(tx.Session(), "id", "rec-102") == true, "DeleteRecord failed inside transaction");

		// 6. DeleteRecords(session, ...) inside transaction
		TEST_ASSERT(db.DeleteRecords(tx.Session(), "id='rec-103'") == true, "DeleteRecords failed inside transaction");

		TEST_ASSERT(tx.Commit() == true, "Failed to commit transaction");

		// Verify outside transaction: recA exists with updated value, recB & recC were deleted
		OpenWifi::TestRecord checkA, checkB, checkC;
		TEST_ASSERT(db.GetRecord("id", "rec-101", checkA) == true, "recA missing after commit");
		TEST_ASSERT(checkA.value == "Val A Updated", "recA updated value did not persist");
		TEST_ASSERT(db.GetRecord("id", "rec-102", checkB) == false, "recB unexpectedly exists after DeleteRecord commit");
		TEST_ASSERT(db.GetRecord("id", "rec-103", checkC) == false, "recC unexpectedly exists after DeleteRecords commit");
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

		TEST_ASSERT(db.CreateRecord(tx.Session(), recD) == true, "Failed to create recD in transaction");
		TEST_ASSERT(db.CreateRecord(tx.Session(), recE) == true, "Failed to create recE in transaction");

		TEST_ASSERT(tx.Rollback() == true, "Failed to rollback transaction");

		// Verify neither record exists outside transaction
		OpenWifi::TestRecord checkD, checkE;
		TEST_ASSERT(db.GetRecord("id", "rec-104", checkD) == false, "recD exists after explicit rollback");
		TEST_ASSERT(db.GetRecord("id", "rec-105", checkE) == false, "recE exists after explicit rollback");
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
			TEST_ASSERT(db.CreateRecord(tx.Session(), recF) == true, "Failed to create recF in transaction");
			// Intentionally exit scope without Commit() or Rollback()
		}

		// Verify record F was rolled back on destructor scope exit
		OpenWifi::TestRecord checkF;
		TEST_ASSERT(db.GetRecord("id", "rec-106", checkF) == false, "recF exists after scope exit auto-rollback");
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
			TEST_ASSERT(db.CreateRecord(tx.Session(), recG) == true, "Failed to create recG in transaction");

			// Attempt duplicate primary key insertion on same tx session
			OpenWifi::TestRecord recG_dup{"rec-107", "Record G Dup", "Val G Dup"};
			bool secondWriteResult = db.CreateRecord(tx.Session(), recG_dup);
			TEST_ASSERT(secondWriteResult == false, "Duplicate primary key write unexpectedly succeeded");

			// Exit scope without commit due to failed second operation
		}

		// Verify recG was completely rolled back due to second write failure
		OpenWifi::TestRecord checkG;
		TEST_ASSERT(db.GetRecord("id", "rec-107", checkG) == false, "recG exists after second write failure atomicity rollback");
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

	std::cout << "[Framework Unit Test] All DbTransaction & Session-Aware ORM Tests Passed Successfully!" << std::endl;
	return 0;
}
