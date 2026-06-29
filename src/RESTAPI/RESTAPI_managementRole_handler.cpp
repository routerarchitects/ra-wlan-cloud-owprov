//
// Created by stephane bourque on 2021-08-26.
//

#include "RESTAPI_managementRole_handler.h"

#include "Poco/JSON/Parser.h"
#include "Poco/StringTokenizer.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTAPI/RESTAPI_rbac_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"
#include "RESTAPI_managementRole_validation.h"

namespace OpenWifi {
	namespace {
		bool ContainsUser(const ProvObjects::ManagementRole &role, const std::string &userId) {
			return std::find(role.users.begin(), role.users.end(), userId) != role.users.end();
		}

		bool RoleScopeOverlaps(const ProvObjects::ManagementRole &lhs,
							   const ProvObjects::ManagementRole &rhs) {
			if (lhs.entity != rhs.entity || lhs.venue != rhs.venue) {
				return false;
			}
			for (const auto &userId : lhs.users) {
				if (ContainsUser(rhs, userId)) {
					return true;
				}
			}
			return false;
		}

		bool ManagementRoleExistsForScope(const ProvObjects::ManagementRole &candidate,
										  const std::string &skipId = {}) {
			bool found = false;
			StorageService()->RolesDB().Iterate(
				[&](const ProvObjects::ManagementRole &existing) {
					if (!skipId.empty() && existing.info.id == skipId) {
						return true;
					}
					if (RoleScopeOverlaps(candidate, existing)) {
						found = true;
						return false;
					}
					return true;
				});
			return found;
		}


	} // namespace

	void RESTAPI_managementRole_handler::DoGet() {
		ProvObjects::ManagementRole Existing;
		std::string UUID = GetBinding(RESTAPI::Protocol::ID, "");
		if (UUID.empty() || !DB_.GetRecord(RESTAPI::Protocol::ID, UUID, Existing)) {
			return NotFound();
		}

		if (!RBAC::RequireAccess(*this, "managementRole", "READ",
								 RBAC::TargetScope{Existing.entity, Existing.venue})) {
			return;
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

		if (!RBAC::RequireAccess(*this, "managementRole", "DELETE",
								 RBAC::TargetScope{Existing.entity, Existing.venue})) {
			return;
		}

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
		return OK();
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

		if (!ValidateManagementPolicyForRole(*this, NewObject.managementPolicy, NewObject)) {
			return;
		}

		if (!RBAC::RequireAccess(*this, "managementRole", "CREATE",
								 RBAC::TargetScope{NewObject.entity, NewObject.venue})) {
			return;
		}

		if (ManagementRoleExistsForScope(NewObject)) {
			return BadRequest(RESTAPI::Errors::UserAlreadyExists);
		}

		if (DB_.CreateRecord(NewObject)) {
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

		ProvObjects::ManagementRole Candidate = Existing;

		if (!UpdateObjectInfo(RawObject, UserInfo_.userinfo, Candidate.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		if (!RBAC::RequireAccess(*this, "managementRole", "UPDATE",
								 RBAC::TargetScope{Existing.entity, Existing.venue})) {
			return;
		}

		std::string FromPolicy, ToPolicy;
		if (RawObject->has("managementPolicy")) {
			FromPolicy = Existing.managementPolicy;
			ToPolicy = RawObject->get("managementPolicy").toString();
			if (!ToPolicy.empty() && !StorageService()->PolicyDB().Exists("id", ToPolicy)) {
				return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
			}
			Candidate.managementPolicy = ToPolicy;
		}

		std::string FromEntity, ToEntity;
		if (RawObject->has("entity")) {
			FromEntity = Existing.entity;
			ToEntity = RawObject->get("entity").toString();
			if (ToEntity.empty() || !StorageService()->EntityDB().Exists("id", ToEntity)) {
				return BadRequest(RESTAPI::Errors::EntityMustExist);
			}
			Candidate.entity = ToEntity;
		}

		std::string FromVenue, ToVenue;
		if (RawObject->has("venue")) {
			FromVenue = Existing.venue;
			ToVenue = RawObject->get("venue").toString();
			if (!ToVenue.empty() && !StorageService()->VenueDB().Exists("id", ToVenue)) {
				return BadRequest(RESTAPI::Errors::VenueMustExist);
			}
			Candidate.venue = ToVenue;
		}

		if (RawObject->has("users")) {
			Types::UUIDvec_t Users;
			if (!AssignIfPresent(RawObject, "users", Users) || Users.empty()) {
				return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
			}
			Candidate.users = Users;
		}

		if ((Candidate.entity != Existing.entity || Candidate.venue != Existing.venue) &&
			!RBAC::RequireAccess(*this, "managementRole", "UPDATE",
								 RBAC::TargetScope{Candidate.entity, Candidate.venue})) {
			return;
		}

		if (!ValidateManagementPolicyForRole(*this, Candidate.managementPolicy, Candidate)) {
			return;
		}

		if (ManagementRoleExistsForScope(Candidate, Existing.info.id)) {
			return BadRequest(RESTAPI::Errors::UserAlreadyExists);
		}

		if (DB_.UpdateRecord("id", UUID, Candidate)) {
			MoveUsage(StorageService()->PolicyDB(), DB_, FromPolicy, ToPolicy, Candidate.info.id);
			ManageMembership(StorageService()->EntityDB(), &ProvObjects::Entity::managementRoles,
							 FromEntity, ToEntity, Candidate.info.id);
			ManageMembership(StorageService()->VenueDB(), &ProvObjects::Venue::managementRoles,
							 FromVenue, ToVenue, Candidate.info.id);

			ProvObjects::ManagementRole NewRecord;

			DB_.GetRecord("id", UUID, NewRecord);
			Poco::JSON::Object Answer;
			NewRecord.to_json(Answer);
			return ReturnObject(Answer);
		}
		InternalError(RESTAPI::Errors::RecordNotUpdated);
	}
} // namespace OpenWifi
