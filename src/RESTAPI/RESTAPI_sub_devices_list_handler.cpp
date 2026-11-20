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
		if (IsSubscriber()) {
			const std::string callerId = UserInfo_.userinfo.id;
			SubscriberDeviceDB::RecordVec entries;
			std::string whereClause = fmt::format(" subscriberId='{}' ", ORM::Escape(callerId));
			bool querySucceeded = false;
			DB_.GetRecords(QB_.Offset, QB_.Limit, entries, whereClause, "", &querySucceeded);
			if (!querySucceeded)
				return InternalError(RESTAPI::Errors::InternalError);
			return ReturnObject("subscriberDevices", entries);
		}

		// --- Administrative / Inter-service path (restored to upstream main) ---
		auto operatorId = ORM::Escape(GetParameter("operatorId"));
		auto subscriberId = ORM::Escape(GetParameter("subscriberId"));

		if (!operatorId.empty() && !StorageService()->OperatorDB().Exists("id", operatorId)) {
			return BadRequest(RESTAPI::Errors::OperatorIdMustExist);
		}

		return ListHandlerForOperator<SubscriberDeviceDB>("subscriberDevices", DB_, *this,
														  operatorId, subscriberId);
	}

} // namespace OpenWifi
