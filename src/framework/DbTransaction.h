/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#pragma once

#include <functional>
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
			: session_(std::move(session)), logger_(logger), transaction_(session_, &logger_) {}

		// Poco::Data::Transaction rolls back automatically if still active.
		// Pending post-commit actions are discarded on destruction.
		~DbTransaction() = default;

		DbTransaction(const DbTransaction &) = delete;
		DbTransaction &operator=(const DbTransaction &) = delete;
		DbTransaction(DbTransaction &&) = delete;
		DbTransaction &operator=(DbTransaction &&) = delete;

		Poco::Data::Session &Session() {
			if (!transaction_.isActive()) {
				throw Poco::IllegalStateException("Database transaction is not active");
			}
			return session_;
		}

		// Registers a callback to execute ONLY after DB commit succeeds. Callbacks run synchronously and must remain fast/lightweight (e.g. cache sync).
		void AfterCommit(PostCommitFunc fn) {
			if (!transaction_.isActive()) {
				throw Poco::IllegalStateException("Database transaction is not active");
			}
			if (fn) {
				after_commit_actions_.push_back(std::move(fn));
			}
		}

		[[nodiscard]] bool Commit() noexcept {
			try {
				if (!transaction_.isActive()) {
					return false;
				}

				transaction_.commit();

				// Execute queued post-commit actions only after DB commit succeeds
				for (const auto &fn : after_commit_actions_) {
					try {
						fn();
					} catch (const Poco::Exception &e) {
						logger_.error("Error in post-commit action: " + e.displayText());
					} catch (...) {
						logger_.error("Error in post-commit action: unknown exception");
					}
				}
				after_commit_actions_.clear();
				return true;
			} catch (const Poco::Exception &e) {
				logger_.error("Failed to commit database transaction: " + e.displayText());
			} catch (...) {
				logger_.error("Failed to commit database transaction: unknown exception");
			}

			after_commit_actions_.clear();
			return false;
		}

		[[nodiscard]] bool Rollback() noexcept {
			try {
				after_commit_actions_.clear();
				if (!transaction_.isActive()) {
					return false;
				}

				transaction_.rollback();
				return true;
			} catch (const Poco::Exception &e) {
				logger_.error("Failed to roll back database transaction: " + e.displayText());
			} catch (...) {
				logger_.error("Failed to roll back database transaction: unknown exception");
			}

			after_commit_actions_.clear();
			return false;
		}

	  private:
		Poco::Data::Session session_;
		Poco::Logger &logger_;
		Poco::Data::Transaction transaction_;
		std::vector<PostCommitFunc> after_commit_actions_;
	};

} // namespace OpenWifi

