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

		if (!UpdateObjectInfo(RawObject, UserInfo_.userinfo, Existing.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		if (!RBAC::RequireAccess(*this, "managementRole", "MODIFY",
								 RBAC::TargetScope{Existing.entity, Existing.venue})) {
			return;
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

		if ((ToEntity != FromEntity || ToVenue != FromVenue) &&
			!RBAC::RequireAccess(*this, "managementRole", "MODIFY",
								 RBAC::TargetScope{ToEntity.empty() ? Existing.entity : ToEntity,
													ToVenue.empty() ? Existing.venue : ToVenue})) {
			return;
		}

		if (!ValidateManagementPolicyForRole(*this, Existing.managementPolicy, Existing)) {
			return;
		}

		if (ManagementRoleExistsForScope(Existing, Existing.info.id)) {
			return BadRequest(RESTAPI::Errors::UserAlreadyExists);
		}

		if (DB_.UpdateRecord("id", UUID, Existing)) {
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