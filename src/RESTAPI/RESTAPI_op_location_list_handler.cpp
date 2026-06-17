//
// Created by stephane bourque on 2022-04-07.
//

#include "RESTAPI_op_location_list_handler.h"
#include "RESTAPI_db_helpers.h"
#include "RESTAPI/RESTAPI_rbac_helpers.h"

namespace OpenWifi {
	void RESTAPI_op_location_list_handler::DoGet() {
		auto operatorId = GetParameter("operatorId");

		if (operatorId.empty()) {
			return BadRequest(RESTAPI::Errors::OperatorIdMustExist);
		}
		if (!RBAC::RequireOperatorAccessOrBadRequest(*this, operatorId, "LIST",
													 RESTAPI::Errors::OperatorIdMustExist)) {
			return;
		}
		return ListHandlerForOperator<OpLocationDB>("locations", DB_, *this, operatorId);
	}

} // namespace OpenWifi
