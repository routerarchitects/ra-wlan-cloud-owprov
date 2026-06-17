#include "RESTAPI_rbac_helpers.h"

#include "Poco/String.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"
#include "fmt/format.h"
#include "framework/orm.h"
#include <algorithm>

namespace OpenWifi::RBAC {

	namespace {
		bool Contains(const std::vector<std::string> &values, const std::string &target) {
			return std::find(values.begin(), values.end(), target) != values.end();
		}

		bool ResourceMatches(const std::string &candidate, const std::string &required) {
			auto c = Poco::toLower(candidate);
			auto r = Poco::toLower(required);
			if (c == r) {
				return true;
			}
			if ((c == "inventory" && r == "device") || (c == "device" && r == "inventory")) {
				return true;
			}
			return false;
		}

		bool AccessMatches(const std::vector<std::string> &access, const std::string &required) {
			auto requiredAction = Poco::toUpper(required);
			for (const auto &entry : access) {
				auto normalized = Poco::toUpper(entry);
				if (normalized == "FULL" || normalized == requiredAction) {
					return true;
				}
			}
			return false;
		}

		bool TargetInsideRoleScope(const ProvObjects::ManagementRole &role,
								   const TargetScope &targetScope) {
			if (!role.venue.empty()) {
				return !targetScope.venue.empty() && targetScope.venue == role.venue;
			}

			if (!role.entity.empty()) {
				if (!targetScope.entity.empty()) {
					return targetScope.entity == role.entity;
				}
				if (!targetScope.venue.empty()) {
					ProvObjects::Venue venue;
					if (StorageService()->VenueDB().GetRecord("id", targetScope.venue, venue)) {
						return venue.entity == role.entity;
					}
				}
			}
			return false;
		}

		bool PolicyAllows(const ProvObjects::ManagementPolicy &policy, const std::string &resource,
						  const std::string &action) {
			for (const auto &entry : policy.entries) {
				bool resourceAllowed = false;
				for (const auto &r : entry.resources) {
					if (ResourceMatches(r, resource)) {
						resourceAllowed = true;
						break;
					}
				}
				if (!resourceAllowed) {
					continue;
				}
				if (AccessMatches(entry.access, action)) {
					return true;
				}
			}
			return false;
		}

		bool CanAccessUserScope(const std::string &userId, const std::string &resourceType,
								const std::string &action, const TargetScope &targetScope) {
			ProvObjects::ManagementRoleVec roles;
			std::string targetEntity = targetScope.entity;
			if (targetEntity.empty() && !targetScope.venue.empty()) {
				ProvObjects::Venue venue;
				if (StorageService()->VenueDB().GetRecord("id", targetScope.venue, venue)) {
					targetEntity = venue.entity;
				}
			}

			if (!targetScope.venue.empty()) {
				ProvObjects::ManagementRoleVec venueRoles;
				StorageService()->RolesDB().GetRecords(
					0, 5000, venueRoles,
					fmt::format(" venue='{}' ", ORM::Escape(targetScope.venue)));
				for (const auto &role : venueRoles) {
					roles.push_back(role);
				}
			}

			if (!targetEntity.empty()) {
				ProvObjects::ManagementRoleVec entityRoles;
				StorageService()->RolesDB().GetRecords(
					0, 5000, entityRoles,
					fmt::format(" entity='{}' ", ORM::Escape(targetEntity)));
				for (const auto &role : entityRoles) {
					auto duplicate =
						std::find_if(roles.begin(), roles.end(), [&](const auto &r) {
							return r.info.id == role.info.id;
						}) != roles.end();
					if (!duplicate) {
						roles.push_back(role);
					}
				}
			}

			for (const auto &role : roles) {
				if (!Contains(role.users, userId)) {
					continue;
				}
				if (!TargetInsideRoleScope(role, targetScope)) {
					continue;
				}

				ProvObjects::ManagementPolicy policy;
				if (role.managementPolicy.empty() ||
					!StorageService()->PolicyDB().GetRecord("id", role.managementPolicy, policy)) {
					continue;
				}
				if (PolicyAllows(policy, resourceType, action)) {
					return true;
				}
			}
			return false;
		}
	} // namespace

	bool IsRootUser(const RESTAPIHandler &handler) {
		return handler.UserInfo_.userinfo.userRole == SecurityObjects::ROOT;
	}

	bool ResolveEntityScopeForAccess(const std::string &userId, const std::string &resourceType,
									 const std::string &action, std::string &entityId) {
		entityId.clear();
		if (userId.empty()) {
			return false;
		}

		ProvObjects::ManagementRoleVec roles;
		StorageService()->RolesDB().GetRecords(0, 5000, roles);
		for (const auto &role : roles) {
			if (!Contains(role.users, userId)) {
				continue;
			}

			std::string targetEntity = role.entity;
			if (targetEntity.empty() && !role.venue.empty()) {
				ProvObjects::Venue venue;
				if (StorageService()->VenueDB().GetRecord("id", role.venue, venue)) {
					targetEntity = venue.entity;
				}
			}
			if (targetEntity.empty()) {
				continue;
			}

			ProvObjects::ManagementPolicy policy;
			if (role.managementPolicy.empty() ||
				!StorageService()->PolicyDB().GetRecord("id", role.managementPolicy, policy)) {
				continue;
			}
			if (PolicyAllows(policy, resourceType, action)) {
				entityId = targetEntity;
				return true;
			}
		}
		return false;
	}

	bool HasAccess(RESTAPIHandler &handler, const std::string &resourceType,
				   const std::string &action, const TargetScope &targetScope) {
		if (IsRootUser(handler)) {
			return true;
		}
		if (handler.UserInfo_.userinfo.id.empty()) {
			return false;
		}
		return CanAccessUserScope(handler.UserInfo_.userinfo.id, resourceType, action,
								  targetScope);
	}

	bool RequireAccess(RESTAPIHandler &handler, const std::string &resourceType,
					   const std::string &action, const TargetScope &targetScope) {
		if (!HasAccess(handler, resourceType, action, targetScope)) {
			handler.UnAuthorized(RESTAPI::Errors::ACCESS_DENIED);
			return false;
		}
		return true;
	}

	bool IsScopeAllowed(RESTAPIHandler &handler, const TargetScope &targetScope) {
		if (IsRootUser(handler)) {
			return true;
		}
		if (handler.UserInfo_.userinfo.id.empty()) {
			return false;
		}
		static const std::string kList = "LIST";
		static const std::string kRead = "READ";
		return CanAccessUserScope(handler.UserInfo_.userinfo.id, "entity", kList, targetScope) ||
			   CanAccessUserScope(handler.UserInfo_.userinfo.id, "entity", kRead, targetScope) ||
			   CanAccessUserScope(handler.UserInfo_.userinfo.id, "venue", kList, targetScope) ||
			   CanAccessUserScope(handler.UserInfo_.userinfo.id, "venue", kRead, targetScope) ||
			   CanAccessUserScope(handler.UserInfo_.userinfo.id, "inventory", kList, targetScope) ||
			   CanAccessUserScope(handler.UserInfo_.userinfo.id, "inventory", kRead, targetScope) ||
			   CanAccessUserScope(handler.UserInfo_.userinfo.id, "managementPolicy", kList, targetScope) ||
			   CanAccessUserScope(handler.UserInfo_.userinfo.id, "managementRole", kList, targetScope);
	}

	bool IsEntityVisible(RESTAPIHandler &handler, const std::string &entityId) {
		return IsScopeAllowed(handler, TargetScope{entityId, ""});
	}

	bool IsVenueVisible(RESTAPIHandler &handler, const std::string &venueId) {
		ProvObjects::Venue venue;
		if (!StorageService()->VenueDB().GetRecord("id", venueId, venue)) {
			return false;
		}
		return IsScopeAllowed(handler, TargetScope{venue.entity, venueId});
	}

} // namespace OpenWifi::RBAC
