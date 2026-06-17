#pragma once

#include "framework/RESTAPI_Handler.h"

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

} // namespace OpenWifi::RBAC
