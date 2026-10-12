/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Poco/Data/Session.h"
#include "Poco/Data/Statement.h"
#include "Poco/Data/Transaction.h"
#include "Poco/Exception.h"
#include "Poco/Logger.h"
#include "Poco/String.h"

namespace OpenWifi {

	// Caller Contract for DbTransaction:
	// 1. Keep transactions narrow and DB-only. Perform network/JSON operations before opening DbTransaction to prevent pool exhaustion.
	// 2. API workflows that require multi-operation atomicity, row-level locking,
	//    or rollback across related DB writes must use StorageService()->BeginTransaction()
	//    to define the transaction boundary.
	// 3. If any required transaction-aware ORM operation returns false, the caller MUST call Rollback() or allow the DbTransaction object to exit scope (RAII auto-rollback).
	// 4. Calling Commit() after a failed required operation is a caller bug.
	// 5. AfterCommit callbacks run ONLY after a successful DB commit and after DB session resources are released. Callbacks must NOT be used to make a committed DB transaction appear failed.
	// 6. Transaction-aware ORM helpers automatically catch lock/statement timeouts and mark the transaction failed.
	//    Callers executing raw SQL directly through tx.Session() must catch database exceptions, handle errors,
	//    and explicitly call tx.MarkFailed() before rolling back (or allow RAII auto-rollback).
	class DbTransaction final {
	  public:
		using PostCommitFunc = std::function<void()>;

		static constexpr uint64_t DEFAULT_LOCK_TIMEOUT_MS = 2000;
		static constexpr uint64_t DEFAULT_STATEMENT_TIMEOUT_MS = 0;

		static void SetDefaultTimeouts(uint64_t lockTimeoutMs, uint64_t statementTimeoutMs = 0) noexcept {
			default_lock_timeout_ms_.store(lockTimeoutMs);
			default_statement_timeout_ms_.store(statementTimeoutMs);
		}

		// Owns one pooled session and starts a transaction on it.
		// On PostgreSQL, applies transaction-local lock_timeout (default 2000ms) and optional statement_timeout.
		explicit DbTransaction(Poco::Data::Session session, Poco::Logger &logger,
		                        uint64_t lockTimeoutMs = 0, uint64_t statementTimeoutMs = 0)
			: logger_(logger),
			  lockTimeoutMs_(lockTimeoutMs > 0 ? lockTimeoutMs : default_lock_timeout_ms_.load()),
			  statementTimeoutMs_(statementTimeoutMs > 0 ? statementTimeoutMs : default_statement_timeout_ms_.load()) {
			resources_.emplace(std::move(session), logger_, lockTimeoutMs_, statementTimeoutMs_);
		}

		// Poco::Data::Transaction rolls back automatically if still active.
		// Pending post-commit actions are discarded on destruction.
		~DbTransaction() = default;

		DbTransaction(const DbTransaction &) = delete;
		DbTransaction &operator=(const DbTransaction &) = delete;
		DbTransaction(DbTransaction &&) = delete;
		DbTransaction &operator=(DbTransaction &&) = delete;

		// Returned reference is valid only while the transaction is active.
		Poco::Data::Session &Session() {
			if (!resources_ || !resources_->transaction.isActive()) {
				throw Poco::IllegalStateException("Database transaction is not active");
			}

			return resources_->session;
		}

		// Runs only after a successful DB commit.
		// Callbacks are synchronous and run after DB resources are released.
		void AfterCommit(PostCommitFunc fn) {
			if (!resources_ || !resources_->transaction.isActive()) {
				throw Poco::IllegalStateException("Database transaction is not active");
			}
			if (fn) {
				after_commit_actions_.push_back(std::move(fn));
			}
		}

		// Marks transaction as failed when an operation within it encounters an error.
		// A failed transaction refuses to commit and enforces rollback.
		void MarkFailed() noexcept {
			failed_ = true;
		}

		[[nodiscard]] bool HasFailed() const noexcept {
			return failed_;
		}

		[[nodiscard]] bool Commit() noexcept {
			std::vector<PostCommitFunc> actions;

			try {
				if (failed_) {
					logger_.error("Cannot commit database transaction: an operation within the transaction failed");
					(void)Rollback();
					return false;
				}

				if (!resources_ || !resources_->transaction.isActive()) {
					return false;
				}

				resources_->transaction.commit();

				actions.swap(after_commit_actions_);

				// Release DB transaction and pooled session before callbacks.
				resources_.reset();

			} catch (const Poco::Exception &e) {
				logger_.error("Failed to commit database transaction: " + e.displayText());
				after_commit_actions_.clear();
				return false;
			} catch (...) {
				logger_.error("Failed to commit database transaction: unknown exception");
				after_commit_actions_.clear();
				return false;
			}

			for (const auto &fn : actions) {
				try {
					fn();
				} catch (const Poco::Exception &e) {
					logger_.error("Error in post-commit action: " + e.displayText());
				} catch (...) {
					logger_.error("Error in post-commit action: unknown exception");
				}
			}

			return true;
		}

		[[nodiscard]] bool Rollback() noexcept {
			after_commit_actions_.clear();

			try {
				if (!resources_ || !resources_->transaction.isActive()) {
					return false;
				}

				resources_->transaction.rollback();

				// Return the session immediately.
				resources_.reset();

				return true;

			} catch (const Poco::Exception &e) {
				logger_.error("Failed to roll back database transaction: " + e.displayText());
			} catch (...) {
				logger_.error("Failed to roll back database transaction: unknown exception");
			}

			return false;
		}

		[[nodiscard]] uint64_t LockTimeoutMs() const noexcept {
			return lockTimeoutMs_;
		}

		[[nodiscard]] uint64_t StatementTimeoutMs() const noexcept {
			return statementTimeoutMs_;
		}

	  private:
		struct Resources {
			Resources(Poco::Data::Session dbSession, Poco::Logger &logger, uint64_t lockTimeoutMs, uint64_t statementTimeoutMs)
				: session(std::move(dbSession)), transaction(session, &logger) {
				if (Poco::toLower(session.connector()) == "postgresql") {
					if (lockTimeoutMs > 0) {
						session << "SET LOCAL lock_timeout = '" + std::to_string(lockTimeoutMs) + "ms'", Poco::Data::Keywords::now;
					}
					if (statementTimeoutMs > 0) {
						session << "SET LOCAL statement_timeout = '" + std::to_string(statementTimeoutMs) + "ms'", Poco::Data::Keywords::now;
					}
				}
			}

			Poco::Data::Session session;
			Poco::Data::Transaction transaction;
		};

		inline static std::atomic<uint64_t> default_lock_timeout_ms_{DEFAULT_LOCK_TIMEOUT_MS};
		inline static std::atomic<uint64_t> default_statement_timeout_ms_{DEFAULT_STATEMENT_TIMEOUT_MS};

		Poco::Logger &logger_;
		std::optional<Resources> resources_;
		std::vector<PostCommitFunc> after_commit_actions_;
		uint64_t lockTimeoutMs_ = DEFAULT_LOCK_TIMEOUT_MS;
		uint64_t statementTimeoutMs_ = DEFAULT_STATEMENT_TIMEOUT_MS;
		bool failed_ = false;
	};

} // namespace OpenWifi
