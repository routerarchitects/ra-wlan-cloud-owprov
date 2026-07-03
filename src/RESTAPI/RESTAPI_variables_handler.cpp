//
// Created by stephane bourque on 2022-02-23.
//

#include "RESTAPI_variables_handler.h"
#include "RESTAPI_db_helpers.h"

namespace OpenWifi {

	void RESTAPI_variables_handler::DoGet() {
		auto UUID = GetBinding("uuid", "");
		if (UUID.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		VariablesDB::RecordName Existing;
		if (!DB_.GetRecord("id", UUID, Existing)) {
			return NotFound();
		}
		RBAC::TargetScope scope;
		if (RBAC::ResolveVariableScope(UUID, scope)) {
			if (!RBAC::RequireAccess(*this, "variable", "READ", scope)) {
				return;
			}
		} else if (!RBAC::IsRootUser(*this)) {
			return UnAuthorized(RESTAPI::Errors::ACCESS_DENIED);
		}

		Poco::JSON::Object Answer;
		if (QB_.AdditionalInfo)
			AddExtendedInfo(Existing, Answer);
		Existing.to_json(Answer);
		ReturnObject(Answer);
	}

	void RESTAPI_variables_handler::DoDelete() {
		auto UUID = GetBinding("uuid", "");
		if (UUID.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		VariablesDB::RecordName Existing;
		if (!DB_.GetRecord("id", UUID, Existing)) {
			return NotFound();
		}
		RBAC::TargetScope scope;
		if (RBAC::ResolveVariableScope(UUID, scope)) {
			if (!RBAC::RequireAccess(*this, "variable", "DELETE", scope)) {
				return;
			}
		} else if (!RBAC::IsRootUser(*this)) {
			return UnAuthorized(RESTAPI::Errors::ACCESS_DENIED);
		}

		if (!Existing.configurations.empty()) {
			return BadRequest(RESTAPI::Errors::StillInUse);
		}
		MoveUsage(StorageService()->PolicyDB(), DB_, Existing.managementPolicy, "",
				  Existing.info.id);
		RemoveMembership(StorageService()->VenueDB(), &ProvObjects::Venue::variables,
						 Existing.venue, Existing.info.id);
		RemoveMembership(StorageService()->EntityDB(), &ProvObjects::Entity::variables,
						 Existing.entity, Existing.info.id);
		DB_.DeleteRecord("id", UUID);

		return OK();
	}

	void RESTAPI_variables_handler::DoPost() {
		auto UUID = GetBinding("uuid", "");
		if (UUID.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		const auto &RawObj = ParsedBody_;
		VariablesDB::RecordName NewObject;
		if (!NewObject.from_json(RawObj)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		RBAC::TargetScope scope;
		if (!NewObject.entity.empty()) {
			scope.entity = NewObject.entity;
		} else if (!NewObject.venue.empty()) {
			ProvObjects::Venue venue;
			if (StorageService()->VenueDB().GetRecord("id", NewObject.venue, venue) &&
				!venue.entity.empty()) {
				scope.entity = venue.entity;
				scope.venue = venue.info.id;
			}
		} else if (!NewObject.inventory.empty()) {
			RBAC::ResolveInventoryScope(NewObject.inventory, scope);
		} else if (RBAC::ResolveManagementPolicyScope(NewObject.managementPolicy, scope)) {
			// scope resolved from the attached policy
		}
		if (!scope.entity.empty() || !scope.venue.empty()) {
			if (!RBAC::RequireAccess(*this, "variable", "CREATE", scope)) {
				return;
			}
		} else if (!RBAC::IsRootUser(*this)) {
			return UnAuthorized(RESTAPI::Errors::ACCESS_DENIED);
		}

		if (!NewObject.entity.empty() &&
			!StorageService()->EntityDB().Exists("id", NewObject.entity)) {
			return BadRequest(RESTAPI::Errors::EntityMustExist);
		}

		if (!NewObject.venue.empty() &&
			!StorageService()->VenueDB().Exists("id", NewObject.venue)) {
			return BadRequest(RESTAPI::Errors::VenueMustExist);
		}

		if (!NewObject.managementPolicy.empty() &&
			!StorageService()->PolicyDB().Exists("id", NewObject.managementPolicy)) {
			return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
		}

		if (!ProvObjects::CreateObjectInfo(RawObj, UserInfo_.userinfo, NewObject.info)) {
			return BadRequest((RESTAPI::Errors::MissingOrInvalidParameters));
		}

		if (DB_.CreateRecord(NewObject)) {
			MoveUsage(StorageService()->PolicyDB(), DB_, "", NewObject.managementPolicy,
					  NewObject.info.id);
			AddMembership(StorageService()->VenueDB(), &ProvObjects::Venue::variables,
						  NewObject.venue, NewObject.info.id);
			AddMembership(StorageService()->EntityDB(), &ProvObjects::Entity::variables,
						  NewObject.entity, NewObject.info.id);

			VariablesDB::RecordName Added;
			DB_.GetRecord("id", NewObject.info.id, Added);
			Poco::JSON::Object Answer;
			Added.to_json(Answer);
			return ReturnObject(Answer);
		}
		return BadRequest(RESTAPI::Errors::RecordNotCreated);
	}

	void RESTAPI_variables_handler::DoPut() {
		auto UUID = GetBinding("uuid", "");
		if (UUID.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		VariablesDB::RecordName Existing;
		if (!StorageService()->VariablesDB().GetRecord("id", UUID, Existing)) {
			return NotFound();
		}
		RBAC::TargetScope scope;
		if (RBAC::ResolveVariableScope(UUID, scope)) {
			if (!RBAC::RequireAccess(*this, "variable", "UPDATE", scope)) {
				return;
			}
		} else if (!RBAC::IsRootUser(*this)) {
			return UnAuthorized(RESTAPI::Errors::ACCESS_DENIED);
		}

		const auto &RawObject = ParsedBody_;
		VariablesDB::RecordName NewObj;
		if (!NewObj.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if (!ProvObjects::UpdateObjectInfo(RawObject, UserInfo_.userinfo, Existing.info)) {
			return BadRequest((RESTAPI::Errors::MissingOrInvalidParameters));
		}

		if (RawObject->has("variables"))
			Existing.variables = NewObj.variables;

		std::string FromPolicy, ToPolicy;
		if (!CreateMove(RawObject, "managementPolicy", &VariablesDB::RecordName::managementPolicy,
						Existing, FromPolicy, ToPolicy, StorageService()->PolicyDB()))
			return BadRequest(RESTAPI::Errors::EntityMustExist);

		std::string FromEntity, ToEntity;
		if (!CreateMove(RawObject, "entity", &VariablesDB::RecordName::entity, Existing, FromEntity,
						ToEntity, StorageService()->EntityDB()))
			return BadRequest(RESTAPI::Errors::EntityMustExist);

		std::string FromVenue, ToVenue;
		if (!CreateMove(RawObject, "venue", &VariablesDB::RecordName::venue, Existing, FromVenue,
						ToVenue, StorageService()->VenueDB()))
			return BadRequest(RESTAPI::Errors::VenueMustExist);

		if (DB_.UpdateRecord("id", UUID, Existing)) {
			MoveUsage(StorageService()->PolicyDB(), DB_, FromPolicy, ToPolicy, Existing.info.id);
			ManageMembership(StorageService()->VenueDB(), &ProvObjects::Venue::variables, FromVenue,
							 ToVenue, Existing.info.id);
			ManageMembership(StorageService()->EntityDB(), &ProvObjects::Entity::variables,
							 FromEntity, ToEntity, Existing.info.id);

			VariablesDB::RecordName Added;
			DB_.GetRecord("id", NewObj.info.id, Added);
			Poco::JSON::Object Answer;
			Added.to_json(Answer);
			return ReturnObject(Answer);
		}
		return BadRequest(RESTAPI::Errors::RecordNotCreated);
	}

} // namespace OpenWifi
