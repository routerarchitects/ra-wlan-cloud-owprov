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

	static bool AccessEntryGrants(const ProvObjects::ManagementPolicyEntry &entry, const std::string &resource, const std::string &accessRequired) {
		bool ResourceMatches = false;
		for (const auto &res : entry.resources) {
			if (Poco::icompare(res, resource) == 0 || res == "*") {
				ResourceMatches = true;
				break;
			}
		}
		if (!ResourceMatches) {
			return false;
		}

		for (const auto &acc : entry.access) {
			if (acc == "FULL" ||
				acc == accessRequired ||
				(accessRequired == "UPDATE" && acc == "MODIFY") ||
				(accessRequired == "MODIFY" && acc == "UPDATE")) {
				return true;
			}
		}
		return false;
	}

	static bool PolicyGrants(const ProvObjects::ManagementPolicy &policy, const std::string &resource, const std::string &accessRequired) {
		for (const auto &entry : policy.entries) {
			if (AccessEntryGrants(entry, resource, accessRequired)) {
				return true;
			}
		}
		return false;
	}

	static bool RequesterHasEqualOrStrongerPermission(const std::string &userId,
													  const std::string &entityId,
													  const std::string &venueId,
													  const ProvObjects::ManagementPolicy &TargetPolicy,
													  std::string &ErrorDescription) {
		ManagementRoleDB::RecordVec Roles;
		std::vector<ProvObjects::ManagementPolicy> requesterPolicies;
		if (StorageService()->RolesDB().GetRecords(0, 1000, Roles)) {
			for (const auto &role : Roles) {
				if (role.entity != entityId || role.venue != venueId) {
					continue;
				}
				for (const auto &u : role.users) {
					if (u == userId) {
						ProvObjects::ManagementPolicy Policy;
						if (StorageService()->PolicyDB().GetRecord("id", role.managementPolicy, Policy)) {
							requesterPolicies.push_back(Policy);
						}
						break;
					}
				}
			}
		}

		if (requesterPolicies.empty()) {
			ErrorDescription = "Privilege mismatch: requester has no role on the target scope.";
			return false;
		}

		for (const auto &entry : TargetPolicy.entries) {
			for (const auto &res : entry.resources) {
				for (const auto &acc : entry.access) {
					bool Covered = false;
					for (const auto &policy : requesterPolicies) {
						if (PolicyGrants(policy, res, acc) || PolicyGrants(policy, res, "FULL")) {
							Covered = true;
							break;
						}
					}
					if (acc == "FULL") {
						if (!Covered) {
							ErrorDescription = "Privilege mismatch: requester does not have FULL permission on resource " + res;
							return false;
						}
					} else if (!Covered) {
						ErrorDescription = "Privilege mismatch: requester does not have " + acc + " permission on resource " + res;
						return false;
					}
				}
			}
		}
		return true;
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

	static bool ValidateAssignableUser(RESTAPIHandler *handler,
									   const std::string &requesterUserId,
									   SecurityObjects::USER_ROLE requesterRole,
									   const std::string &targetUserId,
									   std::string &ErrorDescription) {
		SecurityObjects::UserInfo TargetUser;
		if (!SDK::Sec::User::Get(handler, targetUserId, TargetUser)) {
			ErrorDescription = "The selected user could not be found.";
			return false;
		}

		if (requesterRole == SecurityObjects::ROOT) {
			return true;
		}

		if (TargetUser.createdBy.empty() || TargetUser.createdBy != requesterUserId) {
			ErrorDescription = "You can only assign access to users that you created.";
			return false;
		}

		return true;
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
		ProvObjects::ManagementPolicy TargetPolicy;
		if (!NewObject.managementPolicy.empty()) {
			if (!StorageService()->PolicyDB().GetRecord("id", NewObject.managementPolicy, TargetPolicy)) {
				return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
			}
			if (UserInfo_.userinfo.userRole != SecurityObjects::ROOT && UserInfo_.userinfo.userRole != SecurityObjects::SYSTEM) {
				std::string PrivilegeError;
				if (!RequesterHasEqualOrStrongerPermission(UserInfo_.userinfo.id, NewObject.entity, NewObject.venue, TargetPolicy, PrivilegeError)) {
					return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters, PrivilegeError);
				}
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

		if (NewObject.users.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUserID);
		}
		std::string UserId = NewObject.users[0];
		std::string UserValidationError;
		if (!ValidateAssignableUser(this, UserInfo_.userinfo.id, UserInfo_.userinfo.userRole, UserId, UserValidationError)) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters, UserValidationError);
		}

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

		std::string EffectivePolicyUUID = Existing.managementPolicy;
		if (RawObject->has("managementPolicy")) {
			EffectivePolicyUUID = RawObject->get("managementPolicy").toString();
		}

		std::string EffectiveEntity = Existing.entity;
		if (RawObject->has("entity")) {
			EffectiveEntity = RawObject->get("entity").toString();
		}

		std::string EffectiveVenue = Existing.venue;
		if (RawObject->has("venue")) {
			EffectiveVenue = RawObject->get("venue").toString();
		}

		ProvObjects::ManagementPolicy TargetPolicy;
		if (!EffectivePolicyUUID.empty()) {
			if (!StorageService()->PolicyDB().GetRecord("id", EffectivePolicyUUID, TargetPolicy)) {
				return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
			}
			if (UserInfo_.userinfo.userRole != SecurityObjects::ROOT && UserInfo_.userinfo.userRole != SecurityObjects::SYSTEM) {
				std::string PrivilegeError;
				if (!RequesterHasEqualOrStrongerPermission(UserInfo_.userinfo.id, EffectiveEntity, EffectiveVenue, TargetPolicy, PrivilegeError)) {
					return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters, PrivilegeError);
				}
			}
		}

		// Validate venue belongs to entity
		if (!EffectiveVenue.empty()) {
			ProvObjects::Venue VenueObj;
			if (!StorageService()->VenueDB().GetRecord("id", EffectiveVenue, VenueObj)) {
				return BadRequest(RESTAPI::Errors::VenueMustExist);
			}
			if (VenueObj.entity != EffectiveEntity) {
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
