//
// Created by stephane bourque on 2022-10-25.
//

#include "RESTAPI_Handler.h"
#include "StorageService.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "framework/MicroServiceFuncs.h"

namespace OpenWifi {

	bool RESTAPIHandler::RoleIsAuthorized(const std::string &Path,
										  const std::string &Method,
										  std::string &Reason) {
		// 1. Bypass check only if user is root
		if (UserInfo_.userinfo.userRole == SecurityObjects::ROOT) {
			return true;
		}

		// 2. Map path to resource
		std::string Resource = GetResourceName(Path);
		if (Resource.empty()) {
			Reason = "Unknown or prohibited resource path.";
			return false;
		}

		std::string UserId = UserInfo_.userinfo.id;
		std::vector<ProvObjects::ManagementRole> Roles;
		if (!AuthCache::GetInstance()->GetUserRoles(UserId, Roles)) {
			ManagementRoleDB::RecordVec DB_Roles;
			if (StorageService()->RolesDB().GetRecords(0, 1000, DB_Roles)) {
				for (const auto &role : DB_Roles) {
					for (const auto &user : role.users) {
						if (user == UserId) {
							Roles.push_back(role);
						}
					}
				}
			}
			AuthCache::GetInstance()->SetUserRoles(UserId, Roles);
		}

		auto CheckRolePolicy = [&](const ProvObjects::ManagementRole &role) -> bool {
			ProvObjects::ManagementPolicy Policy;
			if (!AuthCache::GetInstance()->GetPolicy(role.managementPolicy, Policy)) {
				if (!StorageService()->PolicyDB().GetRecord("id", role.managementPolicy, Policy)) {
					return false;
				}
				AuthCache::GetInstance()->SetPolicy(role.managementPolicy, Policy);
			}
			return PolicyAllows(Policy, Resource, Method);
		};

		// 3. Resolve target Entity and Venue
		std::string TargetEntity, TargetVenue;
		if (!ResolveTargetContext(Path, Method, TargetEntity, TargetVenue)) {
			// Check if any of the user's roles permit this resource and method.
			for (const auto &role : Roles) {
				if (CheckRolePolicy(role)) {
					return true;
				}
			}
			Reason = "No authorized role found for this target resource and operation.";
			return false;
		}

		if (!TargetVenue.empty()) {
			for (const auto &role : Roles) {
				if (role.entity == TargetEntity && role.venue == TargetVenue) {
					if (CheckRolePolicy(role)) {
						return true;
					}
				}
			}
		} else {
			for (const auto &role : Roles) {
				if (role.entity == TargetEntity && (role.venue.empty() || role.venue == "")) {
					if (CheckRolePolicy(role)) {
						return true;
					}
				}
			}
		}

		Reason = "No authorized role matches the required scope and permission.";
		return false;
	}

	bool RESTAPIHandler::ResolveTargetContext(const std::string &Path, const std::string &Method, std::string &TargetEntity, std::string &TargetVenue) {
		// 1. Check bindings for ID
		std::string Id;
		auto it = Bindings_.find("id");
		if (it != Bindings_.end()) {
			Id = it->second;
		} else {
			it = Bindings_.find("uuid");
			if (it != Bindings_.end()) {
				Id = it->second;
			}
		}

		// 2. For object routes, always derive scope from the bound object first.
		if (!Id.empty() && Id != "0") {
			if (Path.find("/api/v1/entity") != std::string::npos) {
				TargetEntity = Id;
			} else if (Path.find("/api/v1/venue") != std::string::npos) {
				TargetVenue = Id;
				ProvObjects::Venue V;
				if (StorageService()->VenueDB().GetRecord("id", Id, V)) {
					TargetEntity = V.entity;
				}
			} else if (Path.find("/api/v1/inventory") != std::string::npos) {
				ProvObjects::InventoryTag T;
				if (StorageService()->InventoryDB().GetRecord("id", Id, T)) {
					TargetEntity = T.entity;
					TargetVenue = T.venue;
				}
			} else if (Path.find("/api/v1/configuration") != std::string::npos) {
				ProvObjects::DeviceConfiguration C;
				if (StorageService()->ConfigurationDB().GetRecord("id", Id, C)) {
					TargetEntity = C.entity;
					TargetVenue = C.venue;
				}
			} else if (Path.find("/api/v1/managementRole") != std::string::npos) {
				ProvObjects::ManagementRole R;
				if (StorageService()->RolesDB().GetRecord("id", Id, R)) {
					TargetEntity = R.entity;
					TargetVenue = R.venue;
				}
			}

			if (!TargetEntity.empty() || !TargetVenue.empty()) {
				return true;
			}
		}

		// 3. For collection routes, fall back to query parameters.
		for (const auto &[name, value] : Parameters_) {
			if (name == "entity") {
				TargetEntity = value;
			} else if (name == "venue") {
				TargetVenue = value;
			}
		}

		// 4. Check body if parsed body exists
		if (TargetEntity.empty() && TargetVenue.empty() && ParsedBody_) {
			if (ParsedBody_->has("entity")) {
				TargetEntity = ParsedBody_->get("entity").toString();
			}
			if (ParsedBody_->has("venue")) {
				TargetVenue = ParsedBody_->get("venue").toString();
			}
			if (TargetEntity.empty() && TargetVenue.empty() && ParsedBody_->has("parent")) {
				std::string ParentId = ParsedBody_->get("parent").toString();
				if (!ParentId.empty()) {
					if (Path.find("/api/v1/entity") != std::string::npos) {
						TargetEntity = ParentId;
					} else if (Path.find("/api/v1/venue") != std::string::npos) {
						TargetVenue = ParentId;
						ProvObjects::Venue V;
						if (StorageService()->VenueDB().GetRecord("id", ParentId, V)) {
							TargetEntity = V.entity;
						}
					}
				}
			}
		}

		return !TargetEntity.empty() || !TargetVenue.empty();
	}

	bool RESTAPIHandler::PolicyAllows(const ProvObjects::ManagementPolicy &Policy, const std::string &Resource, const std::string &Method) {
		std::string AccessRequired;
		if (Method == Poco::Net::HTTPRequest::HTTP_GET) AccessRequired = "READ";
		else if (Method == Poco::Net::HTTPRequest::HTTP_POST) AccessRequired = "CREATE";
		else if (Method == Poco::Net::HTTPRequest::HTTP_PUT) AccessRequired = "UPDATE";
		else if (Method == Poco::Net::HTTPRequest::HTTP_DELETE) AccessRequired = "DELETE";
		else return false;

		for (const auto &entry : Policy.entries) {
			bool ResourceMatches = false;
			for (const auto &res : entry.resources) {
				if (Poco::icompare(res, Resource) == 0 || res == "*") {
					ResourceMatches = true;
					break;
				}
			}
			if (!ResourceMatches) continue;

			for (const auto &acc : entry.access) {
				if (acc == "FULL" || acc == AccessRequired || (AccessRequired == "UPDATE" && acc == "MODIFY")) {
					return true;
				}
			}
		}
		return false;
	}

	bool AuthCache::GetUserRoles(const std::string &userId, std::vector<ProvObjects::ManagementRole> &roles) {
		std::shared_lock<std::shared_mutex> lock(Mutex_);
		auto it = Cache_.find(userId);
		if (it != Cache_.end()) {
			roles = it->second.roles;
			return true;
		}
		return false;
	}

	void AuthCache::SetUserRoles(const std::string &userId, const std::vector<ProvObjects::ManagementRole> &roles) {
		std::unique_lock<std::shared_mutex> lock(Mutex_);
		Cache_[userId].roles = roles;
		Cache_[userId].lastFetched = Utils::Now();
	}

	bool AuthCache::GetPolicy(const std::string &policyId, ProvObjects::ManagementPolicy &policy) {
		std::shared_lock<std::shared_mutex> lock(Mutex_);
		auto it = Policies_.find(policyId);
		if (it != Policies_.end()) {
			policy = it->second;
			return true;
		}
		return false;
	}

	void AuthCache::SetPolicy(const std::string &policyId, const ProvObjects::ManagementPolicy &policy) {
		std::unique_lock<std::shared_mutex> lock(Mutex_);
		Policies_[policyId] = policy;
	}

	void AuthCache::InvalidateUser(const std::string &userId) {
		std::unique_lock<std::shared_mutex> lock(Mutex_);
		Cache_.erase(userId);
	}

	void AuthCache::Clear() {
		std::unique_lock<std::shared_mutex> lock(Mutex_);
		Cache_.clear();
		Policies_.clear();
	}

	bool RESTAPIHandler::FindAnyRole(const std::string &userId, ProvObjects::ManagementRole &AnyRole) {
		std::vector<ProvObjects::ManagementRole> Roles;
		if (!AuthCache::GetInstance()->GetUserRoles(userId, Roles)) {
			ManagementRoleDB::RecordVec DB_Roles;
			if (StorageService()->RolesDB().GetRecords(0, 1000, DB_Roles)) {
				for (const auto &role : DB_Roles) {
					for (const auto &user : role.users) {
						if (user == userId) {
							Roles.push_back(role);
						}
					}
				}
			}
			AuthCache::GetInstance()->SetUserRoles(userId, Roles);
		}

		if (!Roles.empty()) {
			AnyRole = Roles.front();
			return true;
		}
		return false;
	}

	bool RESTAPIHandler::FindAllUserRoles(const std::string &userId, std::vector<ProvObjects::ManagementRole> &Roles) {
		if (!AuthCache::GetInstance()->GetUserRoles(userId, Roles)) {
			ManagementRoleDB::RecordVec DB_Roles;
			if (StorageService()->RolesDB().GetRecords(0, 1000, DB_Roles)) {
				for (const auto &role : DB_Roles) {
					for (const auto &user : role.users) {
						if (user == userId) {
							Roles.push_back(role);
						}
					}
				}
			}
			AuthCache::GetInstance()->SetUserRoles(userId, Roles);
		}
		return !Roles.empty();
	}

	bool RESTAPIHandler::FindExistingRole(const std::string &userId, const std::string &entityId, const std::string &venueId, ProvObjects::ManagementRole &ExistingRole) {
		std::vector<ProvObjects::ManagementRole> Roles;
		if (!AuthCache::GetInstance()->GetUserRoles(userId, Roles)) {
			ManagementRoleDB::RecordVec DB_Roles;
			if (StorageService()->RolesDB().GetRecords(0, 1000, DB_Roles)) {
				for (const auto &role : DB_Roles) {
					for (const auto &user : role.users) {
						if (user == userId) {
							Roles.push_back(role);
						}
					}
				}
			}
			AuthCache::GetInstance()->SetUserRoles(userId, Roles);
		}

		if (!venueId.empty()) {
			for (const auto &role : Roles) {
				if (role.entity == entityId && role.venue == venueId) {
					ExistingRole = role;
					return true;
				}
			}
		} else {
			for (const auto &role : Roles) {
				if (role.entity == entityId && (role.venue.empty() || role.venue == "")) {
					ExistingRole = role;
					return true;
				}
			}
		}

		return false;
	}

	void RESTAPIHandler::GetDescendantEntities(const std::string &id, std::set<std::string> &descendants) {
		descendants.insert(id);
		ProvObjects::Entity E;
		if (StorageService()->EntityDB().GetRecord("id", id, E)) {
			for (const auto &child : E.children) {
				GetDescendantEntities(child, descendants);
			}
		}
	}

	void RESTAPIHandler::GetDescendantVenues(const std::string &id, std::set<std::string> &venues) {
		venues.insert(id);
		ProvObjects::Venue V;
		if (StorageService()->VenueDB().GetRecord("id", id, V)) {
			for (const auto &child : V.children) {
				GetDescendantVenues(child, venues);
			}
		}
	}

	std::string RESTAPIHandler::GetResourceName(const std::string &Path) {
		if (Path.find("/api/v1/entity") != std::string::npos) return "entity";
		if (Path.find("/api/v1/venue") != std::string::npos) return "venue";
		if (Path.find("/api/v1/inventory") != std::string::npos) return "inventory";
		if (Path.find("/api/v1/configuration") != std::string::npos) return "configuration";
		if (Path.find("/api/v1/managementRole") != std::string::npos) return "managementRole";
		if (Path.find("/api/v1/managementPolicy") != std::string::npos) return "managementPolicy";
		if (Path.find("/api/v1/operator") != std::string::npos) return "operator";
		if (Path.find("/api/v1/customer") != std::string::npos) return "customer";
		if (Path.find("/api/v1/user") != std::string::npos) return "user";
		if (Path.find("/api/v1/contact") != std::string::npos) return "contact";
		if (Path.find("/api/v1/location") != std::string::npos) return "location";
		if (Path.find("/api/v1/map") != std::string::npos) return "map";
		if (Path.find("/api/v1/variables") != std::string::npos) return "variables";
		if (Path.find("/api/v1/radiusEndpoint") != std::string::npos) return "radiusEndpoint";
		if (Path.find("/api/v1/subscriber") != std::string::npos) return "subscriber";
		if (Path.find("/api/v1/sub_devices") != std::string::npos) return "sub_devices";
		if (Path.find("/api/v1/openroaming") != std::string::npos) return "openroaming";
		if (Path.find("/api/v1/serviceClass") != std::string::npos) return "serviceClass";
		if (Path.find("/api/v1/overrides") != std::string::npos) return "overrides";
		if (Path.find("/api/v1/iptocountry") != std::string::npos) return "iptocountry";
		return "";
	}

	void RESTAPIHandler::AutoCreateCreatorRole(const std::string &CreatedEntityId, const std::string &CreatedVenueId, const std::string &ParentEntityId, const std::string &ParentVenueId) {
		if (UserInfo_.userinfo.userRole == SecurityObjects::ROOT || UserInfo_.userinfo.userRole == SecurityObjects::SYSTEM) {
			return;
		}

		ProvObjects::ManagementRole ParentRole;
		bool Found = false;
		if (!ParentVenueId.empty()) {
			Found = FindExistingRole(UserInfo_.userinfo.id, ParentEntityId, ParentVenueId, ParentRole);
		}
		if (!Found && !ParentEntityId.empty()) {
			Found = FindExistingRole(UserInfo_.userinfo.id, ParentEntityId, "", ParentRole);
		}

		if (Found) {
			ProvObjects::ManagementRole NewRole;
			NewRole.info.id = MicroServiceCreateUUID();
			NewRole.info.name = "Auto-created role for creator of " + (CreatedVenueId.empty() ? std::string("Entity") : std::string("Venue"));
			NewRole.info.description = "Grants same access policy as parent role: " + ParentRole.info.name;
			NewRole.info.created = Utils::Now();
			NewRole.info.modified = Utils::Now();
			NewRole.entity = CreatedEntityId;
			NewRole.venue = CreatedVenueId;
			NewRole.managementPolicy = ParentRole.managementPolicy;
			NewRole.users.push_back(UserInfo_.userinfo.id);

			if (StorageService()->RolesDB().CreateRecord(NewRole)) {
				poco_information(Logger(), fmt::format("AutoCreateCreatorRole: Auto-created role {} for user {} on {} (policy {})",
					NewRole.info.id, UserInfo_.userinfo.email,
					CreatedVenueId.empty() ? "entity " + CreatedEntityId : "venue " + CreatedVenueId,
					NewRole.managementPolicy));

				AuthCache::GetInstance()->Clear();

				if (!CreatedVenueId.empty()) {
					AddMembership(StorageService()->VenueDB(), &ProvObjects::Venue::managementRoles, CreatedVenueId, NewRole.info.id);
				} else if (!CreatedEntityId.empty()) {
					AddMembership(StorageService()->EntityDB(), &ProvObjects::Entity::managementRoles, CreatedEntityId, NewRole.info.id);
				}

				MoveUsage(StorageService()->PolicyDB(), StorageService()->RolesDB(), "", NewRole.managementPolicy, NewRole.info.id);
			} else {
				poco_error(Logger(), "AutoCreateCreatorRole: Failed to create role record in DB");
			}
		}
	}

} // namespace OpenWifi
