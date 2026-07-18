//
// Created by stephane bourque on 2021-08-26.
//

#include "RESTAPI_managementPolicy_list_handler.h"

#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTAPI/RESTAPI_rbac_helpers.h"
#include "RESTAPI/RESTAPI_list_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"

namespace OpenWifi {
	void RESTAPI_managementPolicy_list_handler::DoGet() {
		ProvObjects::ManagementPolicyVec policies;
		auto total = DB_.Count();
		if (total > 0) {
			DB_.GetRecords(0, total, policies);
		}

		if (!RBAC::IsRootUser(*this)) {
			policies = RESTAPI::FilterRecords(
				policies,
				[&](const auto &policy) {
					const auto scope = RBAC::TargetScope{policy.entity, policy.venue};
					return RBAC::HasAccess(*this, "managementPolicy", "LIST", scope) ||
						   RBAC::HasAccess(*this, "managementPolicy", "READ", scope);
				});
		}

		if (QB_.CountOnly) {
			return ReturnCountOnly(policies.size());
		}

		auto page = RESTAPI::ApplyPagination(policies, QB_.Offset, QB_.Limit);
		return MakeJSONObjectArray("managementPolicies", page, *this);
	}
} // namespace OpenWifi
