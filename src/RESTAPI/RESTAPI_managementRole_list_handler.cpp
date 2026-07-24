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
			bool isRoot = (UserInfo_.userinfo.userRole == SecurityObjects::ROOT);

			std::set<std::string> AllowedEntities;
			std::set<std::string> AllowedVenues;

			if (!isRoot) {
				std::vector<ProvObjects::ManagementRole> RequesterRoles;
				if (FindAllUserRoles(UserInfo_.userinfo.id, RequesterRoles)) {
					for (const auto &role : RequesterRoles) {
						if (!role.entity.empty()) {
							AllowedEntities.insert(role.entity);
						}
						if (!role.venue.empty()) {
							AllowedVenues.insert(role.venue);
						}
					}
				}
				if (AllowedEntities.empty() && AllowedVenues.empty()) {
					ProvObjects::ManagementRoleVec EmptyRoles;
					return MakeJSONObjectArray("roles", EmptyRoles, *this);
				}
			}

			ProvObjects::ManagementRoleVec Roles;
			auto lambda = [&](const ProvObjects::ManagementRole &role) {
				if (std::find(role.users.begin(), role.users.end(), userParam) != role.users.end()) {
					if (isRoot || AllowedEntities.count(role.entity) || AllowedVenues.count(role.venue)) {
						Roles.push_back(role);
					}
				}
				return true;
			};
			DB_.Iterate(lambda);

			return MakeJSONObjectArray("roles", Roles, *this);
		}

		return ListHandler<ManagementRoleDB>("roles", DB_, *this);
	}
} // namespace OpenWifi