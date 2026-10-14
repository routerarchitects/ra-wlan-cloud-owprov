/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 */

// RetryableTransaction_test.cpp
// Unit and PostgreSQL integration tests for RetryableTransaction.h, DbTransaction.h, and tx-aware ORM operations

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Poco/AutoPtr.h"
#include "Poco/ConsoleChannel.h"
#include "Poco/Data/DataException.h"
#include "Poco/Data/SQLite/Connector.h"
#include "Poco/Data/Session.h"
#include "Poco/Data/SessionPool.h"
#include "Poco/Data/Statement.h"
#include "Poco/Logger.h"
#include "framework/DbTransaction.h"
#include "framework/RetryableTransaction.h"
#include "framework/StorageClass.h"
#include "framework/orm.h"

#ifndef SMALL_BUILD
#include "Poco/Data/PostgreSQL/Connector.h"
#endif

#define TEST_ASSERT(cond, msg)                                                                     \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			std::cerr << "TEST FAILURE [" << __FILE__ << ":" << __LINE__ << "]: " << msg          \
			          << std::endl;                                                                \
			std::exit(1);                                                                          \
		}                                                                                          \
	} while (0)

namespace OpenWifi {

	struct RtxTestRecord {
		std::string id;
		std::string name;
		std::string value;
	};

	using RtxTestRecordTuple = Poco::Tuple<std::string, std::string, std::string>;

} // namespace OpenWifi

template <>
void ORM::DB<OpenWifi::RtxTestRecordTuple, OpenWifi::RtxTestRecord>::Convert(
	const OpenWifi::RtxTestRecordTuple &In, OpenWifi::RtxTestRecord &Out) {
	Out.id = In.get<0>();
	Out.name = In.get<1>();
	Out.value = In.get<2>();
}

template <>
void ORM::DB<OpenWifi::RtxTestRecordTuple, OpenWifi::RtxTestRecord>::Convert(
	const OpenWifi::RtxTestRecord &In, OpenWifi::RtxTestRecordTuple &Out) {
	Out.set<0>(In.id);
	Out.set<1>(In.name);
	Out.set<2>(In.value);
}

class RtxTestDB : public ORM::DB<OpenWifi::RtxTestRecordTuple, OpenWifi::RtxTestRecord> {
  public:
	RtxTestDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L)
		: DB(T, "rtx_test_records",
		     ORM::FieldVec{
		         ORM::Field{"id", ORM::FieldType::FT_TEXT, 0, true},
		         ORM::Field{"name", ORM::FieldType::FT_TEXT},
		         ORM::Field{"value", ORM::FieldType::FT_TEXT},
		     },
		     ORM::IndexVec{}, P, L, "rtx") {}
};

// Helper executing raw SQL, marking transaction failure on error per DbTransaction caller contract
static bool ExecSql(OpenWifi::DbTransaction &tx, const std::string &sql) {
	try {
		tx.Session() << sql, Poco::Data::Keywords::now;
		return true;
	} catch (const Poco::Exception &e) {
		tx.MarkFailed(e);
		return false;
	}
}

#ifndef SMALL_BUILD
static bool DropRtxTable(Poco::Data::SessionPool &pool) {
	try {
		Poco::Data::Session s = pool.get();
		Poco::Data::Statement stmt(s);
		stmt << "DROP TABLE IF EXISTS rtx_test_records";
		stmt.execute();
		return true;
	} catch (const std::exception &e) {
		std::cerr << "DropRtxTable: " << e.what() << std::endl;
		return false;
	}
}

static bool CreateRtxTable(Poco::Data::SessionPool &pool) {
	try {
		Poco::Data::Session s = pool.get();
		Poco::Data::Statement stmt(s);
		stmt << "CREATE TABLE IF NOT EXISTS rtx_test_records ("
		     << "  id VARCHAR(64) PRIMARY KEY,"
		     << "  name VARCHAR(64) NOT NULL,"
		     << "  value VARCHAR(64) NOT NULL"
		     << ")";
		stmt.execute();
		return true;
	} catch (const std::exception &e) {
		std::cerr << "CreateRtxTable: " << e.what() << std::endl;
		return false;
	}
}
#endif

int main() {
	Poco::AutoPtr<Poco::ConsoleChannel> ch(new Poco::ConsoleChannel);
	Poco::Logger &logger = Poco::Logger::get("RetryableTransaction_test");
	logger.setChannel(ch);
	logger.setLevel(Poco::Message::PRIO_WARNING);

	Poco::Data::SQLite::Connector::registerConnector();

	std::cout << "[RetryableTransaction Tests] Starting..." << std::endl;

	// =========================================================================
	// Section 1: In-Memory Unit Tests (no DB connection required)
	// =========================================================================

	// 1a. SQLSTATE Extraction & Restriction to Poco::Data::DataException
	{
		std::cout << "  - SQLSTATE extraction & provenance restriction..." << std::flush;
		std::string rawMsg = "PostgreSQL: error State: 55P03 Detail: N/A";
		TEST_ASSERT(OpenWifi::detail::ExtractSqlState(rawMsg) == "55P03", "Must extract 55P03");

		Poco::Data::DataException dbEx("error State: 40001 Detail: N/A");
		TEST_ASSERT(OpenWifi::detail::ExtractSqlState(dbEx) == "40001", "Must extract from DataException");

		// Commit-time PostgreSQL exceptions lacking " State: <code>" (e.g. SessionHandle::commit())
		Poco::Data::DataException commitSerEx(
		    "StatementException: COMMIT statement failed:: ERROR: could not serialize access "
		    "due to read/write dependencies among transactions\n"
		    "DETAIL: Reason code: Canceled on identification as a pivot, during commit.\n"
		    "HINT: The transaction might succeed if retried.");
		TEST_ASSERT(OpenWifi::detail::ExtractSqlState(commitSerEx) == "40001",
		            "Must extract 40001 from commit serialization failure lacking explicit State formatting");

		Poco::Data::DataException commitDeadlockEx(
		    "StatementException: COMMIT statement failed:: ERROR: deadlock detected");
		TEST_ASSERT(OpenWifi::detail::ExtractSqlState(commitDeadlockEx) == "40P01",
		            "Must extract 40P01 from commit deadlock failure lacking explicit State formatting");

		Poco::Data::DataException commitLockTimeoutEx(
		    "StatementException: COMMIT statement failed:: ERROR: canceling statement due to lock timeout");
		TEST_ASSERT(OpenWifi::detail::ExtractSqlState(commitLockTimeoutEx) == "55P03",
		            "Must extract 55P03 from commit lock timeout failure lacking explicit State formatting");

		Poco::ApplicationException nonDbEx("simulated State: 40001 Detail: N/A");
		TEST_ASSERT(OpenWifi::detail::ExtractSqlState(nonDbEx).empty(),
		            "Non-DataException must return empty SQLSTATE");
		std::cout << "PASSED" << std::endl;
	}

	// 1b. DbTransaction MarkFailed semantics & first meaningful metadata preservation
	{
		std::cout << "  - DbTransaction MarkFailed metadata preservation..." << std::flush;
		Poco::Data::Session dummySession("SQLite", ":memory:");
		OpenWifi::DbTransaction tx(dummySession, logger);

		TEST_ASSERT(!tx.HasFailed(), "Initial HasFailed must be false");
		TEST_ASSERT(tx.FailureSqlState().empty(), "Initial FailureSqlState must be empty");

		tx.MarkFailed();
		TEST_ASSERT(tx.HasFailed(), "HasFailed must be true after MarkFailed()");

		// First non-empty SQLSTATE wins
		tx.MarkFailed("55P03", "lock timeout");
		TEST_ASSERT(tx.FailureSqlState() == "55P03", "FailureSqlState must be 55P03");
		TEST_ASSERT(tx.FailureMessage() == "lock timeout", "FailureMessage must match");

		tx.MarkFailed("40001", "later serialization error");
		TEST_ASSERT(tx.FailureSqlState() == "55P03", "First non-empty SQLSTATE must be preserved");
		TEST_ASSERT(tx.FailureMessage() == "lock timeout", "First non-empty message must be preserved");

		// Exception overloads
		Poco::Data::Session dummySession2("SQLite", ":memory:");
		OpenWifi::DbTransaction tx2(dummySession2, logger);
		Poco::Data::DataException pEx("db conflict State: 40P01 Detail: deadlock");
		tx2.MarkFailed(pEx);
		TEST_ASSERT(tx2.FailureSqlState() == "40P01", "Must extract 40P01 via MarkFailed(Poco::Exception)");

		Poco::Data::Session dummySession3("SQLite", ":memory:");
		OpenWifi::DbTransaction tx3(dummySession3, logger);
		std::runtime_error stdEx("regular std exception");
		tx3.MarkFailed("", stdEx.what());
		TEST_ASSERT(tx3.HasFailed(), "Must mark failed via MarkFailed");
		TEST_ASSERT(tx3.FailureSqlState().empty(), "Non-DB std::exception should have empty SQLSTATE");
		TEST_ASSERT(tx3.FailureMessage() == "regular std exception", "FailureMessage must match std::exception what()");

		Poco::Data::Session dummySession4("SQLite", ":memory:");
		OpenWifi::DbTransaction tx4(dummySession4, logger);
		Poco::Data::DataException commitEx(
		    "StatementException: COMMIT statement failed:: ERROR: could not serialize access "
		    "due to read/write dependencies among transactions");
		tx4.MarkFailed(commitEx);
		TEST_ASSERT(tx4.FailureSqlState() == "40001",
		            "Must preserve 40001 SQLSTATE for COMMIT-time serialization conflicts via MarkFailed");
		std::cout << "PASSED" << std::endl;
	}

	// 1c. SQLSTATE Retryability & Final Classification
	{
		std::cout << "  - SQLSTATE classification..." << std::flush;
		TEST_ASSERT(OpenWifi::detail::IsRetryableSqlState("40001"), "40001 is retryable");
		TEST_ASSERT(OpenWifi::detail::IsRetryableSqlState("40P01"), "40P01 is retryable");
		TEST_ASSERT(OpenWifi::detail::IsRetryableSqlState("55P03"), "55P03 is retryable");
		TEST_ASSERT(!OpenWifi::detail::IsRetryableSqlState("23505"), "23505 is not retryable");
		TEST_ASSERT(!OpenWifi::detail::IsRetryableSqlState("42P01"), "42P01 is not retryable");

		TEST_ASSERT(OpenWifi::detail::FinalStatusForSqlState("23505") ==
		                OpenWifi::RetryableTransactionStatus::UniqueViolation,
		            "23505 maps to UniqueViolation");
		TEST_ASSERT(OpenWifi::detail::FinalStatusForSqlState("55P03") ==
		                OpenWifi::RetryableTransactionStatus::RetryExhausted,
		            "55P03 maps to RetryExhausted when final");
		TEST_ASSERT(OpenWifi::detail::FinalStatusForSqlState("42P01") ==
		                OpenWifi::RetryableTransactionStatus::OperationFailed,
		            "42P01 maps to OperationFailed");
		std::cout << "PASSED" << std::endl;
	}

	// 1d. Backoff and Jitter Calculation
	{
		std::cout << "  - Backoff and jitter calculation..." << std::flush;
		std::mt19937 rng(42);
		OpenWifi::RetryableTransactionOptions opts;
		opts.maxBackoffMs = 100;
		opts.jitter = false;

		uint32_t sleepNoJitter = OpenWifi::CalculateRetrySleepMs(50, opts, rng);
		TEST_ASSERT(sleepNoJitter == 50, "Without jitter, sleep must equal bounded backoff");

		opts.jitter = true;
		for (int i = 0; i < 50; ++i) {
			uint32_t s = OpenWifi::CalculateRetrySleepMs(40, opts, rng);
			TEST_ASSERT(s >= 30 && s <= 50, "Sleep must be within jitter bounds [30, 50]");
		}
		std::cout << "PASSED" << std::endl;
	}

	// 1e. Options Validation & Begin Failure
	{
		std::cout << "  - Options validation & beginTx failure handling..." << std::flush;
		OpenWifi::RetryableTransactionOptions badOpts;
		badOpts.maxAttempts = 0;

		auto dummyBegin = []() -> OpenWifi::DbTransaction {
			throw std::logic_error("should not be called");
		};

		auto resBad = OpenWifi::RunRetryableTransaction(logger, badOpts, dummyBegin,
		                                                [](OpenWifi::DbTransaction &) { return true; });
		TEST_ASSERT(!resBad.Succeeded(), "Must fail when maxAttempts == 0");
		TEST_ASSERT(resBad.attempts == 0, "No attempts should run when maxAttempts == 0");

		OpenWifi::RetryableTransactionOptions okOpts;
		okOpts.maxAttempts = 2;
		auto throwingBegin = []() -> OpenWifi::DbTransaction {
			throw std::runtime_error("pool exhausted");
		};

		bool lambdaCalled = false;
		auto resThrow = OpenWifi::RunRetryableTransaction(
		    logger, okOpts, throwingBegin,
		    [&](OpenWifi::DbTransaction &) {
			    lambdaCalled = true;
			    return true;
		    });

		TEST_ASSERT(!resThrow.Succeeded(), "Must fail when beginTx throws");
		TEST_ASSERT(!lambdaCalled, "Lambda must not be invoked if beginTx throws");
		TEST_ASSERT(resThrow.status == OpenWifi::RetryableTransactionStatus::OperationFailed,
		            "Must return OperationFailed on beginTx error");
		std::cout << "PASSED" << std::endl;
	}

	// 1f. Non-DB Exceptions Escape and Are Rethrown
	{
		std::cout << "  - Non-DB exceptions escape and are rethrown..." << std::flush;
		Poco::Data::Session dummySession("SQLite", ":memory:");
		auto beginTx = [&]() -> OpenWifi::DbTransaction {
			return OpenWifi::DbTransaction(dummySession, logger);
		};

		OpenWifi::RetryableTransactionOptions opts;
		opts.maxAttempts = 3;

		bool caught = false;
		try {
			(void)OpenWifi::RunRetryableTransaction(logger, opts, beginTx, [](OpenWifi::DbTransaction &) -> bool {
				throw std::runtime_error("app logic error");
			});
		} catch (const std::runtime_error &e) {
			caught = (std::string(e.what()) == "app logic error");
		}

		TEST_ASSERT(caught, "Generic std::exception must escape RunRetryableTransaction");
		std::cout << "PASSED" << std::endl;
	}

	// 1g. Simulated commit-failure metadata extraction & retry
	{
		std::cout << "  - Simulated commit-failure metadata extraction & retry..." << std::flush;
		Poco::Data::Session dummySession("SQLite", ":memory:");
		auto beginTx = [&]() -> OpenWifi::DbTransaction {
			return OpenWifi::DbTransaction(dummySession, logger);
		};

		OpenWifi::RetryableTransactionOptions opts;
		opts.maxAttempts = 3;
		opts.initialBackoffMs = 1;
		opts.maxBackoffMs = 5;
		opts.jitter = false;

		int attempts = 0;
		auto res = OpenWifi::RunRetryableTransaction(logger, opts, beginTx, [&](OpenWifi::DbTransaction &tx) -> bool {
			++attempts;
			if (attempts == 1) {
				Poco::Data::DataException commitSerEx(
				    "StatementException: COMMIT statement failed:: ERROR: could not serialize access "
				    "due to read/write dependencies among transactions");
				tx.MarkFailed(commitSerEx);
				return false;
			}
			return true;
		});

		TEST_ASSERT(res.Succeeded(), "Must succeed after retrying commit serialization failure");
		TEST_ASSERT(res.attempts == 2, "Must retry exactly once (total 2 attempts)");
		TEST_ASSERT(res.status == OpenWifi::RetryableTransactionStatus::Committed, "Status must be Committed");
		std::cout << "PASSED" << std::endl;
	}

	// =========================================================================
	// Section 2: PostgreSQL Integration Tests (ORM & Concurrency)
	// =========================================================================
#ifdef SMALL_BUILD
	std::cout << "[RetryableTransaction Tests] Skipping PostgreSQL integration tests (SMALL_BUILD defined)." << std::endl;
	return 77;
#else
	const char *pgHost = std::getenv("PGHOST");
	const char *pgPort = std::getenv("PGPORT");
	const char *pgUser = std::getenv("PGUSER");
	const char *pgPassword = std::getenv("PGPASSWORD");
	const char *pgDatabase = std::getenv("PGDATABASE");

	if (!pgHost || !pgUser || !pgPassword || !pgDatabase) {
		std::cout << "[RetryableTransaction Tests] Skipping PostgreSQL integration tests (PG env vars not set)." << std::endl;
		return 77;
	}

	std::string connStr = "host=" + std::string(pgHost) +
	                      " port=" + (pgPort ? std::string(pgPort) : "5432") +
	                      " user=" + std::string(pgUser) +
	                      " password=" + std::string(pgPassword) +
	                      " dbname=" + std::string(pgDatabase);

	Poco::Data::PostgreSQL::Connector::registerConnector();
	std::unique_ptr<Poco::Data::SessionPool> pool;
	try {
		pool = std::make_unique<Poco::Data::SessionPool>("PostgreSQL", connStr, 1, 16, 5);
	} catch (const std::exception &e) {
		std::cout << "Could not connect to PostgreSQL: " << e.what() << " - skipping." << std::endl;
		return 77;
	}

	DropRtxTable(*pool);
	if (!CreateRtxTable(*pool)) {
		std::cerr << "Failed creating rtx_test_records table" << std::endl;
		return 1;
	}

	RtxTestDB pgDb(OpenWifi::DBType::pgsql, *pool, logger);

	auto SeedRecord = [&](std::string id, std::string name, std::string val) {
		Poco::Data::Session s = pool->get();
		Poco::Data::Statement stmt(s);
		stmt << "INSERT INTO rtx_test_records (id, name, value) VALUES ($1, $2, $3) "
		        "ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name, value = EXCLUDED.value",
		    Poco::Data::Keywords::use(id),
		    Poco::Data::Keywords::use(name),
		    Poco::Data::Keywords::use(val),
		    Poco::Data::Keywords::now;
	};

	auto AssertRecordValue = [&](std::string id, const std::string &expectedVal) {
		Poco::Data::Session s = pool->get();
		Poco::Data::Statement stmt(s);
		std::string val;
		stmt << "SELECT value FROM rtx_test_records WHERE id = $1",
		    Poco::Data::Keywords::into(val),
		    Poco::Data::Keywords::use(id),
		    Poco::Data::Keywords::now;
		TEST_ASSERT(val == expectedVal, "Expected record value '" + expectedVal + "', got '" + val + "'");
	};

	auto beginTx = [&]() -> OpenWifi::DbTransaction {
		return OpenWifi::DbTransaction(pool->get(), logger);
	};

	auto MakeOpts = [](const std::string &ctx, uint32_t maxAttempts = 3, uint32_t initBackoff = 10,
	                   uint32_t maxBackoff = 50, bool jitter = false) {
		OpenWifi::RetryableTransactionOptions opts;
		opts.logContext = ctx;
		opts.maxAttempts = maxAttempts;
		opts.initialBackoffMs = initBackoff;
		opts.maxBackoffMs = maxBackoff;
		opts.jitter = jitter;
		return opts;
	};

	auto AssertResult = [](const OpenWifi::RetryableTransactionResult &res,
	                       OpenWifi::RetryableTransactionStatus expectedStatus,
	                       uint32_t expectedAttempts,
	                       const std::string &expectedSqlState = "") {
		TEST_ASSERT(res.status == expectedStatus, "Unexpected result status");
		TEST_ASSERT(res.attempts == expectedAttempts, "Unexpected attempt count");
		if (!expectedSqlState.empty()) {
			TEST_ASSERT(res.lastFailureSqlState == expectedSqlState,
			            "Unexpected SQLSTATE: got " + res.lastFailureSqlState + ", expected " + expectedSqlState);
		}
	};

	// -------------------------------------------------------------------------
	// Test A: First-attempt success using ORM CreateRecord
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test A: First-attempt success (ORM CreateRecord)..." << std::flush;
		auto result = OpenWifi::RunRetryableTransaction(
		    logger, MakeOpts("TestA"), beginTx,
		    [&](OpenWifi::DbTransaction &tx) -> bool {
			    OpenWifi::RtxTestRecord rec{"rtx-a-001", "Row A", "init"};
			    return pgDb.CreateRecord(tx, rec);
		    });

		AssertResult(result, OpenWifi::RetryableTransactionStatus::Committed, 1);
		AssertRecordValue("rtx-a-001", "init");
		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test B: Lock-timeout retry (55P03) using ORM GetRecordForUpdate
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test B: Lock-timeout retry via ORM (55P03)..." << std::flush;
		SeedRecord("rtx-b-001", "Row B", "init");

		// txHolder locks the row
		OpenWifi::DbTransaction txHolder(pool->get(), logger);
		TEST_ASSERT(ExecSql(txHolder, "SELECT id FROM rtx_test_records WHERE id='rtx-b-001' FOR UPDATE"),
		            "Holder failed locking row");

		auto beginTxShort = [&]() -> OpenWifi::DbTransaction {
			return OpenWifi::DbTransaction(pool->get(), logger, /*lockTimeoutMs=*/50);
		};

		std::atomic<int> lambdaCalls{0};
		auto future = std::async(std::launch::async, [&]() {
			return OpenWifi::RunRetryableTransaction(
			    logger, MakeOpts("TestB", 5, 10, 50, false), beginTxShort,
			    [&](OpenWifi::DbTransaction &tx) -> bool {
				    lambdaCalls++;
				    OpenWifi::RtxTestRecord rec;
				    // GetRecordForUpdate calls tx.MarkFailed(E) on lock timeout (55P03)
				    if (!pgDb.GetRecordForUpdate(tx, "id", std::string("rtx-b-001"), rec)) {
					    return false;
				    }
				    rec.value = "locked-then-updated";
				    return pgDb.UpdateRecord(tx, "id", rec.id, rec);
			    });
		});

		// Wait until worker attempted lock, timed out (55P03), and entered attempt 2
		auto startWait = std::chrono::steady_clock::now();
		while (lambdaCalls.load() < 2) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startWait).count() > 5) {
				TEST_ASSERT(false, "Timeout waiting for worker to retry (attempt 2)");
			}
		}

		// Release lock so retry can proceed
		TEST_ASSERT(txHolder.Commit(), "txHolder commit failed");

		auto result = future.get();
		TEST_ASSERT(result.status == OpenWifi::RetryableTransactionStatus::Committed, "Result must be Committed");
		TEST_ASSERT(result.attempts >= 2, "Attempts must be >= 2");
		TEST_ASSERT(result.lastFailureSqlState == "55P03", "Must have recorded 55P03");
		AssertRecordValue("rtx-b-001", "locked-then-updated");
		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test C: Deadlock retry (40P01)
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test C: Deadlock retry (40P01)..." << std::flush;
		SeedRecord("rtx-c-001", "DeadlockA", "v0");
		SeedRecord("rtx-c-002", "DeadlockB", "v0");

		std::promise<void> p1;
		std::promise<void> p2;
		std::shared_future<void> f1 = p1.get_future().share();
		std::shared_future<void> f2 = p2.get_future().share();
		std::atomic<int> tx1Calls{0};
		std::atomic<int> tx2Calls{0};

		auto beginTxDl = [&]() -> OpenWifi::DbTransaction {
			return OpenWifi::DbTransaction(pool->get(), logger, /*lockTimeoutMs=*/2000);
		};

		auto opts = MakeOpts("TestC", 5, 10, 100, false);

		auto fut1 = std::async(std::launch::async, [&]() {
			return OpenWifi::RunRetryableTransaction(
			    logger, opts, beginTxDl,
			    [&](OpenWifi::DbTransaction &tx) -> bool {
				    int call = ++tx1Calls;
				    if (!ExecSql(tx, "SET LOCAL deadlock_timeout = '100ms'")) return false;
				    if (!ExecSql(tx, "SELECT id FROM rtx_test_records WHERE id='rtx-c-001' FOR UPDATE")) return false;
				    if (call == 1) {
					    p1.set_value();
					    TEST_ASSERT(f2.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
					                "Timeout waiting for worker 2 in Test C");
				    }
				    if (!ExecSql(tx, "SELECT id FROM rtx_test_records WHERE id='rtx-c-002' FOR UPDATE")) return false;
				    return ExecSql(tx, "UPDATE rtx_test_records SET value='w1-done' WHERE id='rtx-c-001'");
			    });
		});

		auto fut2 = std::async(std::launch::async, [&]() {
			return OpenWifi::RunRetryableTransaction(
			    logger, opts, beginTxDl,
			    [&](OpenWifi::DbTransaction &tx) -> bool {
				    int call = ++tx2Calls;
				    if (!ExecSql(tx, "SET LOCAL deadlock_timeout = '100ms'")) return false;
				    if (!ExecSql(tx, "SELECT id FROM rtx_test_records WHERE id='rtx-c-002' FOR UPDATE")) return false;
				    if (call == 1) {
					    p2.set_value();
					    TEST_ASSERT(f1.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
					                "Timeout waiting for worker 1 in Test C");
				    }
				    if (!ExecSql(tx, "SELECT id FROM rtx_test_records WHERE id='rtx-c-001' FOR UPDATE")) return false;
				    return ExecSql(tx, "UPDATE rtx_test_records SET value='w2-done' WHERE id='rtx-c-002'");
			    });
		});

		auto res1 = fut1.get();
		auto res2 = fut2.get();

		bool sawDeadlock = (res1.lastFailureSqlState == "40P01" || res2.lastFailureSqlState == "40P01");
		TEST_ASSERT(res1.status == OpenWifi::RetryableTransactionStatus::Committed, "Worker 1 must commit");
		TEST_ASSERT(res2.status == OpenWifi::RetryableTransactionStatus::Committed, "Worker 2 must commit");
		TEST_ASSERT(sawDeadlock, "At least one worker must have observed SQLSTATE 40P01");
		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test D: Max attempts exhausted (55P03)
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test D: Max attempts exhausted..." << std::flush;
		SeedRecord("rtx-d-001", "Row D", "v0");

		OpenWifi::DbTransaction txHolder(pool->get(), logger);
		TEST_ASSERT(ExecSql(txHolder, "SELECT id FROM rtx_test_records WHERE id='rtx-d-001' FOR UPDATE"),
		            "txHolder failed locking");

		auto beginTxShort = [&]() -> OpenWifi::DbTransaction {
			return OpenWifi::DbTransaction(pool->get(), logger, /*lockTimeoutMs=*/50);
		};

		int lambdaCalls = 0;
		auto result = OpenWifi::RunRetryableTransaction(
		    logger, MakeOpts("TestD", 3, 5, 20, false), beginTxShort,
		    [&](OpenWifi::DbTransaction &tx) {
			    ++lambdaCalls;
			    OpenWifi::RtxTestRecord rec;
			    return pgDb.GetRecordForUpdate(tx, "id", std::string("rtx-d-001"), rec);
		    });

		TEST_ASSERT(txHolder.Commit(), "txHolder commit failed in Test D");
		AssertResult(result, OpenWifi::RetryableTransactionStatus::RetryExhausted, 3, "55P03");
		TEST_ASSERT(lambdaCalls == 3, "Lambda must have been called exactly 3 times");
		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test E: Unique violation (23505) is NOT retried via ORM CreateRecord
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test E: Unique violation (23505) not retried (ORM CreateRecord)..." << std::flush;
		SeedRecord("rtx-e-dup", "UniqueBase", "v0");

		int lambdaCalls = 0;
		auto result = OpenWifi::RunRetryableTransaction(
		    logger, MakeOpts("TestE"), beginTx,
		    [&](OpenWifi::DbTransaction &tx) -> bool {
			    ++lambdaCalls;
			    OpenWifi::RtxTestRecord dup{"rtx-e-dup", "Duplicate", "v1"};
			    return pgDb.CreateRecord(tx, dup);
		    });

		AssertResult(result, OpenWifi::RetryableTransactionStatus::UniqueViolation, 1, "23505");
		TEST_ASSERT(lambdaCalls == 1, "Lambda must only be called once for unique violation");
		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test F: AfterCommit callback fires exactly once on success via ORM UpdateRecord
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test F: AfterCommit fires exactly once (ORM UpdateRecord)..." << std::flush;
		SeedRecord("rtx-f-001", "Row F", "v0");

		int afterCommitCallCount = 0;
		auto result = OpenWifi::RunRetryableTransaction(
		    logger, MakeOpts("TestF"), beginTx,
		    [&](OpenWifi::DbTransaction &tx) -> bool {
			    OpenWifi::RtxTestRecord rec;
			    if (!pgDb.GetRecord(tx, "id", std::string("rtx-f-001"), rec)) return false;
			    rec.value = "f-updated";
			    if (!pgDb.UpdateRecord(tx, "id", rec.id, rec)) return false;
			    tx.AfterCommit([&]() { ++afterCommitCallCount; });
			    return true;
		    });

		AssertResult(result, OpenWifi::RetryableTransactionStatus::Committed, 1);
		TEST_ASSERT(afterCommitCallCount == 1, "AfterCommit callback must run exactly once");
		AssertRecordValue("rtx-f-001", "f-updated");
		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test G: AfterCommit does NOT fire when all attempts fail
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test G: AfterCommit does NOT fire on exhausted retry..." << std::flush;
		SeedRecord("rtx-g-001", "Row G", "v0");

		OpenWifi::DbTransaction txHolder(pool->get(), logger);
		TEST_ASSERT(ExecSql(txHolder, "SELECT id FROM rtx_test_records WHERE id='rtx-g-001' FOR UPDATE"),
		            "txHolder failed locking");

		auto beginTxShort = [&]() -> OpenWifi::DbTransaction {
			return OpenWifi::DbTransaction(pool->get(), logger, /*lockTimeoutMs=*/50);
		};

		int afterCommitCallCount = 0;
		auto result = OpenWifi::RunRetryableTransaction(
		    logger, MakeOpts("TestG", 2, 5, 20, false), beginTxShort,
		    [&](OpenWifi::DbTransaction &tx) -> bool {
			    tx.AfterCommit([&]() { ++afterCommitCallCount; });
			    OpenWifi::RtxTestRecord rec;
			    return pgDb.GetRecordForUpdate(tx, "id", std::string("rtx-g-001"), rec);
		    });

		TEST_ASSERT(txHolder.Commit(), "txHolder commit failed in Test G");
		AssertResult(result, OpenWifi::RetryableTransactionStatus::RetryExhausted, 2, "55P03");
		TEST_ASSERT(afterCommitCallCount == 0, "AfterCommit must NOT fire when all attempts fail");
		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test H: Rollback proof (attempt 1 dirty write rolled back via ORM)
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test H: Rollback proof (attempt 1 dirty write rolled back)..." << std::flush;
		SeedRecord("rtx-h-001", "Row H", "initial-value");

		int lambdaCalls = 0;
		auto result = OpenWifi::RunRetryableTransaction(
		    logger, MakeOpts("TestH"), beginTx,
		    [&](OpenWifi::DbTransaction &tx) -> bool {
			    ++lambdaCalls;
			    if (lambdaCalls == 1) {
				    OpenWifi::RtxTestRecord dirtyRec{"rtx-h-001", "Row H", "attempt1-dirty"};
				    if (!pgDb.UpdateRecord(tx, "id", dirtyRec.id, dirtyRec)) return false;
				    tx.MarkFailed("40001", "simulated serialization conflict");
				    return false;
			    }

			    // Attempt 2: verify attempt 1 dirty write was rolled back
			    OpenWifi::RtxTestRecord freshRec;
			    if (!pgDb.GetRecord(tx, "id", std::string("rtx-h-001"), freshRec)) return false;
			    TEST_ASSERT(freshRec.value == "initial-value",
			                "Attempt 1 dirty write was not rolled back: got " + freshRec.value);

			    OpenWifi::RtxTestRecord committedRec{"rtx-h-001", "Row H", "attempt2-committed"};
			    return pgDb.UpdateRecord(tx, "id", committedRec.id, committedRec);
		    });

		AssertResult(result, OpenWifi::RetryableTransactionStatus::Committed, 2);
		TEST_ASSERT(lambdaCalls == 2, "Lambda must be called exactly 2 times");
		AssertRecordValue("rtx-h-001", "attempt2-committed");
		std::cout << "PASSED" << std::endl;
	}

	// -------------------------------------------------------------------------
	// Test I: Real PostgreSQL commit-time serialization failure retry (40001)
	// -------------------------------------------------------------------------
	{
		std::cout << "  - Test I: Real PostgreSQL commit-time serialization retry (40001)..." << std::flush;
		SeedRecord("rtx-i-001", "Row I1", "10");
		SeedRecord("rtx-i-002", "Row I2", "20");

		auto beginSerializableTx = [&]() -> OpenWifi::DbTransaction {
			Poco::Data::Session s = pool->get();
			s << "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL SERIALIZABLE",
			    Poco::Data::Keywords::now;
			return OpenWifi::DbTransaction(std::move(s), logger);
		};

		std::promise<void> t1ReadDone, t2ReadDone, t1Committed;
		std::shared_future<void> f1Read(t1ReadDone.get_future());
		std::shared_future<void> f2Read(t2ReadDone.get_future());
		std::shared_future<void> f1Commit(t1Committed.get_future());

		std::atomic<int> workerAttempts{0};
		bool attempt1LambdaReturned = false;

		// Worker 1 runs a concurrent transaction in SERIALIZABLE isolation
		std::thread worker1([&]() {
			Poco::Data::Session s = pool->get();
			s << "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL SERIALIZABLE",
			    Poco::Data::Keywords::now;
			Poco::Data::Transaction tx(s);

			std::string val;
			s << "SELECT value FROM rtx_test_records WHERE id = 'rtx-i-002'",
			    Poco::Data::Keywords::into(val), Poco::Data::Keywords::now;
			t1ReadDone.set_value();

			TEST_ASSERT(f2Read.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
			            "Timeout waiting for worker 2 read in Test I");

			s << "UPDATE rtx_test_records SET value = '15' WHERE id = 'rtx-i-001'", Poco::Data::Keywords::now;

			tx.commit();
			t1Committed.set_value();
		});

		// Worker 2 uses RunRetryableTransaction in SERIALIZABLE mode:
		// Attempt 1 cross-reads I1 and updates I2, which forms an SSI pivot cycle against worker 1.
		// Worker 2's lambda returns true; the 40001 failure occurs strictly inside tx.Commit().
		// The retry wrapper catches the commit failure, extracts 40001, and retries cleanly.
		auto result = OpenWifi::RunRetryableTransaction(
		    logger, MakeOpts("TestI", 3, 10, 50, false), beginSerializableTx,
		    [&](OpenWifi::DbTransaction &tx) -> bool {
			    int attempt = ++workerAttempts;

			    if (attempt == 1) {
				    TEST_ASSERT(f1Read.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
				                "Timeout waiting for worker 1 read in Test I");

				    std::string val;
				    tx.Session() << "SELECT value FROM rtx_test_records WHERE id = 'rtx-i-001'",
				        Poco::Data::Keywords::into(val), Poco::Data::Keywords::now;
				    t2ReadDone.set_value();

				    TEST_ASSERT(f1Commit.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
				                "Timeout waiting for worker 1 commit in Test I");

				    tx.Session() << "UPDATE rtx_test_records SET value = '25' WHERE id = 'rtx-i-002'",
				        Poco::Data::Keywords::now;

				    // Lambda finishes and returns true: forces failure to happen inside tx.Commit()!
				    attempt1LambdaReturned = true;
				    return true;
			    }

			    // Attempt 2: succeeds cleanly
			    tx.Session() << "UPDATE rtx_test_records SET value = '30' WHERE id = 'rtx-i-002'",
			        Poco::Data::Keywords::now;
			    return true;
		    });

		worker1.join();

		TEST_ASSERT(attempt1LambdaReturned,
		            "Attempt 1 lambda must have finished and returned true, forcing failure inside tx.Commit()");
		AssertResult(result, OpenWifi::RetryableTransactionStatus::Committed, 2);
		TEST_ASSERT(workerAttempts == 2, "Must retry exactly once after real commit-time serialization failure");
		AssertRecordValue("rtx-i-001", "15");
		AssertRecordValue("rtx-i-002", "30");
		std::cout << "PASSED" << std::endl;
	}

	std::cout << "[RetryableTransaction Tests] All tests PASSED." << std::endl;
	return 0;
#endif
}
