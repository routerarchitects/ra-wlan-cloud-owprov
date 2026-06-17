//
// Created by stephane bourque on 2021-08-26.
//

#include "RESTAPI_managementRole_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTAPI/RESTAPI_rbac_helpers.h"
#include "StorageService.h"
#include <algorithm>

namespace OpenWifi {
	namespace {
		bool RoleMatchesUser(const ProvObjects::ManagementRole &role,
							 const std::string &userId) {
			if (userId.empty()) {
				return true;
			}
			return std::find(role.users.begin(), role.users.end(), userId) != role.users.end();
		}

		bool RoleMatchesEntity(const ProvObjects::ManagementRole &role,
							   const std::string &entityId) {
			return entityId.empty() || role.entity == entityId;
		}
	} // namespace

	void RESTAPI_managementRole_list_handler::DoGet() {
		auto userId = GetParameter("userId");
		auto entityId = GetParameter("entityId");

		if (RBAC::IsRootUser(*this)) {
			ProvObjects::ManagementRoleVec allRoles;
			DB_.GetRecords(QB_.Offset, QB_.Limit, allRoles);
			ProvObjects::ManagementRoleVec filtered;
			for (const auto &role : allRoles) {
				if (RoleMatchesUser(role, userId) && RoleMatchesEntity(role, entityId)) {
					filtered.push_back(role);
				}
			}
			return MakeJSONObjectArray("roles", filtered, *this);
		}

		ProvObjects::ManagementRoleVec allRoles;
		DB_.GetRecords(QB_.Offset, QB_.Limit, allRoles);
		ProvObjects::ManagementRoleVec filtered;
		for (const auto &role : allRoles) {
			if (RoleMatchesUser(role, userId) && RoleMatchesEntity(role, entityId) &&
				RBAC::IsScopeAllowed(*this, RBAC::TargetScope{role.entity, role.venue})) {
				filtered.push_back(role);
			}
		}
		return MakeJSONObjectArray("roles", filtered, *this);
	}
} // namespace OpenWifi
