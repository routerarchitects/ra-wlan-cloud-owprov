#include "RESTAPI_sub_devices_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "StorageService.h"
#include "fmt/format.h"
#include <set>
#include <vector>

namespace OpenWifi {

	void RESTAPI_sub_devices_list_handler::DoGet() {
		auto operatorId = ORM::Escape(GetParameter("operatorId"));
		auto subscriberId = ORM::Escape(GetParameter("subscriberId"));

		if (!operatorId.empty() && !StorageService()->OperatorDB().Exists("id", operatorId)) {
			return BadRequest(RESTAPI::Errors::OperatorIdMustExist);
		}

		if (!operatorId.empty() && subscriberId.empty()) {
			std::string operatorEntityId = operatorId;
			ProvObjects::Entity E;
			if (StorageService()->EntityDB().GetRecord("operatorId", operatorId, E)) {
				operatorEntityId = E.info.id;
			} else if (StorageService()->EntityDB().GetRecord("id", operatorId, E)) {
				operatorEntityId = E.info.id;
			}

			std::string WhereClause = fmt::format(
				" operatorId='{}' or operatorId='{}' or subscriberId in (select subscriber from venues where subscriber!='' and (entity='{}' or entity='{}')) ",
				operatorId, operatorEntityId, operatorEntityId, operatorId);

			if (QB_.CountOnly) {
				auto Count = DB_.Count(WhereClause);
				return ReturnCountOnly(Count);
			}

			if (!QB_.Select.empty()) {
				return ReturnRecordList<SubscriberDeviceDB, ProvObjects::SubscriberDevice>("subscriberDevices", DB_, *this);
			}

			SubscriberDeviceDB::RecordVec Entries;
			DB_.GetRecords(QB_.Offset, QB_.Limit, Entries, WhereClause);
			return MakeJSONObjectArray("subscriberDevices", Entries, *this);
		}

		return ListHandlerForOperator<SubscriberDeviceDB>("subscriberDevices", DB_, *this,
														  operatorId, subscriberId);
	}

} // namespace OpenWifi