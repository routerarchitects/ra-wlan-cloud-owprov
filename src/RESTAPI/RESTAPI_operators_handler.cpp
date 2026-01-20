/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
//
// Created by stephane bourque on 2022-04-06.
//

#include "RESTAPI_operators_handler.h"
#include "RESTAPI_db_helpers.h"
#include "framework/orm.h"
#include "framework/utils.h"

namespace OpenWifi {

	void RESTAPI_operators_handler::DoGet() {
		auto uuid = GetBinding("uuid", "");
		if (uuid.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		OperatorDB::RecordName Existing;
		if (!DB_.GetRecord("id", uuid, Existing)) {
			return NotFound();
		}

		Poco::JSON::Object Answer;
		Existing.to_json(Answer);
		return ReturnObject(Answer);
	}

	void RESTAPI_operators_handler::DoDelete() {
		auto uuid = GetBinding("uuid", "");
		if (uuid.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		OperatorDB::RecordName Existing;
		if (!DB_.GetRecord("id", uuid, Existing)) {
			return NotFound();
		}

		if (Existing.defaultOperator) {
			return BadRequest(RESTAPI::Errors::CannotDeleteDefaultOperator);
		}

		//  Let's see if there are any subscribers in this operator
		auto Count =
			StorageService()->SubscriberDeviceDB().Count(fmt::format(" operatorId='{}'", uuid));
		if (Count > 0) {
			return BadRequest(RESTAPI::Errors::StillInUse);
		}

		DB_.DeleteRecord("id", uuid);
		StorageService()->ServiceClassDB().DeleteRecords(fmt::format(" operatorId='{}'", uuid));
		return OK();
	}

	void RESTAPI_operators_handler::DoPost() {

		const auto &RawObject = ParsedBody_;
		ProvObjects::Operator NewObject;
		if (!NewObject.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if (NewObject.defaultOperator) {
			return BadRequest(RESTAPI::Errors::CannotCreateDefaultOperator);
		}

		if ((RawObject->has("deviceRules") && !ValidDeviceRules(NewObject.deviceRules, *this))) {
			return;
		}

		if (RawObject->has("managementPolicy") &&
			!StorageService()->PolicyDB().Exists("id", NewObject.managementPolicy)) {
			return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
		}

		if (!ValidSourceIP(NewObject.sourceIP)) {
			return BadRequest(RESTAPI::Errors::InvalidIPAddresses);
		}

		Poco::toLowerInPlace(NewObject.registrationId);
		if (NewObject.registrationId.empty() ||
			DB_.Exists("registrationId", NewObject.registrationId)) {
			return BadRequest(RESTAPI::Errors::InvalidRegistrationOperatorName);
		}

		if (RawObject->has("entityId")) {
			if (NewObject.entityId.empty() ||
				!StorageService()->EntityDB().Exists("id", NewObject.entityId)) {
				return BadRequest(RESTAPI::Errors::EntityMustExist);
			}

			auto EscapedEntityId = ORM::Escape(NewObject.entityId);
			if (DB_.Count(" entityId='" + EscapedEntityId + "' ") > 0) {
				return BadRequest(RESTAPI::Errors::EntityAlreadyHasOperator);
			}
		}

		ProvObjects::CreateObjectInfo(RawObject, UserInfo_.userinfo, NewObject.info);
		if (DB_.CreateRecord(NewObject)) {

			// Create the default service...
			ProvObjects::ServiceClass DefSer;
			DefSer.info.id = MicroServiceCreateUUID();
			DefSer.info.name = "Default Service Class";
			DefSer.defaultService = true;
			DefSer.info.created = DefSer.info.modified = Utils::Now();
			DefSer.operatorId = NewObject.info.id;
			DefSer.period = "monthly";
			DefSer.billingCode = "basic";
			DefSer.currency = "USD";
			StorageService()->ServiceClassDB().CreateRecord(DefSer);

			ProvObjects::Operator New;
			DB_.GetRecord("id", NewObject.info.id, New);
			Poco::JSON::Object Answer;
			New.to_json(Answer);
			return ReturnObject(Answer);
		}

		return InternalError(RESTAPI::Errors::RecordNotCreated);
	}

	void RESTAPI_operators_handler::DoPut() {
		auto uuid = GetBinding("uuid", "");
		if (uuid.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		ProvObjects::Operator Existing;
		if (!DB_.GetRecord("id", uuid, Existing)) {
			return NotFound();
		}

		const auto &RawObject = ParsedBody_;
		ProvObjects::Operator UpdatedObj;
		if (!UpdatedObj.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if ((RawObject->has("deviceRules") && !ValidDeviceRules(UpdatedObj.deviceRules, *this))) {
			return;
		}

		if (RawObject->has("managementPolicy")) {
			if (!StorageService()->PolicyDB().Exists("id", UpdatedObj.managementPolicy)) {
				return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
			}
			Existing.managementPolicy = UpdatedObj.managementPolicy;
		}

		ProvObjects::UpdateObjectInfo(RawObject, UserInfo_.userinfo, Existing.info);

		if (RawObject->has("variables")) {
			Existing.variables = UpdatedObj.variables;
		}

		if (RawObject->has("sourceIP")) {
			if (!UpdatedObj.sourceIP.empty() && !ValidSourceIP(UpdatedObj.sourceIP)) {
				return BadRequest(RESTAPI::Errors::InvalidIPAddresses);
			}
			Existing.sourceIP = UpdatedObj.sourceIP;
		}

		if (RawObject->has("deviceRules"))
			Existing.deviceRules = UpdatedObj.deviceRules;

		if (RawObject->has("entityId")) {
			if (UpdatedObj.entityId.empty() ||
				!StorageService()->EntityDB().Exists("id", UpdatedObj.entityId)) {
				return BadRequest(RESTAPI::Errors::EntityMustExist);
			}
			auto EscapedEntityId = ORM::Escape(UpdatedObj.entityId);
			auto EscapedId = ORM::Escape(Existing.info.id);
			if (DB_.Count(" entityId='" + EscapedEntityId + "' and id!='" + EscapedId + "' ") >
				0) {
				return BadRequest(RESTAPI::Errors::EntityAlreadyHasOperator);
			}
			Existing.entityId = UpdatedObj.entityId;
		}

		return ReturnUpdatedObject(DB_, Existing, *this);
	}

} // namespace OpenWifi
