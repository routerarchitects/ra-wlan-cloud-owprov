#pragma once

#include <string>
#include "RESTObjects/RESTAPI_ProvObjects.h"

namespace OpenWifi {
	class RESTAPIHandler;

	bool ValidateManagementPolicyForRole(
		RESTAPIHandler &handler,
		const std::string &policyId,
		const ProvObjects::ManagementRole &role
	);
}
