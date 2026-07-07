//
// Created by stephane bourque on 2022-04-06.
//

#include "RESTAPI_sub_devices_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTAPI/RESTAPI_rbac_helpers.h"

namespace OpenWifi {

	void RESTAPI_sub_devices_list_handler::DoGet() {
		auto operatorId = ORM::Escape(GetParameter("operatorId"));
		auto subscriberId = ORM::Escape(GetParameter("subscriberId"));

		if (!operatorId.empty() && !StorageService()->OperatorDB().Exists("id", operatorId)) {
			return BadRequest(RESTAPI::Errors::OperatorIdMustExist);
		}

		std::vector<SubscriberDeviceDB::RecordName> entries;
		auto matchesFilters = [&](const SubscriberDeviceDB::RecordName &record) {
			if (!operatorId.empty() && record.operatorId != operatorId) {
				return false;
			}
			if (!subscriberId.empty() && record.subscriberId != subscriberId) {
				return false;
			}
			return true;
		};

		auto allow = [&](const SubscriberDeviceDB::RecordName &record) {
			RBAC::TargetScope scope;
			if (!RBAC::ResolveSubscriberDeviceScope(record, scope)) {
				return RBAC::IsRootUser(*this);
			}
			return RBAC::HasAccess(*this, "subscriberDevice", "LIST", scope) ||
				   RBAC::HasAccess(*this, "subscriberDevice", "READ", scope);
		};

		if (!QB_.Select.empty()) {
			for (const auto &id : SelectedRecords()) {
				SubscriberDeviceDB::RecordName record;
				if (DB_.GetRecord("id", id, record) && matchesFilters(record) && allow(record)) {
					entries.push_back(record);
				}
			}
		} else {
			DB_.Iterate([&](const SubscriberDeviceDB::RecordName &record) {
				if (matchesFilters(record) && allow(record)) {
					entries.push_back(record);
				}
				return true;
			});
		}

		if (QB_.CountOnly) {
			return ReturnCountOnly(entries.size());
		}

		return MakeJSONObjectArray("subscriberDevices",
								   RESTAPI::ApplyPagination(entries, QB_.Offset, QB_.Limit),
								   *this);
	}

} // namespace OpenWifi
