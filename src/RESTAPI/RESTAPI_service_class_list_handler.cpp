//
// Created by stephane bourque on 2022-04-06.
//

#include "RESTAPI_service_class_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTAPI/RESTAPI_rbac_helpers.h"

namespace OpenWifi {
	void RESTAPI_service_class_list_handler::DoGet() {
		auto operatorId = GetParameter("operatorId");

		if (operatorId.empty() || !StorageService()->OperatorDB().Exists("id", operatorId)) {
			return BadRequest(RESTAPI::Errors::OperatorIdMustExist);
		}
		ProvObjects::Operator Operator;
		if (!StorageService()->OperatorDB().GetRecord("id", operatorId, Operator)) {
			return BadRequest(RESTAPI::Errors::OperatorIdMustExist);
		}
		if (!RBAC::RequireAccess(*this, "operator", "LIST",
								 RBAC::TargetScope{Operator.entityId, ""})) {
			return;
		}
		return ListHandlerForOperator<ServiceClassDB>("serviceClasses", DB_, *this, operatorId);
	}
} // namespace OpenWifi
