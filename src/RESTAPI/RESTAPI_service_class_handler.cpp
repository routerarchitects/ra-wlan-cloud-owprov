//
// Created by stephane bourque on 2022-04-06.
//

#include "RESTAPI_service_class_handler.h"
#include "RESTAPI_db_helpers.h"
#include "RESTAPI/RESTAPI_rbac_helpers.h"

namespace OpenWifi {

	void RESTAPI_service_class_handler::DoGet() {
		auto uuid = GetBinding("uuid", "");
		if (uuid.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}
		ServiceClassDB::RecordName Existing;
		if (!DB_.GetRecord("id", uuid, Existing)) {
			return NotFound();
		}
		ProvObjects::Operator Operator;
		if (!StorageService()->OperatorDB().GetRecord("id", Existing.operatorId, Operator)) {
			return NotFound();
		}
		if (!RBAC::RequireAccess(*this, "operator", "READ",
								 RBAC::TargetScope{Operator.entityId, ""})) {
			return;
		}
		Poco::JSON::Object Answer;
		Existing.to_json(Answer);
		return ReturnObject(Answer);
	}

	void RESTAPI_service_class_handler::DoDelete() {
		auto uuid = GetBinding("uuid", "");
		if (uuid.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}
		ServiceClassDB::RecordName Existing;
		if (!DB_.GetRecord("id", uuid, Existing)) {
			return NotFound();
		}
		ProvObjects::Operator Operator;
		if (!StorageService()->OperatorDB().GetRecord("id", Existing.operatorId, Operator)) {
			return NotFound();
		}
		if (!RBAC::RequireAccess(*this, "operator", "DELETE",
								 RBAC::TargetScope{Operator.entityId, ""})) {
			return;
		}

		// see if anyone is still using this thing
		auto Count =
			StorageService()->SubscriberDeviceDB().Count(fmt::format(" serviceClass='{}' ", uuid));
		if (Count > 0) {
			return BadRequest(RESTAPI::Errors::StillInUse);
		}
		DB_.DeleteRecord("id", uuid);
		return OK();
	}

	void RESTAPI_service_class_handler::DoPost() {
		const auto &RawObject = ParsedBody_;
		ProvObjects::ServiceClass NewObject;
		if (!NewObject.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if (NewObject.operatorId.empty() ||
			!StorageService()->OperatorDB().Exists("id", NewObject.operatorId)) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}
		ProvObjects::Operator Operator;
		if (!StorageService()->OperatorDB().GetRecord("id", NewObject.operatorId, Operator)) {
			return BadRequest(RESTAPI::Errors::InvalidOperatorId);
		}
		if (!RBAC::RequireAccess(*this, "operator", "CREATE",
								 RBAC::TargetScope{Operator.entityId, ""})) {
			return;
		}

		ProvObjects::CreateObjectInfo(RawObject, UserInfo_.userinfo, NewObject.info);

		if (RawObject->has("managementPolicy") &&
			!StorageService()->PolicyDB().Exists("id", NewObject.managementPolicy)) {
			return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
		}

		if (NewObject.billingCode.empty()) {
			return BadRequest(RESTAPI::Errors::InvalidBillingCode);
		}

		auto DefaultCount = DB_.Count(
			fmt::format(" defaultService=true and operatorId='{}' ", NewObject.operatorId));
		if (DefaultCount == 0)
			NewObject.defaultService = true;
		else
			NewObject.defaultService = false;

		if (NewObject.period.empty())
			NewObject.period = "monthly";

		if (!ValidPeriod(NewObject.period)) {
			return BadRequest(RESTAPI::Errors::InvalidBillingPeriod);
		}

		return ReturnCreatedObject(DB_, NewObject, *this);
	}

	void RESTAPI_service_class_handler::DoPut() {
		auto uuid = GetBinding("uuid", "");

		ProvObjects::ServiceClass Existing;
		if (uuid.empty() || !DB_.GetRecord("id", uuid, Existing)) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		const auto &RawObject = ParsedBody_;
		ProvObjects::ServiceClass UpdateObj;
		if (!UpdateObj.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if (RawObject->has("managementPolicy") &&
			!StorageService()->PolicyDB().Exists("id", UpdateObj.managementPolicy)) {
			return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
		}
		ProvObjects::Operator Operator;
		if (!StorageService()->OperatorDB().GetRecord("id", Existing.operatorId, Operator)) {
			return NotFound();
		}
		if (!RBAC::RequireAccess(*this, "operator", "MODIFY",
								 RBAC::TargetScope{Operator.entityId, ""})) {
			return;
		}

		ProvObjects::UpdateObjectInfo(RawObject, UserInfo_.userinfo, Existing.info);
		AssignIfPresent(RawObject, "cost", Existing.cost);
		AssignIfPresent(RawObject, "currency", Existing.currency);
		if (RawObject->has("billingCode") && UpdateObj.billingCode.empty()) {
			return BadRequest(RESTAPI::Errors::InvalidBillingCode);
		}
		AssignIfPresent(RawObject, "billingCode", Existing.billingCode);

		if (RawObject->has("variables")) {
			Existing.variables = UpdateObj.variables;
		}

		return ReturnUpdatedObject(DB_, Existing, *this);
	}
} // namespace OpenWifi
