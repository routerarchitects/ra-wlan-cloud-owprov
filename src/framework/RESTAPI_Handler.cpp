//
// Created by stephane bourque on 2022-10-25.
//

#include "RESTAPI_Handler.h"
#include "StorageService.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"

namespace OpenWifi {

	bool RESTAPIHandler::RoleIsAuthorized(const std::string &Path,
										  const std::string &Method,
										  std::string &Reason) {
		// 1. Bypass check if user is root/system
		if (UserInfo_.userinfo.userRole == SecurityObjects::ROOT || UserInfo_.userinfo.userRole == SecurityObjects::SYSTEM) {
			return true;
		}

		// 2. Map path to resource
		std::string Resource = GetResourceName(Path);
		if (Resource.empty()) {
			// Allow non-scoped resources by default to maintain existing routing compatibility
			return true;
		}

		// 3. Resolve target Entity and Venue
		std::string TargetEntity, TargetVenue;
		if (!ResolveTargetContext(Path, Method, TargetEntity, TargetVenue)) {
			// Fallback: If target context cannot be resolved and it's a GET request, check if the user has ANY role that permits reading this resource.
			if (Method == Poco::Net::HTTPRequest::HTTP_GET) {
				ProvObjects::ManagementRole AnyRole;
				if (FindAnyRole(UserInfo_.userinfo.id, AnyRole)) {
					ProvObjects::ManagementPolicy Policy;
					if (AuthCache::GetInstance()->GetPolicy(AnyRole.managementPolicy, Policy) ||
						(StorageService()->PolicyDB().GetRecord("id", AnyRole.managementPolicy, Policy) && 
						 (AuthCache::GetInstance()->SetPolicy(AnyRole.managementPolicy, Policy), true))) {
						if (PolicyAllows(Policy, Resource, Method)) {
							return true;
						}
					}
				}
			}
			Reason = "Target context could not be resolved.";
			return false;
		}

		// 4. Find most specific role
		ProvObjects::ManagementRole ActiveRole;
		std::string UserId = UserInfo_.userinfo.id;
		if (!FindExistingRole(UserId, TargetEntity, TargetVenue, ActiveRole)) {
			Reason = "No matching role found for the user-entity-venue scope.";
			return false;
		}

		// 5. Load fixed policy
		ProvObjects::ManagementPolicy Policy;
		if (!AuthCache::GetInstance()->GetPolicy(ActiveRole.managementPolicy, Policy)) {
			if (!StorageService()->PolicyDB().GetRecord("id", ActiveRole.managementPolicy, Policy)) {
				Reason = "Could not load management policy for the active role.";
				return false;
			}
			AuthCache::GetInstance()->SetPolicy(ActiveRole.managementPolicy, Policy);
		}

		// 6. Check policy permissions
		if (!PolicyAllows(Policy, Resource, Method)) {
			Reason = "Policy does not permit this resource and operation.";
			return false;
		}

		return true;
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

		// 2. Check query parameters
		for (const auto &[name, value] : Parameters_) {
			if (name == "entity") {
				TargetEntity = value;
			} else if (name == "venue") {
				TargetVenue = value;
			}
		}

		// 3. If ID is provided, figure out the resource type from the Path
		if (!Id.empty() && TargetEntity.empty() && TargetVenue.empty()) {
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
		}

		// 4. Check body if parsed body exists
		if (TargetEntity.empty() && TargetVenue.empty() && ParsedBody_) {
			if (ParsedBody_->has("entity")) {
				TargetEntity = ParsedBody_->get("entity").toString();
			}
			if (ParsedBody_->has("venue")) {
				TargetVenue = ParsedBody_->get("venue").toString();
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

		// Find exact match first (entity + venue)
		if (!venueId.empty()) {
			for (const auto &role : Roles) {
				if (role.entity == entityId && role.venue == venueId) {
					ExistingRole = role;
					return true;
				}
			}
		}

		// Fallback to entity-wide role
		for (const auto &role : Roles) {
			if (role.entity == entityId && (role.venue.empty() || role.venue == "")) {
				ExistingRole = role;
				return true;
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
		return "";
	}

} // namespace OpenWifi