/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#pragma once

#include <utility>

#include "Poco/Data/Session.h"
#include "Poco/Data/Transaction.h"
#include "Poco/Exception.h"
#include "Poco/Logger.h"

namespace OpenWifi {

	class DbTransaction final {
	  public:
		// Owns one pooled session and starts a transaction on it.
		explicit DbTransaction(Poco::Data::Session session, Poco::Logger &logger)
			: session_(std::move(session)), logger_(logger), transaction_(session_, &logger_) {}

		// Poco::Data::Transaction rolls back automatically if still active.
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

		[[nodiscard]] bool Commit() noexcept {
			try {
				if (!transaction_.isActive()) {
					return false;
				}

				transaction_.commit();
				return true;
			} catch (const Poco::Exception &e) {
				logger_.error("Failed to commit database transaction: " + e.displayText());
			} catch (...) {
				logger_.error("Failed to commit database transaction: unknown exception");
			}

			return false;
		}

		[[nodiscard]] bool Rollback() noexcept {
			try {
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

			return false;
		}

	  private:
		Poco::Data::Session session_;
		Poco::Logger &logger_;
		Poco::Data::Transaction transaction_;
	};

} // namespace OpenWifi
