/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <random>
#include <string>
#include <thread>

#include "Poco/Data/DataException.h"
#include "Poco/Exception.h"
#include "Poco/Logger.h"
#include "framework/DbTransaction.h"

namespace OpenWifi {

	enum class RetryableTransactionStatus { Committed, RetryExhausted, UniqueViolation, OperationFailed };

	namespace detail {
		[[nodiscard]] inline bool IsRetryableSqlState(const std::string &s) noexcept {
			return s == "40001" || s == "40P01" || s == "55P03";
		}

		[[nodiscard]] inline bool IsUniqueViolation(const std::string &s) noexcept {
			return s == "23505";
		}

		[[nodiscard]] inline RetryableTransactionStatus FinalStatusForSqlState(const std::string &sqlstate) noexcept {
			if (IsUniqueViolation(sqlstate)) return RetryableTransactionStatus::UniqueViolation;
			if (IsRetryableSqlState(sqlstate)) return RetryableTransactionStatus::RetryExhausted;
			return RetryableTransactionStatus::OperationFailed;
		}
	} // namespace detail

	struct RetryableTransactionOptions {
		uint32_t maxAttempts{3};
		uint32_t initialBackoffMs{25}; // Setting to 0 disables backoff delay
		uint32_t maxBackoffMs{250};    // Setting to 0 disables backoff delay
		bool jitter{true};
		std::string logContext{"retryable_transaction"};
	};

	struct RetryableTransactionResult {
		RetryableTransactionStatus status{RetryableTransactionStatus::OperationFailed};
		uint32_t attempts{0};
		std::string lastFailureSqlState;
		std::string lastFailureMessage;

		[[nodiscard]] bool Succeeded() const noexcept { return status == RetryableTransactionStatus::Committed; }
	};

	// Computes sleep duration with bounded exponential backoff and jitter.
	template <typename URBG>
	[[nodiscard]] inline uint32_t CalculateRetrySleepMs(
		uint32_t currentBackoffMs, const RetryableTransactionOptions &opts, URBG &rng) noexcept {
		uint64_t base = std::min(currentBackoffMs, opts.maxBackoffMs);
		if (base == 0) return 0;
		if (!opts.jitter) return static_cast<uint32_t>(base);
		uint64_t high = std::min(base * 5 / 4, static_cast<uint64_t>(opts.maxBackoffMs));
		uint64_t low = std::min(base * 3 / 4, high);
		return static_cast<uint32_t>(std::uniform_int_distribution<uint64_t>(low, high)(rng));
	}

	// Runs lambda(tx) with a fresh DbTransaction per attempt, retrying transient concurrency conflicts
	// (40001 serialization_failure, 40P01 deadlock_detected, 55P03 lock_not_available) with backoff.
	// Caller Contract:
	//   1. Perform only DB work on tx (no network, Kafka, or non-idempotent side effects).
	//   2. Use tx.AfterCommit(...) for actions that must run exactly once after a successful commit.
	//   3. Stop immediately after any required DB operation fails; do not call tx.Commit() after failure.
	//   4. Do not hold locks or resources across invocations; each attempt gets a fresh DbTransaction.
	//   5. Non-DB exceptions are rolled back and rethrown; only PostgreSQL/Data SQLSTATE failures are retried.
	template <typename BeginTxFn, typename LambdaFn>
	[[nodiscard]] inline RetryableTransactionResult RunRetryableTransaction(
		Poco::Logger &logger,
		const RetryableTransactionOptions &opts,
		BeginTxFn &&beginTx,
		LambdaFn &&lambda) {

		RetryableTransactionResult res;
		std::string logPrefix = "[" + (opts.logContext.empty() ? "retryable_transaction" : opts.logContext) + "]";
		if (opts.maxAttempts == 0) {
			res.lastFailureMessage = "maxAttempts must be greater than 0";
			logger.error(logPrefix + " " + res.lastFailureMessage);
			return res;
		}

		static thread_local std::mt19937 rng{std::random_device{}()};
		uint32_t backoffMs = std::min(opts.initialBackoffMs, opts.maxBackoffMs);

		for (uint32_t attempt = 1; attempt <= opts.maxAttempts; ++attempt) {
			res.attempts = attempt;
			bool beginSucceeded = false;

			try {
				DbTransaction tx = beginTx();
				beginSucceeded = true;

				bool ok = false;
				try {
					ok = lambda(tx);
				} catch (const Poco::Data::DataException &e) {
					tx.MarkFailed(e);
				} catch (const Poco::Exception &e) {
					(void)tx.Rollback();
					throw;
				} catch (...) {
					(void)tx.Rollback();
					throw;
				}

				if (ok && !tx.HasFailed() && tx.Commit()) {
					res.status = RetryableTransactionStatus::Committed;
					return res;
				}

				(void)tx.Rollback();
				res.lastFailureSqlState = tx.FailureSqlState();
				res.lastFailureMessage = tx.FailureMessage();
				if (res.lastFailureMessage.empty()) {
					res.lastFailureMessage = "operation returned false without transaction failure metadata";
				}
			} catch (const std::exception &e) {
				if (!beginSucceeded) {
					res.lastFailureSqlState = "";
					res.lastFailureMessage = "Failed to begin transaction: " + std::string(e.what());
					logger.error(logPrefix + " " + res.lastFailureMessage);
					res.status = RetryableTransactionStatus::OperationFailed;
					return res;
				}
				throw;
			} catch (...) {
				if (!beginSucceeded) {
					res.lastFailureMessage = "Failed to begin transaction: unknown exception";
					logger.error(logPrefix + " " + res.lastFailureMessage);
					res.status = RetryableTransactionStatus::OperationFailed;
					return res;
				}
				throw;
			}

			if (detail::IsRetryableSqlState(res.lastFailureSqlState) && attempt < opts.maxAttempts) {
				logger.warning(logPrefix + " Transient DB conflict (" + res.lastFailureSqlState +
				               "), retrying attempt " + std::to_string(attempt + 1) + "/" + std::to_string(opts.maxAttempts));
				uint32_t sleepMs = CalculateRetrySleepMs(backoffMs, opts, rng);
				if (sleepMs > 0) {
					std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
				}
				backoffMs = static_cast<uint32_t>(
					std::min<uint64_t>(static_cast<uint64_t>(backoffMs) * 2, opts.maxBackoffMs));
				continue;
			}

			res.status = detail::FinalStatusForSqlState(res.lastFailureSqlState);
			logger.error(logPrefix + " Failed (" + res.lastFailureSqlState + "): " + res.lastFailureMessage);
			return res;
		}

		return res;
	}

} // namespace OpenWifi
