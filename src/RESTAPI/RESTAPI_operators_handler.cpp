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
			StorageService()->SignupDB().Count(fmt::format(" operatorId='{}'", uuid));
		if (Count > 0) {
			return BadRequest(RESTAPI::Errors::StillInUse);
		}

		ProvObjects::Entity LinkedEntity;
		bool HaveLinkedEntity = false;

		if (!Existing.entityId.empty()) {
			HaveLinkedEntity =
				StorageService()->EntityDB().GetRecord("id", Existing.entityId, LinkedEntity);
		}

		if (!HaveLinkedEntity) {
			const auto where = fmt::format(" operatorId='{}' ", ORM::Escape(Existing.info.id));
			HaveLinkedEntity = StorageService()->EntityDB().GetRecord(LinkedEntity, where);
		}

		if (HaveLinkedEntity) {
			if (LinkedEntity.info.id == EntityDB::RootUUID()) {
				return BadRequest(RESTAPI::Errors::StillInUse);
			}

			if (!LinkedEntity.operatorId.empty() && LinkedEntity.operatorId != Existing.info.id) {
				return BadRequest(RESTAPI::Errors::StillInUse);
			}

			if (!LinkedEntity.children.empty() || !LinkedEntity.devices.empty() ||
				!LinkedEntity.venues.empty() || !LinkedEntity.locations.empty() ||
				!LinkedEntity.contacts.empty() || !LinkedEntity.configurations.empty()) {
				return BadRequest(RESTAPI::Errors::StillInUse);
			}

			MoveUsage(StorageService()->PolicyDB(), StorageService()->EntityDB(),
					  LinkedEntity.managementPolicy, "", LinkedEntity.info.id);
			StorageService()->EntityDB().DeleteRecord("id", LinkedEntity.info.id);
			StorageService()->EntityDB().DeleteChild("id", LinkedEntity.parent,
													 LinkedEntity.info.id);
		}

		if (!DB_.DeleteRecord("id", uuid)) {
			return InternalError(RESTAPI::Errors::CouldNotBeDeleted);
		}
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
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
		}

		if (!ProvObjects::CreateObjectInfo(RawObject, UserInfo_.userinfo, NewObject.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		ProvObjects::Entity NewEntity;
		ProvObjects::CreateObjectInfo(UserInfo_.userinfo, NewEntity.info);
		NewEntity.info.name = "Operator-entity:" + NewObject.info.name;
		NewEntity.info.description = NewObject.info.description;
		NewEntity.parent = EntityDB::RootUUID();
		NewEntity.operatorId = NewObject.info.id;
		NewObject.entityId = NewEntity.info.id;

		if (!StorageService()->EntityDB().CreateRecord(NewEntity)) {
			return InternalError(RESTAPI::Errors::RecordNotCreated);
		}
		StorageService()->EntityDB().AddChild("id", NewEntity.parent, NewEntity.info.id);

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

		StorageService()->EntityDB().DeleteChild("id", NewEntity.parent, NewEntity.info.id);
		StorageService()->EntityDB().DeleteRecord("id", NewEntity.info.id);
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
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
		}

		return ReturnUpdatedObject(DB_, Existing, *this);
	}

} // namespace OpenWifi
