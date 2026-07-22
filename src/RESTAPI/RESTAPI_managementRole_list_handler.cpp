#include "RESTAPI_managementRole_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "StorageService.h"
#include <algorithm>

namespace OpenWifi {
	void RESTAPI_managementRole_list_handler::DoGet() {
		auto userParam = GetParameter("user", "");
		if (userParam.empty()) {
			userParam = GetParameter("userId", "");
		}
		if (userParam.empty()) {
			userParam = GetParameter("user_id", "");
		}

		if (!userParam.empty()) {
			if (UserInfo_.userinfo.userRole != SecurityObjects::ROOT) {
				SecurityObjects::UserInfo TargetUser;
				if (!SDK::Sec::User::Get(this, userParam, TargetUser)) {
					return UnAuthorized(RESTAPI::Errors::ACCESS_DENIED);
				}
			}

			ProvObjects::ManagementRoleVec Roles;
			auto lambda = [&](const ProvObjects::ManagementRole &role) {
				if (std::find(role.users.begin(), role.users.end(), userParam) != role.users.end()) {
					Roles.push_back(role);
				}
				return true;
			};
			DB_.Iterate(lambda);

			return MakeJSONObjectArray("roles", Roles, *this);
		}

		return ListHandler<ManagementRoleDB>("roles", DB_, *this);
	}
} // namespace OpenWifi