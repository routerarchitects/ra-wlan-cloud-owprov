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
		if (!StorageService()->PolicyDB().GetRecord("id", ActiveRole.managementPolicy, Policy)) {
			Reason = "Could not load management policy for the active role.";
			return false;
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

	bool RESTAPIHandler::FindExistingRole(const std::string &userId, const std::string &entityId, const std::string &venueId, ProvObjects::ManagementRole &ExistingRole) {
		// Exact match first (entity + venue)
		if (!venueId.empty()) {
			ManagementRoleDB::RecordVec Roles;
			std::string WhereClause = "entity='" + entityId + "' and venue='" + venueId + "'";
			if (StorageService()->RolesDB().GetRecords(0, 500, Roles, WhereClause)) {
				for (const auto &role : Roles) {
					for (const auto &user : role.users) {
						if (user == userId) {
							ExistingRole = role;
							return true;
						}
					}
				}
			}
		}

		// Fallback to entity-wide role
		ManagementRoleDB::RecordVec Roles;
		std::string WhereClause = "entity='" + entityId + "' and (venue='' or venue is null)";
		if (StorageService()->RolesDB().GetRecords(0, 500, Roles, WhereClause)) {
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