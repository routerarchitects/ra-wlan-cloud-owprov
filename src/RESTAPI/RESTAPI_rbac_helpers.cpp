#include "RESTAPI_rbac_helpers.h"

#include "Poco/JSON/Parser.h"
#include "Poco/String.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"
#include "fmt/format.h"
#include "framework/orm.h"
#include <algorithm>

namespace OpenWifi::RBAC {

	namespace {
		struct PolicyScope {
			bool constrained = false;
			std::string type;
			std::string entityId;
			std::string venueId;
			bool includeVenues = false;
			bool includeChildEntities = false;
			bool allowOperatorBoundaryDelegation = false;
		};

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

		bool IsEntityInPolicyChildScope(const std::string &targetEntityId,
										const std::string &ancestorEntityId) {
			if (targetEntityId.empty() || ancestorEntityId.empty()) {
				return false;
			}

			std::string currentEntityId = targetEntityId;
			bool crossedChildOperator = false;
			while (!currentEntityId.empty()) {
				if (currentEntityId == ancestorEntityId) {
					return true;
				}

				ProvObjects::Entity entity;
				if (!StorageService()->EntityDB().GetRecord("id", currentEntityId, entity)) {
					return false;
				}

				if (!entity.operatorId.empty()) {
					if (crossedChildOperator) {
						poco_debug(Poco::Logger::get("RBAC"), fmt::format(
																	 "RBAC policy child-scope deny target='{}' ancestor='{}' reason='grandchild operator boundary'",
																	 targetEntityId, ancestorEntityId));
						return false;
					}
					crossedChildOperator = true;
				}

				currentEntityId = entity.parent;
			}
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

		bool ResolveTargetEntityAndVenue(const TargetScope &targetScope,
										 std::string &targetEntityId,
										 std::string &targetVenueId) {
			targetEntityId = targetScope.entity;
			targetVenueId = targetScope.venue;
			if (targetEntityId.empty() && !targetVenueId.empty()) {
				ProvObjects::Venue venue;
				if (!StorageService()->VenueDB().GetRecord("id", targetVenueId, venue)) {
					return false;
				}
				targetEntityId = venue.entity;
			}
			return !targetEntityId.empty();
		}

		bool ParsePolicyScope(const std::string &policyString, PolicyScope &scope) {
			scope = {};
			if (policyString.empty()) {
				return true;
			}

			try {
				Poco::JSON::Parser parser;
				auto object = parser.parse(policyString).extract<Poco::JSON::Object::Ptr>();
				scope.constrained = true;
				if (object->has("type") && !object->isNull("type")) {
					scope.type = Poco::toLower(object->getValue<std::string>("type"));
				}
				if (object->has("entityId") && !object->isNull("entityId")) {
					scope.entityId = object->getValue<std::string>("entityId");
				} else if (object->has("entity") && !object->isNull("entity")) {
					scope.entityId = object->getValue<std::string>("entity");
				}
				if (object->has("venueId") && !object->isNull("venueId")) {
					scope.venueId = object->getValue<std::string>("venueId");
				} else if (object->has("venue") && !object->isNull("venue")) {
					scope.venueId = object->getValue<std::string>("venue");
				}
				if (object->has("includeVenues") && !object->isNull("includeVenues")) {
					scope.includeVenues = object->getValue<bool>("includeVenues");
				}
				if (object->has("includeChildEntities") &&
					!object->isNull("includeChildEntities")) {
					scope.includeChildEntities =
						object->getValue<bool>("includeChildEntities");
				}
				if (object->has("allowOperatorBoundaryDelegation") &&
					!object->isNull("allowOperatorBoundaryDelegation")) {
					scope.allowOperatorBoundaryDelegation =
						object->getValue<bool>("allowOperatorBoundaryDelegation");
				}
				return true;
			} catch (...) {
				scope = {};
				return false;
			}
		}

		bool PolicyScopeAllowsTarget(const std::string &policyString,
									 const TargetScope &targetScope) {
			PolicyScope scope;
			if (!ParsePolicyScope(policyString, scope) || !scope.constrained) {
				return true;
			}

			std::string targetEntityId;
			std::string targetVenueId;
			if (!ResolveTargetEntityAndVenue(targetScope, targetEntityId, targetVenueId)) {
				return false;
			}

			if (!scope.venueId.empty() || scope.type == "venue") {
				if (scope.venueId.empty() || targetVenueId.empty()) {
					return false;
				}
				return IsVenueDescendantOf(targetVenueId, scope.venueId);
			}

			if (!scope.entityId.empty() || scope.type == "entity") {
				if (scope.entityId.empty()) {
					return false;
				}
				if (targetEntityId == scope.entityId) {
					return targetVenueId.empty() || scope.includeVenues;
				}
				if (!scope.includeChildEntities ||
					!IsEntityInPolicyChildScope(targetEntityId, scope.entityId)) {
					return false;
				}
				return targetVenueId.empty() || scope.includeVenues;
			}

			return true;
		}

		bool PolicyScopeAllowsBoundaryDelegation(const std::string &policyString,
												 const TargetScope &targetScope) {
			PolicyScope scope;
			if (!ParsePolicyScope(policyString, scope) || !scope.constrained ||
				!scope.allowOperatorBoundaryDelegation) {
				return false;
			}

			std::string targetEntityId;
			std::string targetVenueId;
			if (!ResolveTargetEntityAndVenue(targetScope, targetEntityId, targetVenueId)) {
				return false;
			}

			if (!scope.venueId.empty() || scope.type == "venue") {
				if (scope.venueId.empty() || targetVenueId.empty()) {
					return false;
				}
				return targetVenueId == scope.venueId;
			}

			if (!scope.entityId.empty() || scope.type == "entity") {
				if (scope.entityId.empty() || scope.includeChildEntities ||
					targetEntityId != scope.entityId) {
					return false;
				}
				return targetVenueId.empty() || scope.includeVenues;
			}

			return false;
		}

		bool ResourceMatches(const std::string &candidate, const std::string &required) {
			auto c = Poco::toLower(candidate);
			auto r = Poco::toLower(required);
			if (c == r) {
				return true;
			}
			const auto isInventoryResource = [](const std::string &resource) {
				return resource == "inventory" || resource == "device";
			};
			if (isInventoryResource(c) && isInventoryResource(r)) {
				return true;
			}
			if (r == "subscriberdevice" && isInventoryResource(c)) {
				return true;
			}
			return false;
		}

		std::string StripKnownOwnerPrefix(const std::string &owner) {
			static const std::vector<std::string> prefixes{"opr:", "operator:", "ent:",
														   "entity:"};
			for (const auto &prefix : prefixes) {
				if (owner.rfind(prefix, 0) == 0) {
					return owner.substr(prefix.size());
				}
			}
			return owner;
		}

		bool AccessMatches(const std::vector<std::string> &access, const std::string &required) {
			auto requiredAction = Poco::toUpper(required);
			for (const auto &entry : access) {
				auto normalized = Poco::toUpper(entry);
				if (normalized == "FULL" || normalized == requiredAction) {
					return true;
				}
				if ((requiredAction == "UPDATE" && normalized == "MODIFY") ||
					(requiredAction == "MODIFY" && normalized == "UPDATE")) {
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
			if (!role.venue.empty()) {
				if (targetScope.venue.empty()) {
					return false;
				}
				return IsVenueDescendantOf(targetScope.venue, role.venue);
			}

			if (!role.entity.empty()) {
				if (!targetScope.entity.empty()) {
					return IsEntityDescendantOf(targetScope.entity, role.entity);
				}
				if (!targetScope.venue.empty()) {
					ProvObjects::Venue venue;
					if (StorageService()->VenueDB().GetRecord("id", targetScope.venue, venue)) {
						return IsEntityDescendantOf(venue.entity, role.entity);
					}
				}
			}
			return false;
		}

		bool PolicyAllows(const ProvObjects::ManagementPolicy &policy, const std::string &userId,
						  const std::string &resource, const std::string &action,
						  const TargetScope &targetScope,
						  bool requireOperatorBoundaryDelegation = false) {
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
				if (!PolicyScopeAllowsTarget(entry.policy, targetScope)) {
					continue;
				}
				if (requireOperatorBoundaryDelegation &&
					!PolicyScopeAllowsBoundaryDelegation(entry.policy, targetScope)) {
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
								const std::string &action, const TargetScope &targetScope,
								bool requireOperatorBoundaryDelegation = false) {
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
				if (PolicyAllows(policy, userId, resourceType, action, targetScope,
								 requireOperatorBoundaryDelegation)) {
					return true;
				}
			}
			return false;
		}

		bool HasExplicitRoleAccessForUser(const std::string &userId,
										  const std::string &resourceType,
										  const std::string &action,
										  const TargetScope &targetScope,
										  bool requireOperatorBoundaryDelegation = false) {
			return CanAccessUserScope(userId, resourceType, action, targetScope,
									  requireOperatorBoundaryDelegation);
		}

		bool HasVisibleScopeAccessForUser(const std::string &userId,
										  const TargetScope &targetScope,
										  bool requireOperatorBoundaryDelegation = false) {
			static const std::string kList = "LIST";
			static const std::string kRead = "READ";
			return HasExplicitRoleAccessForUser(userId, "entity", kList, targetScope,
												requireOperatorBoundaryDelegation) ||
				   HasExplicitRoleAccessForUser(userId, "entity", kRead, targetScope,
												requireOperatorBoundaryDelegation) ||
				   HasExplicitRoleAccessForUser(userId, "venue", kList, targetScope,
												requireOperatorBoundaryDelegation) ||
				   HasExplicitRoleAccessForUser(userId, "venue", kRead, targetScope,
												requireOperatorBoundaryDelegation) ||
				   HasExplicitRoleAccessForUser(userId, "operator", kList, targetScope,
												requireOperatorBoundaryDelegation) ||
				   HasExplicitRoleAccessForUser(userId, "operator", kRead, targetScope,
												requireOperatorBoundaryDelegation) ||
				   HasExplicitRoleAccessForUser(userId, "inventory", kList, targetScope,
												requireOperatorBoundaryDelegation) ||
				   HasExplicitRoleAccessForUser(userId, "inventory", kRead, targetScope,
												requireOperatorBoundaryDelegation) ||
				   HasExplicitRoleAccessForUser(userId, "subscriberDevice", kList, targetScope,
												requireOperatorBoundaryDelegation) ||
				   HasExplicitRoleAccessForUser(userId, "subscriberDevice", kRead, targetScope,
												requireOperatorBoundaryDelegation) ||
				   HasExplicitRoleAccessForUser(userId, "subscriber", kList, targetScope,
												requireOperatorBoundaryDelegation) ||
				   HasExplicitRoleAccessForUser(userId, "subscriber", kRead, targetScope,
												requireOperatorBoundaryDelegation) ||
				   HasExplicitRoleAccessForUser(userId, "managementPolicy", kList, targetScope,
												requireOperatorBoundaryDelegation) ||
				   HasExplicitRoleAccessForUser(userId, "managementPolicy", kRead, targetScope,
												requireOperatorBoundaryDelegation) ||
				   HasExplicitRoleAccessForUser(userId, "managementRole", kList, targetScope,
												requireOperatorBoundaryDelegation) ||
				   HasExplicitRoleAccessForUser(userId, "managementRole", kRead, targetScope,
												requireOperatorBoundaryDelegation);
		}

		bool ResolveTargetEntity(const TargetScope &targetScope, std::string &targetEntityId) {
			targetEntityId = targetScope.entity;
			if (targetEntityId.empty() && !targetScope.venue.empty()) {
				ProvObjects::Venue venue;
				if (!StorageService()->VenueDB().GetRecord("id", targetScope.venue, venue)) {
					return false;
				}
				targetEntityId = venue.entity;
			}
			return !targetEntityId.empty();
		}

		bool ResolveOwningOperatorForEntity(const std::string &entityId,
										   ProvObjects::Operator &ownerOperator) {
			std::string currentEntityId = entityId;
			while (!currentEntityId.empty()) {
				ProvObjects::Entity entity;
				if (!StorageService()->EntityDB().GetRecord("id", currentEntityId, entity)) {
					poco_debug(Poco::Logger::get("RBAC"),
							   fmt::format("RBAC operator-scope deny: entity '{}' not found while resolving owner for target '{}'",
										   currentEntityId, entityId));
					return false;
				}

				if (!entity.operatorId.empty()) {
					if (!StorageService()->OperatorDB().GetRecord("id", entity.operatorId,
																  ownerOperator)) {
						poco_debug(Poco::Logger::get("RBAC"),
								   fmt::format("RBAC operator-scope deny: operator '{}' not found for entity '{}'",
											   entity.operatorId, currentEntityId));
						return false;
					}
					return true;
				}

				currentEntityId = entity.parent;
			}

			poco_debug(Poco::Logger::get("RBAC"),
					   fmt::format("RBAC operator-scope deny: cannot resolve target owning operator for entity '{}'",
								   entityId));
			return false;
		}

		bool ResolveUserOperatorBoundary(const SecurityObjects::UserInfo &user,
										 const TargetScope &targetScope,
										 bool &insideBoundary) {
			insideBoundary = false;

			ProvObjects::Operator actorOperator;
			if (!ResolveUserOwnerOperator(user, actorOperator)) {
				poco_debug(Poco::Logger::get("RBAC"),
						   fmt::format("RBAC operator-scope deny: cannot resolve actor operator user='{}' owner='{}'",
									   user.id, user.owner));
				return false;
			}

			std::string targetEntityId;
			if (!ResolveTargetEntity(targetScope, targetEntityId)) {
				poco_debug(Poco::Logger::get("RBAC"),
						   fmt::format("RBAC operator-scope deny: cannot resolve target entity user='{}' owner='{}' entity='{}' venue='{}'",
									   user.id, user.owner, targetScope.entity,
									   targetScope.venue));
				return false;
			}

			ProvObjects::Operator targetOperator;
			if (!ResolveOwningOperatorForEntity(targetEntityId, targetOperator)) {
				poco_debug(Poco::Logger::get("RBAC"),
						   fmt::format("RBAC operator-scope deny: cannot resolve target owning operator actorOperator='{}' actorEntity='{}' targetEntity='{}'",
									   actorOperator.info.id, actorOperator.entityId,
									   targetEntityId));
				return false;
			}

			poco_debug(Poco::Logger::get("RBAC"),
					   fmt::format("RBAC operator-scope check actorOperator='{}' actorEntity='{}' targetEntity='{}' targetOperator='{}' targetParentOperator='{}'",
								   actorOperator.info.id, actorOperator.entityId, targetEntityId,
								   targetOperator.info.id, targetOperator.parentOperatorId));

			if (targetOperator.info.id == actorOperator.info.id) {
				poco_debug(Poco::Logger::get("RBAC"),
						   "RBAC operator-scope allow: same operator");
				insideBoundary = true;
				return true;
			}

			if (targetOperator.parentOperatorId == actorOperator.info.id) {
				poco_debug(Poco::Logger::get("RBAC"),
						   "RBAC operator-scope allow: direct child operator");
				insideBoundary = true;
				return true;
			}

			poco_debug(Poco::Logger::get("RBAC"),
					   "RBAC operator-scope outside-boundary: target operator is not actor or direct child");
			return true;
		}

		bool ResolveScopeFromVenue(const std::string &venueId, TargetScope &scope) {
			if (venueId.empty()) {
				return false;
			}

			ProvObjects::Venue venue;
			if (!StorageService()->VenueDB().GetRecord("id", venueId, venue)) {
				return false;
			}
			if (venue.entity.empty()) {
				return false;
			}

			scope.entity = venue.entity;
			scope.venue = venue.info.id;
			return true;
		}

		bool ResolveScopeFromInventoryRecord(const ProvObjects::InventoryTag &inventory,
											TargetScope &scope) {
			if (!inventory.venue.empty() && ResolveScopeFromVenue(inventory.venue, scope)) {
				return true;
			}

			if (!inventory.entity.empty()) {
				scope.entity = inventory.entity;
				scope.venue.clear();
				return true;
			}

			return false;
		}

		bool ResolveScopeFromManagementPolicy(const std::string &policyId, TargetScope &scope) {
			if (policyId.empty()) {
				return false;
			}

			ProvObjects::ManagementPolicy policy;
			if (!StorageService()->PolicyDB().GetRecord("id", policyId, policy)) {
				return false;
			}

			if (!policy.venue.empty() && ResolveScopeFromVenue(policy.venue, scope)) {
				return true;
			}

			if (!policy.entity.empty()) {
				scope.entity = policy.entity;
				scope.venue.clear();
				return true;
			}

			return false;
		}

		template <typename MatchFn>
		bool ResolveScopeFromInventoryMatches(MatchFn &&match, TargetScope &scope) {
			bool found = false;
			StorageService()->InventoryDB().Iterate(
				[&](const ProvObjects::InventoryTag &inventory) {
					if (!match(inventory)) {
						return true;
					}
					found = ResolveScopeFromInventoryRecord(inventory, scope);
					return !found;
				});
			return found;
		}

		template <typename EntityMatchFn, typename VenueMatchFn>
		bool ResolveScopeFromEntityVenueMatches(EntityMatchFn &&entityMatch,
												VenueMatchFn &&venueMatch,
												TargetScope &scope) {
			bool found = false;

			StorageService()->EntityDB().Iterate([&](const ProvObjects::Entity &entity) {
				if (!entityMatch(entity)) {
					return true;
				}
				scope.entity = entity.info.id;
				scope.venue.clear();
				found = true;
				return false;
			});
			if (found) {
				return true;
			}

			StorageService()->VenueDB().Iterate([&](const ProvObjects::Venue &venue) {
				if (!venueMatch(venue)) {
					return true;
				}
				if (venue.entity.empty()) {
					return true;
				}
				scope.entity = venue.entity;
				scope.venue = venue.info.id;
				found = true;
				return false;
			});
			return found;
		}
	} // namespace

	bool IsRootUser(const RESTAPIHandler &handler) {
		return handler.UserInfo_.userinfo.userRole == SecurityObjects::ROOT;
	}

	bool ResolveUserOwnerOperator(const SecurityObjects::UserInfo &user,
								  ProvObjects::Operator &ownerOperator) {
		if (user.owner.empty()) {
			return false;
		}

		const auto ownerId = StripKnownOwnerPrefix(user.owner);
		if (StorageService()->OperatorDB().GetRecord("id", ownerId, ownerOperator) &&
			!ownerOperator.entityId.empty()) {
			return true;
		}

		ProvObjects::Entity ownerEntity;
		if (StorageService()->EntityDB().GetRecord("id", ownerId, ownerEntity) &&
			!ownerEntity.operatorId.empty() &&
			StorageService()->OperatorDB().GetRecord("id", ownerEntity.operatorId,
													 ownerOperator) &&
			!ownerOperator.entityId.empty()) {
			return true;
		}

		return false;
	}

	bool ResolveUserOwnerEntity(const SecurityObjects::UserInfo &user,
								std::string &ownerEntityId) {
		ownerEntityId.clear();

		ProvObjects::Operator ownerOperator;
		if (ResolveUserOwnerOperator(user, ownerOperator)) {
			ownerEntityId = ownerOperator.entityId;
			return true;
		}

		const auto ownerId = StripKnownOwnerPrefix(user.owner);
		ProvObjects::Entity ownerEntity;
		if (StorageService()->EntityDB().GetRecord("id", ownerId, ownerEntity)) {
			ownerEntityId = ownerEntity.info.id;
			return true;
		}

		return false;
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
				if (PolicyAllows(policy, userId, resourceType, action,
								 TargetScope{targetEntity, role.venue})) {
					entityId = targetEntity;
					found = true;
					return false;
				}
				return true;
			});
		return found;
	}

	bool ResolveInventoryScope(const std::string &serialNumber, TargetScope &scope) {
		scope = {};
		if (serialNumber.empty()) {
			return false;
		}

		ProvObjects::InventoryTag inventory;
		if (!StorageService()->InventoryDB().GetRecord("serialNumber", serialNumber, inventory)) {
			return false;
		}

		if (ResolveScopeFromInventoryRecord(inventory, scope)) {
			return true;
		}

		if (ResolveScopeFromManagementPolicy(inventory.managementPolicy, scope)) {
			return true;
		}

		return ResolveScopeFromEntityVenueMatches(
			[&](const ProvObjects::Entity &entity) {
				return Contains(entity.devices, inventory.info.id);
			},
			[&](const ProvObjects::Venue &venue) { return Contains(venue.devices, inventory.info.id); },
			scope);
	}

	bool ResolveConfigurationOverrideScope(const std::string &serialNumber, TargetScope &scope) {
		scope = {};
		if (serialNumber.empty()) {
			return false;
		}

		ProvObjects::ConfigurationOverrideList overrides;
		if (!StorageService()->OverridesDB().GetRecord("serialNumber", serialNumber, overrides)) {
			return false;
		}

		if (ResolveScopeFromManagementPolicy(overrides.managementPolicy, scope)) {
			return true;
		}

		return ResolveInventoryScope(serialNumber, scope);
	}

	bool ResolveManagementPolicyScope(const std::string &policyId, TargetScope &scope) {
		scope = {};
		return ResolveScopeFromManagementPolicy(policyId, scope);
	}

	bool ResolveConfigurationScope(const std::string &uuid, TargetScope &scope) {
		scope = {};
		if (uuid.empty()) {
			return false;
		}

		ProvObjects::DeviceConfiguration configuration;
		if (!StorageService()->ConfigurationDB().GetRecord("id", uuid, configuration)) {
			return false;
		}

		if (!configuration.venue.empty() && ResolveScopeFromVenue(configuration.venue, scope)) {
			return true;
		}

		if (!configuration.entity.empty()) {
			scope.entity = configuration.entity;
			scope.venue.clear();
			return true;
		}

		if (ResolveScopeFromManagementPolicy(configuration.managementPolicy, scope)) {
			return true;
		}

		if (ResolveScopeFromInventoryMatches(
				[&](const ProvObjects::InventoryTag &inventory) {
					return inventory.deviceConfiguration == configuration.info.id;
				},
				scope)) {
			return true;
		}

		return ResolveScopeFromEntityVenueMatches(
			[&](const ProvObjects::Entity &entity) {
				return Contains(entity.configurations, configuration.info.id);
			},
			[&](const ProvObjects::Venue &venue) {
				return Contains(venue.configurations, configuration.info.id);
			},
			scope);
	}

	bool ResolveContactScope(const std::string &uuid, TargetScope &scope) {
		scope = {};
		if (uuid.empty()) {
			return false;
		}

		ProvObjects::Contact contact;
		if (!StorageService()->ContactDB().GetRecord("id", uuid, contact)) {
			return false;
		}

		if (!contact.entity.empty()) {
			scope.entity = contact.entity;
			scope.venue.clear();
			return true;
		}

		if (ResolveScopeFromManagementPolicy(contact.managementPolicy, scope)) {
			return true;
		}

		if (ResolveScopeFromInventoryMatches(
				[&](const ProvObjects::InventoryTag &inventory) {
					return inventory.contact == contact.info.id;
				},
				scope)) {
			return true;
		}

		return ResolveScopeFromEntityVenueMatches(
			[&](const ProvObjects::Entity &entity) {
				return Contains(entity.contacts, contact.info.id);
			},
			[&](const ProvObjects::Venue &venue) {
				return Contains(venue.contacts, contact.info.id);
			},
			scope);
	}

	bool ResolveLocationScope(const std::string &uuid, TargetScope &scope) {
		scope = {};
		if (uuid.empty()) {
			return false;
		}

		ProvObjects::Location location;
		if (!StorageService()->LocationDB().GetRecord("id", uuid, location)) {
			return false;
		}

		if (!location.entity.empty()) {
			scope.entity = location.entity;
			scope.venue.clear();
			return true;
		}

		if (ResolveScopeFromManagementPolicy(location.managementPolicy, scope)) {
			return true;
		}

		if (ResolveScopeFromInventoryMatches(
				[&](const ProvObjects::InventoryTag &inventory) {
					return inventory.location == location.info.id;
				},
				scope)) {
			return true;
		}

		return ResolveScopeFromEntityVenueMatches(
			[&](const ProvObjects::Entity &entity) {
				return Contains(entity.locations, location.info.id);
			},
			[&](const ProvObjects::Venue &venue) { return venue.location == location.info.id; },
			scope);
	}

	bool ResolveVariableScope(const std::string &uuid, TargetScope &scope) {
		scope = {};
		if (uuid.empty()) {
			return false;
		}

		ProvObjects::VariableBlock variable;
		if (!StorageService()->VariablesDB().GetRecord("id", uuid, variable)) {
			return false;
		}

		if (!variable.venue.empty() && ResolveScopeFromVenue(variable.venue, scope)) {
			return true;
		}

		if (!variable.entity.empty()) {
			scope.entity = variable.entity;
			scope.venue.clear();
			return true;
		}

		if (ResolveScopeFromManagementPolicy(variable.managementPolicy, scope)) {
			return true;
		}

		if (!variable.inventory.empty() && ResolveInventoryScope(variable.inventory, scope)) {
			return true;
		}

		if (ResolveScopeFromEntityVenueMatches(
				[&](const ProvObjects::Entity &entity) {
					return Contains(entity.variables, variable.info.id);
				},
				[&](const ProvObjects::Venue &venue) { return Contains(venue.variables, variable.info.id); },
				scope)) {
			return true;
		}

		bool found = false;
		StorageService()->ConfigurationDB().Iterate([&](const ProvObjects::DeviceConfiguration &configuration) {
			if (!Contains(configuration.variables, variable.info.id)) {
				return true;
			}
			found = ResolveConfigurationScope(configuration.info.id, scope);
			return !found;
		});
		return found;
	}

	bool ResolveMapScope(const std::string &uuid, TargetScope &scope) {
		scope = {};
		if (uuid.empty()) {
			return false;
		}

		ProvObjects::Map map;
		if (!StorageService()->MapDB().GetRecord("id", uuid, map)) {
			return false;
		}

		if (!map.venue.empty() && ResolveScopeFromVenue(map.venue, scope)) {
			return true;
		}

		if (!map.entity.empty()) {
			scope.entity = map.entity;
			scope.venue.clear();
			return true;
		}

		if (ResolveScopeFromManagementPolicy(map.managementPolicy, scope)) {
			return true;
		}

		return ResolveScopeFromEntityVenueMatches(
			[&](const ProvObjects::Entity &entity) { return Contains(entity.maps, map.info.id); },
			[&](const ProvObjects::Venue &venue) { return Contains(venue.maps, map.info.id); },
			scope);
	}

	bool ResolveSubscriberScope(const std::string &subscriberId, TargetScope &scope) {
		scope = {};
		if (subscriberId.empty()) {
			return false;
		}

		ProvObjects::Venue venue;
		if (!StorageService()->VenueDB().GetRecord("subscriber", subscriberId, venue) ||
			venue.entity.empty()) {
			return false;
		}

		scope.entity = venue.entity;
		scope.venue = venue.info.id;
		return true;
	}

	bool ResolveSubscriberDeviceScope(const ProvObjects::SubscriberDevice &device,
									  TargetScope &scope) {
		scope = {};
		if (ResolveInventoryScope(device.serialNumber, scope)) {
			return true;
		}

		if (ResolveSubscriberScope(device.subscriberId, scope)) {
			return true;
		}

		if (ResolveScopeFromManagementPolicy(device.managementPolicy, scope)) {
			return true;
		}

		ProvObjects::Operator op;
		if (!device.operatorId.empty() &&
			StorageService()->OperatorDB().GetRecord("id", device.operatorId, op) &&
			!op.entityId.empty()) {
			scope.entity = op.entityId;
			scope.venue.clear();
			return true;
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
		bool insideOperatorBoundary = false;
		if (!ResolveUserOperatorBoundary(handler.UserInfo_.userinfo, targetScope,
										 insideOperatorBoundary)) {
			poco_debug(handler.Logger(),
					   fmt::format("RBAC operator-scope unresolved deny user='{}' owner='{}' resource='{}' action='{}' entity='{}' venue='{}'",
								   handler.UserInfo_.userinfo.id,
								   handler.UserInfo_.userinfo.owner,
								   resourceType, action, targetScope.entity,
								   targetScope.venue));
			return false;
		}
		const bool requireOperatorBoundaryDelegation = !insideOperatorBoundary;
		auto allowed = HasExplicitRoleAccessForUser(handler.UserInfo_.userinfo.id, resourceType,
													action, targetScope,
													requireOperatorBoundaryDelegation);
		if (allowed) {
			poco_debug(handler.Logger(),
					   fmt::format("RBAC explicit access allow user='{}' resource='{}' action='{}' delegated={}",
								   handler.UserInfo_.userinfo.id, resourceType, action,
								   requireOperatorBoundaryDelegation));
			return true;
		}
		poco_debug(handler.Logger(),
				   fmt::format("RBAC has-access result user='{}' resource='{}' action='{}' delegated={} allowed={}",
							   handler.UserInfo_.userinfo.id, resourceType, action,
							   requireOperatorBoundaryDelegation, allowed));
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
		bool insideOperatorBoundary = false;
		if (!ResolveUserOperatorBoundary(handler.UserInfo_.userinfo, targetScope,
										 insideOperatorBoundary)) {
			poco_debug(handler.Logger(),
					   fmt::format("RBAC operator-scope-visible unresolved deny user='{}' owner='{}' entity='{}' venue='{}'",
								   handler.UserInfo_.userinfo.id,
								   handler.UserInfo_.userinfo.owner,
								   targetScope.entity, targetScope.venue));
			return false;
		}
		const bool requireOperatorBoundaryDelegation = !insideOperatorBoundary;
		const bool allowed =
			HasVisibleScopeAccessForUser(handler.UserInfo_.userinfo.id, targetScope,
										 requireOperatorBoundaryDelegation);
		if (allowed) {
			poco_debug(handler.Logger(),
					   fmt::format("RBAC explicit scope-visible allow user='{}' entity='{}' venue='{}' delegated={}",
								   handler.UserInfo_.userinfo.id, targetScope.entity,
								   targetScope.venue, requireOperatorBoundaryDelegation));
			return true;
		}
		poco_debug(handler.Logger(),
				   fmt::format("RBAC scope-allowed result user='{}' entity='{}' venue='{}' delegated={} allowed={}",
							   handler.UserInfo_.userinfo.id, targetScope.entity,
							   targetScope.venue, requireOperatorBoundaryDelegation,
							   allowed));
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
