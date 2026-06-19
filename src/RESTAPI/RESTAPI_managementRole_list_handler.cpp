//
// Created by stephane bourque on 2021-08-26.
//

#include "RESTAPI_managementRole_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTAPI/RESTAPI_rbac_helpers.h"
#include "RESTAPI/RESTAPI_list_helpers.h"
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

		ProvObjects::ManagementRoleVec roles;
		auto total = DB_.Count();
		if (total > 0) {
			DB_.GetRecords(0, total, roles);
		}

		roles = RESTAPI::FilterRecords(
			roles,
			[&](const auto &role) {
				return RoleMatchesUser(role, userId) &&
					   RoleMatchesEntity(role, entityId);
			});

		if (!RBAC::IsRootUser(*this)) {
			// RBAC filtering must happen before CountOnly and pagination, so fetch the full candidate set first.
			roles = RESTAPI::FilterRecords(
				roles,
				[&](const auto &role) {
					const auto scope = RBAC::TargetScope{role.entity, role.venue};
					return RBAC::HasAccess(*this, "managementRole", "LIST", scope) ||
						   RBAC::HasAccess(*this, "managementRole", "READ", scope);
				});
		}

		if (QB_.CountOnly) {
			return ReturnCountOnly(roles.size());
		}

		auto page = RESTAPI::ApplyPagination(roles, QB_.Offset, QB_.Limit);
		return MakeJSONObjectArray("roles", page, *this);
	}
} // namespace OpenWifi
