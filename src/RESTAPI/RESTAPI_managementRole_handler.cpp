//
// Created by stephane bourque on 2021-08-26.
//

#include "RESTAPI_managementRole_handler.h"

#include "Poco/JSON/Parser.h"
#include "Poco/StringTokenizer.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"

namespace OpenWifi {

	void RESTAPI_managementRole_handler::DoGet() {
		ProvObjects::ManagementRole Existing;
		std::string UUID = GetBinding(RESTAPI::Protocol::ID, "");
		if (UUID.empty() || !DB_.GetRecord(RESTAPI::Protocol::ID, UUID, Existing)) {
			return NotFound();
		}

		Poco::JSON::Object Answer;
		std::string Arg;
		if (HasParameter("expandInUse", Arg) && Arg == "true") {
			Storage::ExpandedListMap M;
			std::vector<std::string> Errors;
			Poco::JSON::Object Inner;
			if (StorageService()->ExpandInUse(Existing.inUse, M, Errors)) {
				for (const auto &[type, list] : M) {
					Poco::JSON::Array ObjList;
					for (const auto &i : list.entries) {
						Poco::JSON::Object O;
						i.to_json(O);
						ObjList.add(O);
					}
					Inner.set(type, ObjList);
				}
			}
			Answer.set("entries", Inner);
			return ReturnObject(Answer);
		}

		if (QB_.AdditionalInfo)
			AddExtendedInfo(Existing, Answer);
		Existing.to_json(Answer);
		ReturnObject(Answer);
	}

	void RESTAPI_managementRole_handler::DoDelete() {
		ProvObjects::ManagementRole Existing;
		std::string UUID = GetBinding(RESTAPI::Protocol::ID, "");
		if (UUID.empty() || !DB_.GetRecord(RESTAPI::Protocol::ID, UUID, Existing)) {
			return NotFound();
		}

		bool Force = false;
		std::string Arg;
		if (HasParameter("force", Arg) && Arg == "true")
			Force = true;

		if (!Force && !Existing.inUse.empty()) {
			return BadRequest(RESTAPI::Errors::StillInUse);
		}

		DB_.DeleteRecord("id", Existing.info.id);
		MoveUsage(StorageService()->PolicyDB(), DB_, Existing.managementPolicy, "",
				  Existing.info.id);
		RemoveMembership(StorageService()->EntityDB(), &ProvObjects::Entity::managementRoles,
						 Existing.entity, Existing.info.id);
		RemoveMembership(StorageService()->VenueDB(), &ProvObjects::Venue::managementRoles,
						 Existing.venue, Existing.info.id);
		AuthCache::GetInstance()->Clear();
		return OK();
	}

	static bool CheckOverlap(const std::string &userId, const std::string &entityId, const std::string &venueId, const std::string &currentRoleId, std::string &ErrorDescription) {
		ManagementRoleDB::RecordVec Roles;
		if (StorageService()->RolesDB().GetRecords(0, 1000, Roles)) {
			std::vector<ProvObjects::ManagementRole> userRoles;
			for (const auto &role : Roles) {
				if (role.info.id == currentRoleId) continue;
				for (const auto &u : role.users) {
					if (u == userId) {
						userRoles.push_back(role);
						break;
					}
				}
			}

			if (userRoles.empty()) {
				return false;
			}

			auto GetPathToRoot = [](const std::string &eId, const std::string &vId) {
				std::vector<std::pair<std::string, std::string>> path;
				if (!vId.empty()) {
					std::string currentVenueId = vId;
					while (!currentVenueId.empty()) {
						path.push_back({eId, currentVenueId});
						ProvObjects::Venue V;
						if (StorageService()->VenueDB().GetRecord("id", currentVenueId, V)) {
							currentVenueId = V.parent;
						} else {
							break;
						}
					}
				}
				std::string currentEntityId = eId;
				while (!currentEntityId.empty()) {
					path.push_back({currentEntityId, ""});
					if (currentEntityId == StorageService()->EntityDB().RootUUID()) {
						break;
					}
					ProvObjects::Entity E;
					if (StorageService()->EntityDB().GetRecord("id", currentEntityId, E)) {
						currentEntityId = E.parent;
					} else {
						break;
					}
				}
				return path;
			};

			auto newPath = GetPathToRoot(entityId, venueId);

			for (const auto &existingRole : userRoles) {
				auto existingPath = GetPathToRoot(existingRole.entity, existingRole.venue);
				for (const auto &node : newPath) {
					if (node.first == existingRole.entity && node.second == existingRole.venue) {
						ErrorDescription = "Overlap detected: user already has a policy on parent/ancestor " + existingRole.entity + (existingRole.venue.empty() ? "" : " (venue: " + existingRole.venue + ")");
						return true;
					}
				}
				for (const auto &node : existingPath) {
					if (node.first == entityId && node.second == venueId) {
						ErrorDescription = "Overlap detected: user already has a policy on child/descendant " + existingRole.entity + (existingRole.venue.empty() ? "" : " (venue: " + existingRole.venue + ")");
						return true;
					}
				}
			}
		}
		return false;
	}

	static bool FindExactExistingRole(ManagementRoleDB &DB, const std::string &userId, const std::string &entityId, const std::string &venueId, ProvObjects::ManagementRole &ExistingRole) {
		ManagementRoleDB::RecordVec Roles;
		std::string WhereClause = "entity='" + entityId + "' and venue='" + venueId + "'";
		if (DB.GetRecords(0, 500, Roles, WhereClause)) {
			for (const auto &role : Roles) {
				for (const auto &user : role.users) {
					if (user == userId) {
						ExistingRole = role;
						return true;
					}
				}
			}
		}
		return false;
	}

	void RESTAPI_managementRole_handler::DoPost() {
		std::string UUID = GetBinding(RESTAPI::Protocol::ID, "");
		if (UUID.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		const auto &RawObj = ParsedBody_;
		ProvObjects::ManagementRole NewObject;
		if (!NewObject.from_json(RawObj)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if (!CreateObjectInfo(RawObj, UserInfo_.userinfo, NewObject.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		if (NewObject.entity.empty() ||
			!StorageService()->EntityDB().Exists("id", NewObject.entity)) {
			return BadRequest(RESTAPI::Errors::EntityMustExist);
		}

		// Validate system policy exists in DB
		if (!NewObject.managementPolicy.empty() &&
			!StorageService()->PolicyDB().Exists("id", NewObject.managementPolicy)) {
			return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
		}

		// Validate venue belongs to entity
		if (!NewObject.venue.empty()) {
			ProvObjects::Venue VenueObj;
			if (!StorageService()->VenueDB().GetRecord("id", NewObject.venue, VenueObj)) {
				return BadRequest(RESTAPI::Errors::VenueMustExist);
			}
			if (VenueObj.entity != NewObject.entity) {
				return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
			}
		}

		if (NewObject.users.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUserID);
		}
		std::string UserId = NewObject.users[0];

		// Check for existing role (idempotent upsert)
		ProvObjects::ManagementRole ExistingRole;
		if (FindExactExistingRole(DB_, UserId, NewObject.entity, NewObject.venue, ExistingRole)) {
			std::string OldPolicy = ExistingRole.managementPolicy;
			ExistingRole.managementPolicy = NewObject.managementPolicy;
			ExistingRole.info.modified = Utils::Now();

			if (DB_.UpdateRecord("id", ExistingRole.info.id, ExistingRole)) {
				AuthCache::GetInstance()->Clear();
				MoveUsage(StorageService()->PolicyDB(), DB_, OldPolicy, NewObject.managementPolicy, ExistingRole.info.id);

				Poco::JSON::Object Answer;
				ExistingRole.to_json(Answer);
				return ReturnObject(Answer);
			}
			return InternalError(RESTAPI::Errors::RecordNotUpdated);
		}

		std::string ErrorDescription;
		if (CheckOverlap(UserId, NewObject.entity, NewObject.venue, "", ErrorDescription)) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters, ErrorDescription);
		}

		if (DB_.CreateRecord(NewObject)) {
			AuthCache::GetInstance()->Clear();
			AddMembership(StorageService()->EntityDB(), &ProvObjects::Entity::managementRoles,
						  NewObject.entity, NewObject.info.id);
			AddMembership(StorageService()->VenueDB(), &ProvObjects::Venue::managementRoles,
						  NewObject.venue, NewObject.info.id);
			MoveUsage(StorageService()->PolicyDB(), DB_, "", NewObject.managementPolicy,
					  NewObject.info.id);

			Poco::JSON::Object Answer;
			ProvObjects::ManagementRole Role;
			DB_.GetRecord("id", NewObject.info.id, Role);
			Role.to_json(Answer);
			return ReturnObject(Answer);
		}
		InternalError(RESTAPI::Errors::RecordNotCreated);
	}

	void RESTAPI_managementRole_handler::DoPut() {
		ProvObjects::ManagementRole Existing;
		std::string UUID = GetBinding(RESTAPI::Protocol::ID, "");
		if (UUID.empty() || !DB_.GetRecord(RESTAPI::Protocol::ID, UUID, Existing)) {
			return NotFound();
		}

		const auto &RawObject = ParsedBody_;
		ProvObjects::ManagementRole NewObject;
		if (!NewObject.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if (!UpdateObjectInfo(RawObject, UserInfo_.userinfo, Existing.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		// Validate system policy exists in DB
		if (RawObject->has("managementPolicy")) {
			std::string PolicyUUID = RawObject->get("managementPolicy").toString();
			if (!PolicyUUID.empty() && !StorageService()->PolicyDB().Exists("id", PolicyUUID)) {
				return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
			}
		}

		// Validate venue belongs to entity
		if (!NewObject.venue.empty()) {
			ProvObjects::Venue VenueObj;
			if (!StorageService()->VenueDB().GetRecord("id", NewObject.venue, VenueObj)) {
				return BadRequest(RESTAPI::Errors::VenueMustExist);
			}
			if (VenueObj.entity != NewObject.entity) {
				return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
			}
		}

		std::string FromPolicy, ToPolicy;
		if (!CreateMove(RawObject, "managementPolicy",
						&ManagementRoleDB::RecordName::managementPolicy, Existing, FromPolicy,
						ToPolicy, StorageService()->PolicyDB()))
			return BadRequest(RESTAPI::Errors::EntityMustExist);

		std::string FromEntity, ToEntity;
		if (!CreateMove(RawObject, "entity", &ManagementRoleDB::RecordName::entity, Existing,
						FromEntity, ToEntity, StorageService()->EntityDB()))
			return BadRequest(RESTAPI::Errors::EntityMustExist);

		std::string FromVenue, ToVenue;
		if (!CreateMove(RawObject, "venue", &ManagementRoleDB::RecordName::venue, Existing,
						FromVenue, ToVenue, StorageService()->VenueDB()))
			return BadRequest(RESTAPI::Errors::EntityMustExist);

		if (!Existing.users.empty()) {
			std::string UserId = Existing.users[0];
			std::string ErrDesc;
			if (CheckOverlap(UserId, Existing.entity, Existing.venue, Existing.info.id, ErrDesc)) {
				return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters, ErrDesc);
			}
		}

		if (DB_.UpdateRecord("id", UUID, Existing)) {
			AuthCache::GetInstance()->Clear();
			MoveUsage(StorageService()->PolicyDB(), DB_, FromPolicy, ToPolicy, Existing.info.id);
			ManageMembership(StorageService()->EntityDB(), &ProvObjects::Entity::managementRoles,
							 FromEntity, ToEntity, Existing.info.id);
			ManageMembership(StorageService()->VenueDB(), &ProvObjects::Venue::managementRoles,
							 FromVenue, ToVenue, Existing.info.id);

			ProvObjects::ManagementRole NewRecord;

			DB_.GetRecord("id", UUID, NewRecord);
			Poco::JSON::Object Answer;
			NewRecord.to_json(Answer);
			return ReturnObject(Answer);
		}
		InternalError(RESTAPI::Errors::RecordNotUpdated);
	}
} // namespace OpenWifi