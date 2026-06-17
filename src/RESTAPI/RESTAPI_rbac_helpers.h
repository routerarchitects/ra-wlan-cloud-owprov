#pragma once

#include "framework/RESTAPI_Handler.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"

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

} // namespace OpenWifi::RBAC
