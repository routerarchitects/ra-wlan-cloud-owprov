/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#pragma once

#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "Poco/Data/Session.h"
#include "Poco/Data/Transaction.h"
#include "Poco/Exception.h"
#include "Poco/Logger.h"

namespace OpenWifi {

	// Note: Keep transactions narrow and DB-only. Perform network/JSON operations before opening DbTransaction to prevent pool exhaustion.
	class DbTransaction final {
	  public:
		using PostCommitFunc = std::function<void()>;

		// Owns one pooled session and starts a transaction on it.
		explicit DbTransaction(Poco::Data::Session session, Poco::Logger &logger)
			: logger_(logger) {
			resources_.emplace(std::move(session), logger_);
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

		[[nodiscard]] bool Commit() noexcept {
			std::vector<PostCommitFunc> actions;

			try {
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

	  private:
		struct Resources {
			Resources(Poco::Data::Session dbSession, Poco::Logger &logger)
				: session(std::move(dbSession)), transaction(session, &logger) {}

			Poco::Data::Session session;
			Poco::Data::Transaction transaction;
		};

		Poco::Logger &logger_;
		std::optional<Resources> resources_;
		std::vector<PostCommitFunc> after_commit_actions_;
	};

} // namespace OpenWifi
