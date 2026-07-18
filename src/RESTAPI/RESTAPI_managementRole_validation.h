#pragma once

#include <string>
#include "RESTAPI/RESTAPI_rbac_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"

namespace OpenWifi {
	class RESTAPIHandler;

	bool ValidateManagementPolicyForTargetScope(
		RESTAPIHandler &handler,
		const std::string &policyId,
		const RBAC::TargetScope &targetScope
	);

	bool ValidateManagementPolicyForRole(
		RESTAPIHandler &handler,
		const std::string &policyId,
		const ProvObjects::ManagementRole &role
	);
}
