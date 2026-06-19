#include "RESTAPI/RESTAPI_managementRole_validation.h"
#include "framework/RESTAPI_Handler.h"
#include "RESTAPI/RESTAPI_rbac_helpers.h"
#include "StorageService.h"
#include "framework/RESTAPI_utils.h"

namespace OpenWifi {

	bool ValidateManagementPolicyForRole(
		RESTAPIHandler &handler,
		const std::string &policyId,
		const ProvObjects::ManagementRole &role
	) {
		if (policyId.empty()) {
			return true;
		}

		ProvObjects::ManagementPolicy policy;
		if (!StorageService()->PolicyDB().GetRecord("id", policyId, policy)) {
			if (handler.Response != nullptr) {
				handler.BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
			}
			return false;
		}

		if (!RBAC::HasAccess(
				handler,
				"managementPolicy",
				"READ",
				RBAC::TargetScope{policy.entity, policy.venue})) {
			if (handler.Response != nullptr) {
				handler.UnAuthorized(RESTAPI::Errors::ACCESS_DENIED);
			}
			return false;
		}

		if (policy.entity != role.entity || policy.venue != role.venue) {
			if (handler.Response != nullptr) {
				handler.BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
			}
			return false;
		}

		return true;
	}

} // namespace OpenWifi
