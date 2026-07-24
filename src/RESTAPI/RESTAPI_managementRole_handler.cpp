//
// Created by stephane bourque on 2021-08-26.
//

#include "RESTAPI_managementRole_handler.h"

#include "Poco/JSON/Parser.h"
#include "Poco/StringTokenizer.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"
#include <set>

namespace OpenWifi {

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

		if (requesterRole != SecurityObjects::ROOT) {
			if (TargetUser.createdBy != requesterUserId && TargetUser.id != requesterUserId) {
				ErrorDescription = "You are not authorized to assign or modify roles for users you did not create.";
				return false;
			}
		}

		return true;
	}

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

		if (UserInfo_.userinfo.userRole != SecurityObjects::ROOT) {
			for (const auto &userId : Existing.users) {
				std::string UserValidationError;
				if (!ValidateAssignableUser(this, UserInfo_.userinfo.id, UserInfo_.userinfo.userRole, userId, UserValidationError)) {
					return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters, UserValidationError);
				}
			}
		}

		if (!DB_.DeleteRecord("id", Existing.info.id)) {
			return InternalError(RESTAPI::Errors::CouldNotBeDeleted);
		}
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
		std::vector<ProvObjects::ManagementRole> Roles;
		if (!AuthCache::GetInstance()->GetUserRoles(userId, Roles)) {
			StorageService()->RolesDB().Iterate([&](const ProvObjects::ManagementRole &role) {
				for (const auto &u : role.users) {
					if (u == userId) {
						Roles.push_back(role);
						break;
					}
				}
				return true;
			});
			AuthCache::GetInstance()->SetUserRoles(userId, Roles);
		}

		std::vector<ProvObjects::ManagementPolicy> requesterPolicies;
		for (const auto &role : Roles) {
			std::set<std::string> AllowedEntities;
			RESTAPIHandler::GetDescendantEntities(role.entity, AllowedEntities);
			if (AllowedEntities.find(entityId) != AllowedEntities.end() && (role.venue == venueId || role.venue.empty())) {
				ProvObjects::ManagementPolicy Policy;
				if (!AuthCache::GetInstance()->GetPolicy(role.managementPolicy, Policy)) {
					if (StorageService()->PolicyDB().GetRecord("id", role.managementPolicy, Policy)) {
						AuthCache::GetInstance()->SetPolicy(role.managementPolicy, Policy);
					} else {
						continue;
					}
				}
				requesterPolicies.push_back(Policy);
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
		std::vector<ProvObjects::ManagementRole> Roles;
		if (AuthCache::GetInstance()->GetUserRoles(userId, Roles)) {
			for (const auto &role : Roles) {
				if (role.entity == entityId && role.venue == venueId) {
					ExistingRole = role;
					return true;
				}
			}
			return false;
		}

		ManagementRoleDB::RecordVec DB_Roles;
		std::string WhereClause = "entity='" + ORM::Escape(entityId) + "' and venue='" + ORM::Escape(venueId) + "' and users LIKE '%" + ORM::Escape(userId) + "%'";
		if (DB.GetRecords(0, 100, DB_Roles, WhereClause)) {
			for (const auto &role : DB_Roles) {
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

	static std::vector<std::string> ParseVenueIds(const Poco::JSON::Object::Ptr &RawObj,
												  const std::string &FallbackVenue) {
		std::vector<std::string> VenueIds;
		std::set<std::string> Seen;

		if (RawObj && RawObj->isArray("venueIds")) {
			auto VenueArray = RawObj->getArray("venueIds");
			for (const auto &value : *VenueArray) {
				auto VenueId = value.toString();
				if (!VenueId.empty() && Seen.insert(VenueId).second) {
					VenueIds.emplace_back(VenueId);
				}
			}
		}

		if (VenueIds.empty() && !FallbackVenue.empty()) {
			VenueIds.emplace_back(FallbackVenue);
		}

		return VenueIds;
	}

	static bool ValidateVenueScope(const std::string &entityId, const std::string &venueId) {
		if (venueId.empty()) {
			return true;
		}

		ProvObjects::Venue VenueObj;
		if (!StorageService()->VenueDB().GetRecord("id", venueId, VenueObj)) {
			return false;
		}

		return VenueObj.entity == entityId;
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

		auto Scopes = ParseVenueIds(RawObj, NewObject.venue);
		if (Scopes.empty()) {
			Scopes.emplace_back("");
		}

		// Validate system policy exists in DB
		ProvObjects::ManagementPolicy TargetPolicy;
		if (!NewObject.managementPolicy.empty()) {
			if (!StorageService()->PolicyDB().GetRecord("id", NewObject.managementPolicy, TargetPolicy)) {
				return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
			}
		}

		for (const auto &venueId : Scopes) {
			if (!ValidateVenueScope(NewObject.entity, venueId)) {
				return BadRequest(RESTAPI::Errors::VenueMustExist);
			}
		}

		if (UserInfo_.userinfo.userRole != SecurityObjects::ROOT &&
			!NewObject.managementPolicy.empty()) {
			for (const auto &venueId : Scopes) {
				std::string PrivilegeError;
				if (!RequesterHasEqualOrStrongerPermission(UserInfo_.userinfo.id, NewObject.entity, venueId, TargetPolicy, PrivilegeError)) {
					return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters, PrivilegeError);
				}
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

		// TODO: Upgrade to native SQL transactions later (requires ORM update in orm.h to support session-bound transactions)
		std::vector<ProvObjects::ManagementRole> SavedRoles;
		std::vector<ProvObjects::ManagementRole> NewlyCreatedRoles;

		bool BatchFailed = false;
		for (std::size_t idx = 0; idx < Scopes.size(); ++idx) {
			ProvObjects::ManagementRole RoleForScope = NewObject;
			RoleForScope.venue = Scopes[idx];
			if (idx > 0) {
				RoleForScope.info.id = MicroServiceCreateUUID();
			}

			ProvObjects::ManagementRole ExistingRole;
			if (FindExactExistingRole(DB_, UserId, RoleForScope.entity, RoleForScope.venue, ExistingRole)) {
				ExistingRole.managementPolicy = RoleForScope.managementPolicy;
				ExistingRole.info.modified = Utils::Now();

				if (!DB_.UpdateRecord("id", ExistingRole.info.id, ExistingRole)) {
					BatchFailed = true;
					break;
				}
				SavedRoles.emplace_back(ExistingRole);
				continue;
			}

			if (!DB_.CreateRecord(RoleForScope)) {
				BatchFailed = true;
				break;
			}
			NewlyCreatedRoles.emplace_back(RoleForScope);
			SavedRoles.emplace_back(RoleForScope);
		}

		if (BatchFailed) {
			for (const auto &role : NewlyCreatedRoles) {
				DB_.DeleteRecord("id", role.info.id);
			}
			return InternalError(RESTAPI::Errors::RecordNotCreated);
		}

		AuthCache::GetInstance()->Clear();

		if (SavedRoles.size() == 1) {
			Poco::JSON::Object Answer;
			SavedRoles.front().to_json(Answer);
			return ReturnObject(Answer);
		}

		Poco::JSON::Object Answer;
		Poco::JSON::Array RolesArray;
		for (const auto &role : SavedRoles) {
			Poco::JSON::Object RoleObject;
			role.to_json(RoleObject);
			RolesArray.add(RoleObject);
		}
		Answer.set("roles", RolesArray);
		return ReturnObject(Answer);
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

		if (UserInfo_.userinfo.userRole != SecurityObjects::ROOT) {
			for (const auto &userId : Existing.users) {
				std::string UserValidationError;
				if (!ValidateAssignableUser(this, UserInfo_.userinfo.id, UserInfo_.userinfo.userRole, userId, UserValidationError)) {
					return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters, UserValidationError);
				}
			}
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
			if (UserInfo_.userinfo.userRole != SecurityObjects::ROOT) {
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

		Existing.managementPolicy = EffectivePolicyUUID;
		Existing.entity = EffectiveEntity;
		Existing.venue = EffectiveVenue;

		if (DB_.UpdateRecord("id", UUID, Existing)) {
			AuthCache::GetInstance()->Clear();

			ProvObjects::ManagementRole NewRecord;
			DB_.GetRecord("id", UUID, NewRecord);
			Poco::JSON::Object Answer;
			NewRecord.to_json(Answer);
			return ReturnObject(Answer);
		}
		InternalError(RESTAPI::Errors::RecordNotUpdated);
	}
} // namespace OpenWifi
