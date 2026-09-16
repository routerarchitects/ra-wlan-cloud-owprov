//
// Created for OpenWifi Prov V2 Management Role API
//

#include "RESTAPI_managementRole_v2_handler.h"

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

	static bool AccessEntryGrants(const ProvObjects::ManagementPolicyEntry &entry, const std::string &resource, const std::string &accessRequired) {
		bool ResourceMatches = false;
		for (const auto &res : entry.resources) {
			if (Poco::icompare(res, resource) == 0 || res == "*" ||
				(Poco::icompare(resource, "subscriberDevice") == 0 && Poco::icompare(res, "inventory") == 0) ||
				(Poco::icompare(resource, "op_contact") == 0 && Poco::icompare(res, "contact") == 0) ||
				(Poco::icompare(resource, "op_location") == 0 && Poco::icompare(res, "location") == 0)) {
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
				(accessRequired == "CREATE" && (acc == "MODIFY" || acc == "UPDATE" || acc == "READWRITE")) ||
				(accessRequired == "UPDATE" && (acc == "MODIFY" || acc == "READWRITE")) ||
				(accessRequired == "MODIFY" && (acc == "UPDATE" || acc == "READWRITE")) ||
				(accessRequired == "READ" && (acc == "READWRITE" || acc == "MODIFY"))) {
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
			if (!Roles.empty()) {
				AuthCache::GetInstance()->SetUserRoles(userId, Roles);
			}
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
						requesterPolicies.push_back(Policy);
					}
				} else {
					requesterPolicies.push_back(Policy);
				}
			}
		}

		if (requesterPolicies.empty()) {
			ErrorDescription = "Requester holds no roles matching or governing the target scope.";
			return false;
		}

		for (const auto &targetEntry : TargetPolicy.entries) {
			for (const auto &targetRes : targetEntry.resources) {
				for (const auto &targetAcc : targetEntry.access) {
					bool granted = false;
					for (const auto &reqPolicy : requesterPolicies) {
						if (PolicyGrants(reqPolicy, targetRes, targetAcc)) {
							granted = true;
							break;
						}
					}
					if (!granted) {
						ErrorDescription = Poco::format("Cannot assign permissions stronger than held: missing '%s' on '%s'.", targetAcc, targetRes);
						return false;
					}
				}
			}
		}

		return true;
	}

	static bool FindExactExistingRole(ManagementRoleDB &DB,
									  const std::string &userId,
									  const std::string &entityId,
									  const std::string &venueId,
									  ProvObjects::ManagementRole &ExistingRole) {
		std::string WhereClause;
		if (venueId.empty()) {
			WhereClause = Poco::format("entity='%s' AND (venue IS NULL OR venue='') AND users LIKE '%%%s%%'",
									   ORM::Escape(entityId), ORM::Escape(userId));
		} else {
			WhereClause = Poco::format("entity='%s' AND venue='%s' AND users LIKE '%%%s%%'",
									   ORM::Escape(entityId), ORM::Escape(venueId), ORM::Escape(userId));
		}

		std::vector<ProvObjects::ManagementRole> CandidateRoles;
		if (DB.GetRecords(0, 50, CandidateRoles, WhereClause)) {
			for (const auto &role : CandidateRoles) {
				if (role.entity == entityId && role.venue == venueId) {
					for (const auto &u : role.users) {
						if (u == userId) {
							ExistingRole = role;
							return true;
						}
					}
				}
			}
		}
		return false;
	}

	static std::vector<std::string> ParseVenueIds(const Poco::JSON::Object::Ptr &RawObj) {
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

	void RESTAPI_managementRole_v2_handler::DoPost() {
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

		// V2 strictly requires venueIds to be a JSON array if provided
		if (RawObj->has("venueIds") && !RawObj->isArray("venueIds")) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters,
							  "Field 'venueIds' must be a JSON array of venue UUIDs.");
		}

		// V2 strictly extracts venueIds array (single venue string is not used)
		auto Scopes = ParseVenueIds(RawObj);
		if (Scopes.empty()) {
			Scopes.emplace_back("");
		}

		// Validate system policy exists in DB
		ProvObjects::ManagementPolicy TargetPolicy;
		if (NewObject.managementPolicy.empty()) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters,
							  "Management policy is required and cannot be empty.");
		}
		if (!StorageService()->PolicyDB().GetRecord("id", NewObject.managementPolicy, TargetPolicy)) {
			return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
		}

		for (const auto &venueId : Scopes) {
			if (!ValidateVenueScope(NewObject.entity, venueId)) {
				return BadRequest(RESTAPI::Errors::VenueMustExist);
			}
		}

		if (UserInfo_.userinfo.userRole != SecurityObjects::ROOT) {
			for (const auto &venueId : Scopes) {
				std::string PrivilegeError;
				if (!RequesterHasEqualOrStrongerPermission(UserInfo_.userinfo.id, NewObject.entity, venueId, TargetPolicy, PrivilegeError)) {
					return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters, PrivilegeError);
				}
			}
		}

		if (NewObject.users.size() != 1) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters, "Management role must contain exactly one user in the users array.");
		}
		std::string UserId = NewObject.users[0];
		std::string UserValidationError;
		if (!ValidateAssignableUser(this, UserInfo_.userinfo.id, UserInfo_.userinfo.userRole, UserId, UserValidationError)) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters, UserValidationError);
		}

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

		// V2 always returns {"roles": [...]} envelope
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

	void RESTAPI_managementRole_v2_handler::DoGet() {
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

	void RESTAPI_managementRole_v2_handler::DoDelete() {
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

	void RESTAPI_managementRole_v2_handler::DoPut() {
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

		if (RawObject->has("entity")) {
			std::string RequestedEntity = RawObject->get("entity").toString();
			if (RequestedEntity != Existing.entity) {
				return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters,
					"Entity ID, Venue ID, and User ID are immutable. To change scope, delete the existing role and create a new role.");
			}
		}

		if (RawObject->has("venue")) {
			std::string RequestedVenue = RawObject->get("venue").toString();
			if (RequestedVenue != Existing.venue) {
				return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters,
					"Entity ID, Venue ID, and User ID are immutable. To change scope, delete the existing role and create a new role.");
			}
		}

		if (RawObject->has("venueIds")) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters,
				"Entity ID, Venue ID, and User ID are immutable. To change scope, delete the existing role and create a new role.");
		}

		if (RawObject->has("users")) {
			if (NewObject.users != Existing.users) {
				return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters,
					"Entity ID, Venue ID, and User ID are immutable. To change scope, delete the existing role and create a new role.");
			}
		}

		std::string EffectivePolicyUUID = Existing.managementPolicy;
		if (RawObject->has("managementPolicy")) {
			EffectivePolicyUUID = RawObject->get("managementPolicy").toString();
		}

		std::string EffectiveEntity = Existing.entity;
		std::string EffectiveVenue = Existing.venue;

		if (EffectivePolicyUUID.empty()) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters,
							  "Management policy is required and cannot be empty.");
		}

		ProvObjects::ManagementPolicy TargetPolicy;
		if (!StorageService()->PolicyDB().GetRecord("id", EffectivePolicyUUID, TargetPolicy)) {
			return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
		}
		if (UserInfo_.userinfo.userRole != SecurityObjects::ROOT) {
			std::string PrivilegeError;
			if (!RequesterHasEqualOrStrongerPermission(UserInfo_.userinfo.id, EffectiveEntity, EffectiveVenue, TargetPolicy, PrivilegeError)) {
				return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters, PrivilegeError);
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

