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

		return ListHandlerForOperator<SubscriberDeviceDB>("subscriberDevices", DB_, *this,
														  operatorId, subscriberId);
	}

} // namespace OpenWifi