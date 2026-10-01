#include "RESTAPI_managementRole_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "StorageService.h"
#include "framework/utils.h"
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

		auto policyParam = GetParameter("policyId", "");
		if (!policyParam.empty() && !Utils::ValidUUID(policyParam)) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
		}

		auto entityParam = GetParameter("entity", "");
		if (!entityParam.empty() && !Utils::ValidUUID(entityParam)) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
		}

		auto venueParam = GetParameter("venue", "");
		if (!venueParam.empty() && !Utils::ValidUUID(venueParam)) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
		}

		bool isRoot = (UserInfo_.userinfo.userRole == SecurityObjects::ROOT);
		std::set<std::string> AllowedEntities;
		std::set<std::string> AllowedVenues;

		if (!isRoot) {
			std::vector<ProvObjects::ManagementRole> RequesterRoles;
			if (FindAllUserRoles(UserInfo_.userinfo.id, RequesterRoles)) {
				for (const auto &role : RequesterRoles) {
					if (!role.venue.empty()) {
						AllowedVenues.insert(role.venue);
					} else if (!role.entity.empty()) {
						AllowedEntities.insert(role.entity);
					}
				}
			}
			if (AllowedEntities.empty() && AllowedVenues.empty()) {
				if (QB_.CountOnly) {
					return ReturnCountOnly(0);
				}
				ProvObjects::ManagementRoleVec EmptyRoles;
				return MakeJSONObjectArray("roles", EmptyRoles, *this);
			}

			if (!entityParam.empty() && !AllowedEntities.count(entityParam)) {
				if (QB_.CountOnly) {
					return ReturnCountOnly(0);
				}
				ProvObjects::ManagementRoleVec EmptyRoles;
				return MakeJSONObjectArray("roles", EmptyRoles, *this);
			}

			if (!venueParam.empty() && !AllowedVenues.count(venueParam)) {
				if (QB_.CountOnly) {
					return ReturnCountOnly(0);
				}
				ProvObjects::ManagementRoleVec EmptyRoles;
				return MakeJSONObjectArray("roles", EmptyRoles, *this);
			}
		}

		if (!userParam.empty()) {
			ProvObjects::ManagementRoleVec Roles;
			auto lambda = [&](const ProvObjects::ManagementRole &role) {
				if (!policyParam.empty() && role.managementPolicy != policyParam) {
					return true;
				}
				if (!entityParam.empty() && role.entity != entityParam) {
					return true;
				}
				if (!venueParam.empty() && role.venue != venueParam) {
					return true;
				}
				if (std::find(role.users.begin(), role.users.end(), userParam) == role.users.end()) {
					return true;
				}
				if (isRoot || AllowedEntities.count(role.entity) || AllowedVenues.count(role.venue)) {
					Roles.push_back(role);
				}
				return true;
			};
			DB_.Iterate(lambda);

			if (QB_.CountOnly) {
				return ReturnCountOnly(Roles.size());
			}

			return MakeJSONObjectArray("roles", Roles, *this);
		}

		if (!policyParam.empty()) {
			std::string Where = " managementPolicy='" + policyParam + "'";
			if (!entityParam.empty()) {
				Where += " AND entity='" + entityParam + "'";
			}
			if (!venueParam.empty()) {
				Where += " AND venue='" + venueParam + "'";
			}

			if (!isRoot && entityParam.empty() && venueParam.empty()) {
				auto makeInClause = [](const std::string &field, const std::set<std::string> &ids) -> std::string {
					if (ids.empty()) return "";
					std::string res = field + " IN (";
					bool first = true;
					for (const auto &id : ids) {
						if (!first) res += ",";
						res += "'" + ORM::Escape(id) + "'";
						first = false;
					}
					res += ")";
					return res;
				};

				std::string entityClause = makeInClause("entity", AllowedEntities);
				std::string venueClause = makeInClause("venue", AllowedVenues);
				if (!entityClause.empty() && !venueClause.empty()) {
					Where += " AND (" + entityClause + " OR " + venueClause + ")";
				} else if (!entityClause.empty()) {
					Where += " AND " + entityClause;
				} else if (!venueClause.empty()) {
					Where += " AND " + venueClause;
				}
			}

			if (QB_.CountOnly) {
				auto C = DB_.Count(Where);
				return ReturnCountOnly(C);
			}

			ProvObjects::ManagementRoleVec Roles;
			DB_.GetRecords(QB_.Offset, QB_.Limit, Roles, Where);
			return MakeJSONObjectArray("roles", Roles, *this);
		}

		return ListHandler<ManagementRoleDB>("roles", DB_, *this);
	}
} // namespace OpenWifi