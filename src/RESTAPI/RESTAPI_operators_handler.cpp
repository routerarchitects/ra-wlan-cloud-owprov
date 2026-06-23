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
#include "RESTAPI/RESTAPI_rbac_helpers.h"
#include "framework/orm.h"
#include "framework/MicroServiceFuncs.h"
#include "framework/utils.h"

namespace OpenWifi {
	namespace {
		bool FindDefaultOperatorId(std::string &operatorId) {
			operatorId.clear();
			StorageService()->OperatorDB().Iterate([&](const ProvObjects::Operator &op) {
				if (op.defaultOperator) {
					operatorId = op.info.id;
					return false;
				}
				return true;
			});
			return !operatorId.empty();
		}

		bool ResolveOperatorCreateParent(RESTAPIHandler &handler,
										 const Poco::JSON::Object::Ptr &rawObj,
										 std::string &parentEntityId,
										 std::string &parentOperatorId) {
			parentEntityId.clear();
			parentOperatorId.clear();

			if (!rawObj || !rawObj->has("entityId")) {
				if (handler.UserInfo_.userinfo.userRole == SecurityObjects::ROOT) {
					parentEntityId = EntityDB::RootUUID();
					FindDefaultOperatorId(parentOperatorId);
					return true;
				}
				handler.BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
				return false;
			}

			parentEntityId = rawObj->getValue<std::string>("entityId");
			ProvObjects::Entity parentEntity;
			if (parentEntityId.empty() ||
				!StorageService()->EntityDB().GetRecord("id", parentEntityId, parentEntity)) {
				handler.BadRequest(RESTAPI::Errors::ParentUUIDMustExist);
				return false;
			}

			if (parentEntity.operatorId.empty() ||
				!StorageService()->OperatorDB().Exists("id", parentEntity.operatorId)) {
				if (parentEntity.info.id != EntityDB::RootUUID() ||
					!FindDefaultOperatorId(parentOperatorId)) {
					handler.BadRequest(RESTAPI::Errors::EntityMustExist);
					return false;
				}
			} else {
				parentOperatorId = parentEntity.operatorId;
			}

			if (handler.UserInfo_.userinfo.userRole != SecurityObjects::ROOT &&
				!RBAC::RequireAccess(handler, "operator", "CREATE",
									 RBAC::TargetScope{parentEntityId, ""})) {
				return false;
			}

			return true;
		}

		bool SeedChildOperatorAccess(const SecurityObjects::UserInfo &creator,
									 const ProvObjects::Operator &createdOperator,
									 const ProvObjects::Entity &createdEntity) {
			if (creator.id.empty() || creator.userRole == SecurityObjects::ROOT) {
				return true;
			}

			const auto creatorUserId = creator.id;

			ProvObjects::ManagementPolicy policy;
			ProvObjects::CreateObjectInfo(creator, policy.info);
			policy.info.name = "Auto access policy for " + createdOperator.info.name;
			policy.entity = createdEntity.info.id;
			policy.venue.clear();
			policy.inUse.clear();

			ProvObjects::ManagementPolicyEntry policyEntry;
			policyEntry.users = {creatorUserId};
			policyEntry.resources = {"entity", "operator", "venue", "inventory",
									 "managementPolicy", "managementRole",
									 "configuration", "contact", "location"};
			policyEntry.access = {"FULL"};
			policy.entries = {policyEntry};

			if (!StorageService()->PolicyDB().CreateRecord(policy)) {
				return false;
			}

			if (!StorageService()->EntityDB().ManipulateVectorMember(
					&ProvObjects::Entity::managementPolicies, "id", createdEntity.info.id,
					policy.info.id, true)) {
				StorageService()->PolicyDB().DeleteRecord("id", policy.info.id);
				return false;
			}

			ProvObjects::ManagementRole role;
			ProvObjects::CreateObjectInfo(creator, role.info);
			role.info.name = "Auto access role for " + createdOperator.info.name;
			role.managementPolicy = policy.info.id;
			role.users = {creatorUserId};
			role.entity = createdEntity.info.id;
			role.venue.clear();
			role.inUse.clear();

			if (!StorageService()->RolesDB().CreateRecord(role)) {
				StorageService()->EntityDB().ManipulateVectorMember(
					&ProvObjects::Entity::managementPolicies, "id", createdEntity.info.id,
					policy.info.id, false);
				StorageService()->PolicyDB().DeleteRecord("id", policy.info.id);
				return false;
			}

			if (!StorageService()->EntityDB().ManipulateVectorMember(
					&ProvObjects::Entity::managementRoles, "id", createdEntity.info.id,
					role.info.id, true)) {
				StorageService()->RolesDB().DeleteRecord("id", role.info.id);
				StorageService()->EntityDB().ManipulateVectorMember(
					&ProvObjects::Entity::managementPolicies, "id", createdEntity.info.id,
					policy.info.id, false);
				StorageService()->PolicyDB().DeleteRecord("id", policy.info.id);
				return false;
			}

			return true;
		}
	} // namespace

	void RESTAPI_operators_handler::DoGet() {
		auto uuid = GetBinding("uuid", "");
		if (uuid.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		OperatorDB::RecordName Existing;
		if (!DB_.GetRecord("id", uuid, Existing)) {
			return NotFound();
		}
		if (!RBAC::RequireAccess(*this, "operator", "READ",
								 RBAC::TargetScope{Existing.entityId, ""})) {
			return;
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
		if (!RBAC::RequireAccess(*this, "operator", "DELETE",
								 RBAC::TargetScope{Existing.entityId, ""})) {
			return;
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

		std::string requestedParentEntity;
		std::string parentOperatorId;
		if (!ResolveOperatorCreateParent(*this, RawObject, requestedParentEntity,
										 parentOperatorId)) {
			return;
		}

		if (!ProvObjects::CreateObjectInfo(RawObject, UserInfo_.userinfo, NewObject.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		ProvObjects::Entity NewEntity;
		ProvObjects::CreateObjectInfo(UserInfo_.userinfo, NewEntity.info);
		NewEntity.info.name = "Operator-entity:" + NewObject.info.name;
		NewEntity.info.description = NewObject.info.description;
		NewEntity.parent = requestedParentEntity;
		NewEntity.operatorId = NewObject.info.id;
		NewObject.entityId = NewEntity.info.id;
		NewObject.parentOperatorId = parentOperatorId;

		if (!StorageService()->EntityDB().CreateRecord(NewEntity)) {
			return InternalError(RESTAPI::Errors::RecordNotCreated);
		}
		StorageService()->EntityDB().AddChild("id", NewEntity.parent, NewEntity.info.id);

		if (DB_.CreateRecord(NewObject)) {
			if (!SeedChildOperatorAccess(UserInfo_.userinfo, NewObject, NewEntity)) {
				StorageService()->EntityDB().DeleteChild("id", NewEntity.parent, NewEntity.info.id);
				StorageService()->EntityDB().DeleteRecord("id", NewEntity.info.id);
				DB_.DeleteRecord("id", NewObject.info.id);
				return InternalError(RESTAPI::Errors::RecordNotCreated);
			}

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
		if (!RBAC::RequireAccess(*this, "operator", "MODIFY",
								 RBAC::TargetScope{Existing.entityId, ""})) {
			return;
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
