#include "RESTAPI_sub_devices_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "StorageService.h"
#include "fmt/format.h"
#include <set>
#include <vector>

namespace OpenWifi {

	void RESTAPI_sub_devices_list_handler::DoGet() {
		// --- Subscriber self-care path (subscriber_rbac.md sec 6.1) ---
		// Collection scoped to CallerId; operatorId param is accepted but ignored.
		if (UserInfo_.userinfo.userRole == SecurityObjects::USER_ROLE::SUBSCRIBER) {
			const std::string callerId = UserInfo_.userinfo.id;
			SubscriberDeviceDB::RecordVec entries;
			std::string whereClause = fmt::format(" subscriberId='{}' ", ORM::Escape(callerId));
			bool querySucceeded = false;
			DB_.GetRecords(QB_.Offset, QB_.Limit, entries, whereClause, "", &querySucceeded);
			if (!querySucceeded)
				return InternalError(RESTAPI::Errors::InternalError);
			return ReturnObject("subscriberDevices", entries);
		}

		// --- Administrative / Inter-service path ---
		auto subscriberId = ORM::Escape(GetParameter("subscriberId"));
		auto operatorId = ORM::Escape(GetParameter("operatorId"));

		if (!subscriberId.empty()) {
			std::string whereClause = fmt::format(" subscriberId='{}' ", subscriberId);
			if (!operatorId.empty()) {
				whereClause += fmt::format(" and operatorId='{}' ", operatorId);
			}
			if (QB_.CountOnly) {
				auto C = DB_.Count(whereClause);
				return ReturnCountOnly(C);
			}
			SubscriberDeviceDB::RecordVec entries;
			DB_.GetRecords(QB_.Offset, QB_.Limit, entries, whereClause, "");
			return ReturnObject("subscriberDevices", entries);
		}

		if (!operatorId.empty() && !StorageService()->OperatorDB().Exists("id", operatorId)) {
			return BadRequest(RESTAPI::Errors::OperatorIdMustExist);
		}

		return ListHandlerForOperator<SubscriberDeviceDB>("subscriberDevices", DB_, *this,
														  operatorId, subscriberId);
	}

} // namespace OpenWifi
