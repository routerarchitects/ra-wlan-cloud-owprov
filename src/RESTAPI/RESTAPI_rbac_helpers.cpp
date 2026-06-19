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

		bool IsEntityDescendantOf(const std::string &targetEntityId,
								  const std::string &ancestorEntityId) {
			if (targetEntityId.empty() || ancestorEntityId.empty()) {
				return false;
			}

			poco_debug(Poco::Logger::get("RBAC"), fmt::format(
														 "RBAC entity-scope check start target='{}' ancestor='{}'",
														 targetEntityId, ancestorEntityId));

			std::string currentEntityId = targetEntityId;
			while (!currentEntityId.empty()) {
				poco_debug(Poco::Logger::get("RBAC"),
						   fmt::format("RBAC entity-scope hop current='{}' ancestor='{}'",
									   currentEntityId, ancestorEntityId));
				if (currentEntityId == ancestorEntityId) {
					poco_debug(Poco::Logger::get("RBAC"), fmt::format(
																 "RBAC entity-scope match target='{}' ancestor='{}'",
																 targetEntityId, ancestorEntityId));
					return true;
				}

				ProvObjects::Entity entity;
				if (!StorageService()->EntityDB().GetRecord("id", currentEntityId, entity)) {
					poco_debug(Poco::Logger::get("RBAC"), fmt::format(
																 "RBAC entity-scope stop target='{}' current='{}' reason='entity not found'",
																 targetEntityId, currentEntityId));
					return false;
				}
				currentEntityId = entity.parent;
			}
			poco_debug(Poco::Logger::get("RBAC"), fmt::format(
														 "RBAC entity-scope miss target='{}' ancestor='{}'",
														 targetEntityId, ancestorEntityId));
			return false;
		}

		bool IsVenueDescendantOf(const std::string &targetVenueId,
								const std::string &ancestorVenueId) {
			if (targetVenueId.empty() || ancestorVenueId.empty()) {
				return false;
			}

			poco_debug(Poco::Logger::get("RBAC"), fmt::format(
														 "RBAC venue-scope check start target='{}' ancestor='{}'",
														 targetVenueId, ancestorVenueId));

			std::string currentVenueId = targetVenueId;
			while (!currentVenueId.empty()) {
				poco_debug(Poco::Logger::get("RBAC"),
						   fmt::format("RBAC venue-scope hop current='{}' ancestor='{}'",
									   currentVenueId, ancestorVenueId));
				if (currentVenueId == ancestorVenueId) {
					poco_debug(Poco::Logger::get("RBAC"), fmt::format(
																 "RBAC venue-scope match target='{}' ancestor='{}'",
																 targetVenueId, ancestorVenueId));
					return true;
				}

				ProvObjects::Venue venue;
				if (!StorageService()->VenueDB().GetRecord("id", currentVenueId, venue)) {
					poco_debug(Poco::Logger::get("RBAC"), fmt::format(
																 "RBAC venue-scope stop target='{}' current='{}' reason='venue not found'",
																 targetVenueId, currentVenueId));
					return false;
				}
				currentVenueId = venue.parent;
			}
			poco_debug(Poco::Logger::get("RBAC"), fmt::format(
														 "RBAC venue-scope miss target='{}' ancestor='{}'",
														 targetVenueId, ancestorVenueId));
			return false;
		}

		bool IsEntityDescendantWithinOperatorDepth(const std::string &targetEntityId,
												 const std::string &ancestorEntityId,
												 std::size_t maxOperatorBoundaries) {
			if (targetEntityId.empty() || ancestorEntityId.empty()) {
				return false;
			}

			std::size_t operatorBoundaries = 0;
			std::string currentEntityId = targetEntityId;
			while (!currentEntityId.empty()) {
				if (currentEntityId == ancestorEntityId) {
					poco_debug(Poco::Logger::get("RBAC"), fmt::format(
																 "RBAC entity-depth match target='{}' ancestor='{}' operatorBoundaries={}",
																 targetEntityId, ancestorEntityId,
																 operatorBoundaries));
					return operatorBoundaries <= maxOperatorBoundaries;
				}

				ProvObjects::Entity entity;
				if (!StorageService()->EntityDB().GetRecord("id", currentEntityId, entity)) {
					poco_debug(Poco::Logger::get("RBAC"), fmt::format(
																 "RBAC entity-depth stop target='{}' current='{}' reason='entity not found'",
																 targetEntityId, currentEntityId));
					return false;
				}

				if (!entity.operatorId.empty()) {
					++operatorBoundaries;
				}
				if (operatorBoundaries > maxOperatorBoundaries) {
					poco_debug(Poco::Logger::get("RBAC"), fmt::format(
																 "RBAC entity-depth deny target='{}' ancestor='{}' operatorBoundaries={} maxAllowed={}",
																 targetEntityId, ancestorEntityId,
																 operatorBoundaries, maxOperatorBoundaries));
					return false;
				}

				currentEntityId = entity.parent;
			}

			poco_debug(Poco::Logger::get("RBAC"), fmt::format(
														 "RBAC entity-depth miss target='{}' ancestor='{}' operatorBoundaries={}",
														 targetEntityId, ancestorEntityId, operatorBoundaries));
			return false;
		}

		bool IsEntityWithinOperatorChainDepth(const std::string &entityId,
											 std::size_t maxOperatorDepth) {
			if (entityId.empty()) {
				return false;
			}

			std::size_t operatorDepth = 0;
			std::string currentEntityId = entityId;
			while (!currentEntityId.empty()) {
				ProvObjects::Entity entity;
				if (!StorageService()->EntityDB().GetRecord("id", currentEntityId, entity)) {
					poco_debug(Poco::Logger::get("RBAC"), fmt::format(
																 "RBAC operator-depth stop entity='{}' current='{}' reason='entity not found'",
																 entityId, currentEntityId));
					return false;
				}

				if (!entity.operatorId.empty()) {
					++operatorDepth;
					poco_debug(Poco::Logger::get("RBAC"), fmt::format(
																 "RBAC operator-depth hop entity='{}' current='{}' operatorDepth={}",
																 entityId, currentEntityId, operatorDepth));
					if (operatorDepth > maxOperatorDepth) {
						poco_debug(Poco::Logger::get("RBAC"), fmt::format(
																	 "RBAC operator-depth deny entity='{}' current='{}' operatorDepth={} maxAllowed={}",
																	 entityId, currentEntityId, operatorDepth,
																	 maxOperatorDepth));
						return false;
					}
				}

				currentEntityId = entity.parent;
			}

			poco_debug(Poco::Logger::get("RBAC"), fmt::format(
														 "RBAC operator-depth allow entity='{}' operatorDepth={} maxAllowed={}",
														 entityId, operatorDepth, maxOperatorDepth));
			return true;
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

		bool EntryAppliesToUser(const ProvObjects::ManagementPolicyEntry &entry,
								const std::string &userId) {
			return entry.users.empty() || Contains(entry.users, userId);
		}

		bool TargetInsideRoleScope(const ProvObjects::ManagementRole &role,
								   const TargetScope &targetScope) {
			static constexpr std::size_t kMaxOperatorChainDepth = 3;

			if (!role.venue.empty()) {
				if (targetScope.venue.empty()) {
					return false;
				}
				ProvObjects::Venue targetVenue;
				if (!StorageService()->VenueDB().GetRecord("id", targetScope.venue, targetVenue)) {
					return false;
				}
				return IsVenueDescendantOf(targetScope.venue, role.venue) &&
					   IsEntityWithinOperatorChainDepth(targetVenue.entity,
														kMaxOperatorChainDepth);
			}

			if (!role.entity.empty()) {
				if (!targetScope.entity.empty()) {
					return IsEntityDescendantWithinOperatorDepth(targetScope.entity, role.entity, 1) &&
						   IsEntityWithinOperatorChainDepth(targetScope.entity,
														kMaxOperatorChainDepth);
				}
				if (!targetScope.venue.empty()) {
					ProvObjects::Venue venue;
					if (StorageService()->VenueDB().GetRecord("id", targetScope.venue, venue)) {
						return IsEntityDescendantWithinOperatorDepth(venue.entity, role.entity, 1) &&
							   IsEntityWithinOperatorChainDepth(venue.entity,
																kMaxOperatorChainDepth);
					}
				}
			}
			return false;
		}

		bool PolicyAllows(const ProvObjects::ManagementPolicy &policy, const std::string &userId,
						  const std::string &resource, const std::string &action) {
			for (const auto &entry : policy.entries) {
				if (!EntryAppliesToUser(entry, userId)) {
					continue;
				}
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

		void AddUniqueRole(ProvObjects::ManagementRoleVec &roles,
						   const ProvObjects::ManagementRole &role) {
			auto duplicate = std::find_if(roles.begin(), roles.end(), [&](const auto &r) {
				return r.info.id == role.info.id;
			}) != roles.end();
			if (!duplicate) {
				roles.push_back(role);
			}
		}

		void CollectRolesForEntityScope(const std::string &entityId,
									   ProvObjects::ManagementRoleVec &roles) {
			std::string currentEntityId = entityId;
			while (!currentEntityId.empty()) {
				poco_debug(Poco::Logger::get("RBAC"),
						   fmt::format("RBAC collect-entity-role-scope current='{}'", currentEntityId));
				std::size_t beforeCount = roles.size();
				ProvObjects::Entity entity;
				if (!StorageService()->EntityDB().GetRecord("id", currentEntityId, entity)) {
					poco_debug(Poco::Logger::get("RBAC"), fmt::format(
																 "RBAC collect-entity-role-scope stop current='{}' reason='entity not found'",
																 currentEntityId));
					break;
				}

				StorageService()->RolesDB().Iterate(
					[&](const ProvObjects::ManagementRole &role) {
						if (role.entity == currentEntityId) {
							AddUniqueRole(roles, role);
						}
						return true;
					},
					fmt::format(" entity='{}' ", ORM::Escape(currentEntityId)));
				poco_debug(Poco::Logger::get("RBAC"),
						   fmt::format("RBAC collect-entity-role-scope current='{}' added={}",
									   currentEntityId, roles.size() - beforeCount));
				poco_debug(Poco::Logger::get("RBAC"), fmt::format(
														 "RBAC collect-entity-role-scope current='{}' parent='{}'",
														 currentEntityId, entity.parent));
				currentEntityId = entity.parent;
			}
		}

		void CollectRolesForVenueScope(const std::string &venueId,
									   ProvObjects::ManagementRoleVec &roles) {
			std::string currentVenueId = venueId;
			while (!currentVenueId.empty()) {
				poco_debug(Poco::Logger::get("RBAC"),
						   fmt::format("RBAC collect-venue-role-scope current='{}'", currentVenueId));
				std::size_t beforeCount = roles.size();
				StorageService()->RolesDB().Iterate(
					[&](const ProvObjects::ManagementRole &role) {
						if (role.venue == currentVenueId) {
							AddUniqueRole(roles, role);
						}
						return true;
					},
					fmt::format(" venue='{}' ", ORM::Escape(currentVenueId)));
				poco_debug(Poco::Logger::get("RBAC"),
						   fmt::format("RBAC collect-venue-role-scope current='{}' added={}",
									   currentVenueId, roles.size() - beforeCount));

				ProvObjects::Venue venue;
				if (!StorageService()->VenueDB().GetRecord("id", currentVenueId, venue)) {
					poco_debug(Poco::Logger::get("RBAC"), fmt::format(
																 "RBAC collect-venue-role-scope stop current='{}' reason='venue not found'",
																 currentVenueId));
					break;
				}
				poco_debug(Poco::Logger::get("RBAC"), fmt::format(
														 "RBAC collect-venue-role-scope current='{}' parent='{}'",
														 currentVenueId, venue.parent));
				currentVenueId = venue.parent;
			}
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
				CollectRolesForVenueScope(targetScope.venue, roles);
			}

			if (!targetEntity.empty()) {
				CollectRolesForEntityScope(targetEntity, roles);
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
				if (PolicyAllows(policy, userId, resourceType, action)) {
					return true;
				}
			}
			return false;
		}

		bool TargetInsideUserOwnerOperator(const SecurityObjects::UserInfo &user,
										  const TargetScope &targetScope) {
			if (user.owner.empty()) {
				return true;
			}

			ProvObjects::Operator ownerOperator;
			if (!StorageService()->OperatorDB().GetRecord("id", user.owner, ownerOperator) ||
				ownerOperator.entityId.empty()) {
				return false;
			}

			std::string targetEntity = targetScope.entity;
			if (targetEntity.empty() && !targetScope.venue.empty()) {
				ProvObjects::Venue venue;
				if (StorageService()->VenueDB().GetRecord("id", targetScope.venue, venue)) {
					targetEntity = venue.entity;
				}
			}
			if (targetEntity.empty()) {
				return false;
			}

			return IsEntityDescendantOf(targetEntity, ownerOperator.entityId);
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

		bool found = false;
		StorageService()->RolesDB().Iterate(
			[&](const ProvObjects::ManagementRole &role) {
				if (!Contains(role.users, userId)) {
					return true;
				}

				std::string targetEntity = role.entity;
				if (targetEntity.empty() && !role.venue.empty()) {
					ProvObjects::Venue venue;
					if (StorageService()->VenueDB().GetRecord("id", role.venue, venue)) {
						targetEntity = venue.entity;
					}
				}
				if (targetEntity.empty()) {
					return true;
				}

				ProvObjects::ManagementPolicy policy;
				if (role.managementPolicy.empty() ||
					!StorageService()->PolicyDB().GetRecord("id", role.managementPolicy, policy)) {
					return true;
				}
				if (PolicyAllows(policy, userId, resourceType, action)) {
					entityId = targetEntity;
					found = true;
					return false;
				}
				return true;
			});
		return found;
	}

	bool HasAccess(RESTAPIHandler &handler, const std::string &resourceType,
				   const std::string &action, const TargetScope &targetScope) {
		if (IsRootUser(handler)) {
			return true;
		}
		if (handler.UserInfo_.userinfo.id.empty()) {
			return false;
		}
		if (!TargetInsideUserOwnerOperator(handler.UserInfo_.userinfo, targetScope)) {
			poco_debug(handler.Logger(),
					   fmt::format("RBAC owner-scope deny user='{}' owner='{}' resource='{}' action='{}' entity='{}' venue='{}'",
								   handler.UserInfo_.userinfo.id,
								   handler.UserInfo_.userinfo.owner,
								   resourceType, action, targetScope.entity,
								   targetScope.venue));
			return false;
		}
		auto allowed = CanAccessUserScope(handler.UserInfo_.userinfo.id, resourceType, action,
										  targetScope);
		poco_debug(handler.Logger(),
				   fmt::format("RBAC has-access result user='{}' resource='{}' action='{}' allowed={}",
							   handler.UserInfo_.userinfo.id, resourceType, action, allowed));
		return allowed;
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
		if (!TargetInsideUserOwnerOperator(handler.UserInfo_.userinfo, targetScope)) {
			poco_debug(handler.Logger(),
					   fmt::format("RBAC owner-scope-visible deny user='{}' owner='{}' entity='{}' venue='{}'",
								   handler.UserInfo_.userinfo.id,
								   handler.UserInfo_.userinfo.owner,
								   targetScope.entity, targetScope.venue));
			return false;
		}
		static const std::string kList = "LIST";
		static const std::string kRead = "READ";
		const bool allowed =
			CanAccessUserScope(handler.UserInfo_.userinfo.id, "entity", kList, targetScope) ||
			CanAccessUserScope(handler.UserInfo_.userinfo.id, "entity", kRead, targetScope) ||
			CanAccessUserScope(handler.UserInfo_.userinfo.id, "venue", kList, targetScope) ||
			CanAccessUserScope(handler.UserInfo_.userinfo.id, "venue", kRead, targetScope) ||
			CanAccessUserScope(handler.UserInfo_.userinfo.id, "inventory", kList, targetScope) ||
			CanAccessUserScope(handler.UserInfo_.userinfo.id, "inventory", kRead, targetScope) ||
			CanAccessUserScope(handler.UserInfo_.userinfo.id, "managementPolicy", kList, targetScope) ||
			CanAccessUserScope(handler.UserInfo_.userinfo.id, "managementRole", kList, targetScope);
		poco_debug(handler.Logger(),
				   fmt::format("RBAC scope-allowed result user='{}' entity='{}' venue='{}' allowed={}",
							   handler.UserInfo_.userinfo.id, targetScope.entity,
							   targetScope.venue, allowed));
		return allowed;
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

	bool LoadOperator(const std::string &operatorId, ProvObjects::Operator &op) {
		return StorageService()->OperatorDB().GetRecord("id", operatorId, op);
	}

	bool RequireOperatorAccessForLoadedOperator(
		RESTAPIHandler &handler,
		const ProvObjects::Operator &op,
		const std::string &action
	) {
		return RequireAccess(
			handler,
			"operator",
			action,
			TargetScope{op.entityId, ""}
		);
	}

	bool RequireOperatorAccessOrNotFound(
		RESTAPIHandler &handler,
		const std::string &operatorId,
		const std::string &action
	) {
		ProvObjects::Operator op;
		if (!LoadOperator(operatorId, op)) {
			handler.NotFound();
			return false;
		}
		return RequireOperatorAccessForLoadedOperator(handler, op, action);
	}

	bool RequireOperatorAccessOrBadRequest(
		RESTAPIHandler &handler,
		const std::string &operatorId,
		const std::string &action,
		RESTAPI::Errors::msg badRequestError
	) {
		ProvObjects::Operator op;
		if (!LoadOperator(operatorId, op)) {
			handler.BadRequest(badRequestError);
			return false;
		}
		return RequireOperatorAccessForLoadedOperator(handler, op, action);
	}

	bool HasAccessForUser(const std::string &userId, const std::string &resourceType,
						  const std::string &action, const TargetScope &targetScope) {
		if (userId.empty()) {
			return false;
		}
		return CanAccessUserScope(userId, resourceType, action, targetScope);
	}

} // namespace OpenWifi::RBAC
