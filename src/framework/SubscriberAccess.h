#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace OpenWifi::SubscriberAccess {

	// Tier 1 capabilities from requirements/subscriber_rbac.md. Deferred endpoints
	// stay closed until their handler-level ownership and mutation guards are ready.
	enum class Operation { Denied, ListDevices, ReadDevice, ClaimDevice, DeleteDevice, DeleteAccount };

	inline Operation Resolve(const std::string &method, const std::string &path,
							 const std::string &callerId, std::string &targetId) {
		targetId.clear();
		if (callerId.empty())
			return Operation::Denied;
		if (path == "/api/v1/subscriberDevice")
			return method == "GET" ? Operation::ListDevices : Operation::Denied;
		if (path == "/api/v1/subscriber")
			return method == "DELETE" ? Operation::DeleteAccount : Operation::Denied;

		const std::string devicePrefix = "/api/v1/subscriberDevice/";
		const std::string subscriberPrefix = "/api/v1/subscriber/";
		if (path.compare(0, devicePrefix.size(), devicePrefix) == 0) {
			targetId = path.substr(devicePrefix.size());
			if (targetId.empty() || targetId.find('/') != std::string::npos)
				return Operation::Denied;
			if (method == "GET")
				return Operation::ReadDevice;
			if (method == "DELETE")
				return Operation::DeleteDevice;
			if (method == "POST" && targetId == "0")
				return Operation::ClaimDevice;
		} else if (path.compare(0, subscriberPrefix.size(), subscriberPrefix) == 0) {
			targetId = path.substr(subscriberPrefix.size());
			if (method == "DELETE" && targetId == callerId)
				return Operation::DeleteAccount;
		}
		return Operation::Denied;
	}

	inline bool IsClaimFieldAllowed(const std::string &field) {
		return field == "name" || field == "description" || field == "serialNumber" ||
			   field == "subscriberId" || field == "operatorId" || field == "deviceGroup" ||
			   field == "configuration" || field == "deviceConfiguration";
	}

	// Return deletion candidates only after every owned venue has been checked.
	// Query status is independent of whether the final page contains records.
	template <typename DB>
	bool CollectEmptyVenues(DB &db, const std::string &where,
		std::vector<std::pair<std::string, std::string>> &venues) {
		venues.clear();
		std::vector<std::pair<std::string, std::string>> checked;
		constexpr uint64_t pageSize = 100;
		for (uint64_t offset = 0;; offset += pageSize) {
			typename DB::RecordVec batch;
			bool querySucceeded = false;
			db.GetRecords(offset, pageSize, batch, where, " ORDER BY id ", &querySucceeded);
			if (!querySucceeded)
				return false;
			for (const auto &venue : batch) {
				if (!venue.devices.empty())
					return false;
				checked.emplace_back(venue.info.id, venue.entity);
			}
			if (batch.size() < pageSize) {
				venues = std::move(checked);
				return true;
			}
		}
	}

} // namespace OpenWifi::SubscriberAccess
