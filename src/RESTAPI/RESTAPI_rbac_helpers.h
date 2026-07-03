#pragma once

#include "framework/RESTAPI_Handler.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "RESTObjects/RESTAPI_SecurityObjects.h"

namespace OpenWifi::RBAC {

	struct TargetScope {
		std::string entity;
		std::string venue;
	};

	bool IsRootUser(const RESTAPIHandler &handler);
	bool ResolveEntityScopeForAccess(const std::string &userId, const std::string &resourceType,
									 const std::string &action, std::string &entityId);
	bool HasAccess(RESTAPIHandler &handler, const std::string &resourceType,
				   const std::string &action, const TargetScope &targetScope);
	bool RequireAccess(RESTAPIHandler &handler, const std::string &resourceType,
					   const std::string &action, const TargetScope &targetScope);
	bool IsScopeAllowed(RESTAPIHandler &handler, const TargetScope &targetScope);
	bool IsEntityVisible(RESTAPIHandler &handler, const std::string &entityId);
	bool IsVenueVisible(RESTAPIHandler &handler, const std::string &venueId);
	bool ResolveUserOwnerOperator(const SecurityObjects::UserInfo &user,
								  ProvObjects::Operator &ownerOperator);
	bool ResolveUserOwnerEntity(const SecurityObjects::UserInfo &user,
								std::string &ownerEntityId);

	bool LoadOperator(
		const std::string &operatorId,
		ProvObjects::Operator &op
	);

	bool RequireOperatorAccessForLoadedOperator(
		RESTAPIHandler &handler,
		const ProvObjects::Operator &op,
		const std::string &action
	);

	bool RequireOperatorAccessOrNotFound(
		RESTAPIHandler &handler,
		const std::string &operatorId,
		const std::string &action
	);

	bool RequireOperatorAccessOrBadRequest(
		RESTAPIHandler &handler,
		const std::string &operatorId,
		const std::string &action,
		RESTAPI::Errors::msg badRequestError
	);

	bool ResolveInventoryScope(const std::string &serialNumber, TargetScope &scope);
	bool ResolveConfigurationOverrideScope(const std::string &serialNumber, TargetScope &scope);
	bool ResolveManagementPolicyScope(const std::string &policyId, TargetScope &scope);
	bool ResolveConfigurationScope(const std::string &uuid, TargetScope &scope);
	bool ResolveContactScope(const std::string &uuid, TargetScope &scope);
	bool ResolveLocationScope(const std::string &uuid, TargetScope &scope);
	bool ResolveVariableScope(const std::string &uuid, TargetScope &scope);
	bool ResolveMapScope(const std::string &uuid, TargetScope &scope);

	bool HasAccessForUser(const std::string &userId, const std::string &resourceType,
						  const std::string &action, const TargetScope &targetScope);
} // namespace OpenWifi::RBAC
