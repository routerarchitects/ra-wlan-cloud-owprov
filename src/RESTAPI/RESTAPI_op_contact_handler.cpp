//
// Created by stephane bourque on 2022-04-07.
//

#include "RESTAPI_op_contact_handler.h"
#include "RESTAPI_db_helpers.h"
#include "RESTAPI/RESTAPI_rbac_helpers.h"
#include "sdks/SDK_sec.h"

namespace OpenWifi {

	void RESTAPI_op_contact_handler::DoGet() {
		auto uuid = GetBinding("uuid", "");
		if (uuid.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}
		OpContactDB::RecordName Existing;
		if (!DB_.GetRecord("id", uuid, Existing)) {
			return NotFound();
		}
		if (!RBAC::RequireOperatorAccessOrNotFound(*this, Existing.operatorId, "READ")) {
			return;
		}
		Poco::JSON::Object Answer;
		Existing.to_json(Answer);
		return ReturnObject(Answer);
	}

	void RESTAPI_op_contact_handler::DoDelete() {
		auto uuid = GetBinding("uuid", "");
		if (uuid.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		OpContactDB::RecordName Existing;
		if (!DB_.GetRecord("id", uuid, Existing)) {
			return NotFound();
		}
		if (!RBAC::RequireOperatorAccessOrNotFound(*this, Existing.operatorId, "DELETE")) {
			return;
		}

		// see if anyone is still using this thing
		if (!Existing.subscriberDeviceId.empty()) {
			return BadRequest(RESTAPI::Errors::StillInUse);
		}

		DB_.DeleteRecord("id", uuid);
		return OK();
	}

	void RESTAPI_op_contact_handler::DoPost() {

		const auto &RawObject = ParsedBody_;
		OpContactDB::RecordName NewObject;
		if (!NewObject.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if (!ValidDbId(NewObject.operatorId, StorageService()->OperatorDB(), false,
					   RESTAPI::Errors::InvalidOperatorId, *this) ||
			!ValidDbId(NewObject.managementPolicy, StorageService()->PolicyDB(), true,
					   RESTAPI::Errors::UnknownManagementPolicyUUID, *this) ||
			!ValidDbId(NewObject.subscriberDeviceId, StorageService()->SubscriberDeviceDB(), true,
					   RESTAPI::Errors::InvalidSubscriberDeviceId, *this) ||
			!ValidContactType(NewObject.type, *this)) {
			return;
		}
		if (!RBAC::RequireOperatorAccessOrBadRequest(*this, NewObject.operatorId, "CREATE",
													 RESTAPI::Errors::InvalidOperatorId)) {
			return;
		}

		ProvObjects::CreateObjectInfo(RawObject, UserInfo_.userinfo, NewObject.info);
		return ReturnCreatedObject(DB_, NewObject, *this);
	}

	void RESTAPI_op_contact_handler::DoPut() {
		auto uuid = GetBinding("uuid");
		OpContactDB::RecordName Existing;
		if (uuid.empty() || !DB_.GetRecord("id", uuid, Existing)) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		const auto &RawObject = ParsedBody_;
		OpContactDB::RecordName UpdateObj;
		if (!UpdateObj.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if (!ValidContactType(UpdateObj.type, *this) ||
			!ValidDbId(UpdateObj.managementPolicy, StorageService()->PolicyDB(), true,
					   RESTAPI::Errors::UnknownManagementPolicyUUID, *this) ||
			!ValidDbId(UpdateObj.subscriberDeviceId, StorageService()->SubscriberDeviceDB(), true,
					   RESTAPI::Errors::InvalidSubscriberDeviceId, *this)) {
			return;
		}
		if (!RBAC::RequireOperatorAccessOrNotFound(*this, Existing.operatorId, "MODIFY")) {
			return;
		}

		ProvObjects::UpdateObjectInfo(RawObject, UserInfo_.userinfo, Existing.info);
		AssignIfPresent(RawObject, "type", Existing.type);
		AssignIfPresent(RawObject, "title", Existing.title);
		AssignIfPresent(RawObject, "salutation", Existing.salutation);
		AssignIfPresent(RawObject, "firstname", Existing.firstname);
		AssignIfPresent(RawObject, "lastname", Existing.lastname);
		AssignIfPresent(RawObject, "initials", Existing.initials);
		AssignIfPresent(RawObject, "visual", Existing.visual);
		AssignIfPresent(RawObject, "mobiles", Existing.mobiles);
		AssignIfPresent(RawObject, "phones", Existing.phones);
		AssignIfPresent(RawObject, "accessPIN", Existing.accessPIN);
		AssignIfPresent(RawObject, "secondaryEmail", Existing.secondaryEmail);
		AssignIfPresent(RawObject, "primaryEmail", Existing.primaryEmail);
		AssignIfPresent(RawObject, "subscriberDeviceId", Existing.subscriberDeviceId);
		AssignIfPresent(RawObject, "managementPolicy", Existing.managementPolicy);

		return ReturnUpdatedObject(DB_, Existing, *this);
	}
} // namespace OpenWifi
