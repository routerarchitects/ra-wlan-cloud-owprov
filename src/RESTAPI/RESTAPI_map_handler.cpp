//
// Created by stephane bourque on 2021-11-09.
//

#include "RESTAPI_map_handler.h"

#include "Poco/JSON/Parser.h"
#include "Poco/StringTokenizer.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"

namespace OpenWifi {

	void RESTAPI_map_handler::DoGet() {

		ProvObjects::Map Existing;
		std::string UUID = GetBinding(RESTAPI::Protocol::UUID, "");
		if (UUID.empty() || !DB_.GetRecord(RESTAPI::Protocol::ID, UUID, Existing)) {
			return NotFound();
		}
		RBAC::TargetScope scope;
		if (RBAC::ResolveMapScope(UUID, scope)) {
			if (!RBAC::RequireAccess(*this, "map", "READ", scope)) {
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

	void RESTAPI_map_handler::DoDelete() {
		ProvObjects::Map Existing;
		std::string UUID = GetBinding(RESTAPI::Protocol::UUID, "");
		if (UUID.empty() || !DB_.GetRecord(RESTAPI::Protocol::ID, UUID, Existing)) {
			return NotFound();
		}
		RBAC::TargetScope scope;
		if (RBAC::ResolveMapScope(UUID, scope)) {
			if (!RBAC::RequireAccess(*this, "map", "DELETE", scope)) {
				return;
			}
		} else if (!RBAC::IsRootUser(*this)) {
			return UnAuthorized(RESTAPI::Errors::ACCESS_DENIED);
		}

		DB_.DeleteRecord("id", Existing.info.id);
		MoveUsage(StorageService()->PolicyDB(), DB_, Existing.managementPolicy, "",
				  Existing.info.id);
		RemoveMembership(StorageService()->EntityDB(), &ProvObjects::Entity::maps, Existing.entity,
						 Existing.info.id);
		RemoveMembership(StorageService()->VenueDB(), &ProvObjects::Venue::maps, Existing.venue,
						 Existing.info.id);
		return OK();
	}

	static bool ValidateVisibility(const std::string &V) {
		return (V == "private" || V == "public" || V == "select");
	}

	void RESTAPI_map_handler::DoPost() {
		std::string UUID = GetBinding(RESTAPI::Protocol::UUID, "");
		if (UUID.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		const auto &RawObject = ParsedBody_;
		ProvObjects::Map NewObject;
		if (!NewObject.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		RBAC::TargetScope scope;
		if (!NewObject.venue.empty()) {
			ProvObjects::Venue venue;
			if (StorageService()->VenueDB().GetRecord("id", NewObject.venue, venue) &&
				!venue.entity.empty()) {
				scope.entity = venue.entity;
				scope.venue = venue.info.id;
			}
		} else if (!NewObject.entity.empty()) {
			scope.entity = NewObject.entity;
		} else if (RBAC::ResolveManagementPolicyScope(NewObject.managementPolicy, scope)) {
			// scope resolved from the attached policy
		}
		if (!scope.entity.empty() || !scope.venue.empty()) {
			if (!RBAC::RequireAccess(*this, "map", "CREATE", scope)) {
				return;
			}
		} else if (!RBAC::IsRootUser(*this)) {
			return UnAuthorized(RESTAPI::Errors::ACCESS_DENIED);
		}

		if (!CreateObjectInfo(RawObject, UserInfo_.userinfo, NewObject.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		if (!ValidateVisibility(NewObject.visibility)) {
			return BadRequest(RESTAPI::Errors::InvalidVisibilityAttribute);
		}

		if (RawObject->has("entity")) {
			if (!NewObject.entity.empty() &&
				!StorageService()->EntityDB().Exists("id", NewObject.entity))
				return BadRequest(RESTAPI::Errors::EntityMustExist);
		}

		if (RawObject->has("managementPolicy")) {
			if (!NewObject.managementPolicy.empty() &&
				!StorageService()->PolicyDB().Exists("id", NewObject.managementPolicy))
				return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
		}

		NewObject.creator = UserInfo_.userinfo.id;

		if (DB_.CreateRecord(NewObject)) {
			AddMembership(StorageService()->EntityDB(), &ProvObjects::Entity::maps,
						  NewObject.entity, NewObject.info.id);
			AddMembership(StorageService()->VenueDB(), &ProvObjects::Venue::maps, NewObject.venue,
						  NewObject.info.id);
			MoveUsage(StorageService()->PolicyDB(), DB_, "", NewObject.managementPolicy,
					  NewObject.info.id);
			Poco::JSON::Object Answer;
			ProvObjects::Map M;
			DB_.GetRecord("id", NewObject.info.id, M);
			M.to_json(Answer);
			return ReturnObject(Answer);
		}
		InternalError(RESTAPI::Errors::RecordNotCreated);
	}

	void RESTAPI_map_handler::DoPut() {
		ProvObjects::Map Existing;
		std::string UUID = GetBinding(RESTAPI::Protocol::UUID, "");
		if (UUID.empty() || !DB_.GetRecord(RESTAPI::Protocol::ID, UUID, Existing)) {
			return NotFound();
		}
		RBAC::TargetScope scope;
		if (RBAC::ResolveMapScope(UUID, scope)) {
			if (!RBAC::RequireAccess(*this, "map", "UPDATE", scope)) {
				return;
			}
		} else if (!RBAC::IsRootUser(*this)) {
			return UnAuthorized(RESTAPI::Errors::ACCESS_DENIED);
		}

		const auto &RawObject = ParsedBody_;
		ProvObjects::Map NewObject;
		if (!NewObject.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if (!UpdateObjectInfo(RawObject, UserInfo_.userinfo, Existing.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		std::string FromPolicy, ToPolicy;
		if (!CreateMove(RawObject, "managementPolicy", &MapDB::RecordName::managementPolicy,
						Existing, FromPolicy, ToPolicy, StorageService()->PolicyDB()))
			return BadRequest(RESTAPI::Errors::EntityMustExist);

		std::string FromEntity, ToEntity;
		if (!CreateMove(RawObject, "entity", &MapDB::RecordName::entity, Existing, FromEntity,
						ToEntity, StorageService()->EntityDB()))
			return BadRequest(RESTAPI::Errors::EntityMustExist);

		std::string FromVenue, ToVenue;
		if (!CreateMove(RawObject, "venue", &MapDB::RecordName::venue, Existing, FromVenue, ToVenue,
						StorageService()->VenueDB()))
			return BadRequest(RESTAPI::Errors::VenueMustExist);

		AssignIfPresent(RawObject, "data", Existing.data);
		if (RawObject->has("visibility"))
			Existing.visibility = NewObject.visibility;

		if (DB_.UpdateRecord("id", UUID, Existing)) {
			MoveUsage(StorageService()->PolicyDB(), DB_, FromPolicy, ToPolicy, Existing.info.id);
			ManageMembership(StorageService()->EntityDB(), &ProvObjects::Entity::maps, FromEntity,
							 ToEntity, Existing.info.id);
			ManageMembership(StorageService()->VenueDB(), &ProvObjects::Venue::maps, FromVenue,
							 ToVenue, Existing.info.id);

			ProvObjects::Map NewRecord;
			DB_.GetRecord("id", UUID, NewRecord);
			Poco::JSON::Object Answer;
			NewRecord.to_json(Answer);
			return ReturnObject(Answer);
		}
		InternalError(RESTAPI::Errors::RecordNotUpdated);
	}
} // namespace OpenWifi
