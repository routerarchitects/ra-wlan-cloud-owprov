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
		std::set<std::string> DeniedVenues;

		if (!isRoot) {
			auto policyAllowsGet = [&](const ProvObjects::ManagementRole &role) -> bool {
				ProvObjects::ManagementPolicy Policy;
				if (!AuthCache::GetInstance()->GetPolicy(role.managementPolicy, Policy)) {
					if (!StorageService()->PolicyDB().GetRecord("id", role.managementPolicy, Policy)) {
						return false;
					}
					AuthCache::GetInstance()->SetPolicy(role.managementPolicy, Policy);
				}
				return PolicyAllows(Policy, "managementRole", Poco::Net::HTTPRequest::HTTP_GET);
			};

			std::vector<ProvObjects::ManagementRole> RequesterRoles;
			if (FindAllUserRoles(UserInfo_.userinfo.id, RequesterRoles)) {
				for (const auto &role : RequesterRoles) {
					if (!role.venue.empty()) {
						if (policyAllowsGet(role)) {
							AllowedVenues.insert(role.venue);
						} else {
							DeniedVenues.insert(role.venue);
						}
					} else if (!role.entity.empty()) {
						if (policyAllowsGet(role)) {
							AllowedEntities.insert(role.entity);
						}
					}
				}
				for (const auto &vId : DeniedVenues) {
					AllowedVenues.erase(vId);
				}
			}

			if (!venueParam.empty() && DeniedVenues.count(venueParam)) {
				if (QB_.CountOnly) {
					return ReturnCountOnly(0);
				}
				ProvObjects::ManagementRoleVec EmptyRoles;
				return MakeJSONObjectArray("roles", EmptyRoles, *this);
			}

			if (AllowedEntities.empty() && AllowedVenues.empty()) {
				if (QB_.CountOnly) {
					return ReturnCountOnly(0);
				}
				ProvObjects::ManagementRoleVec EmptyRoles;
				return MakeJSONObjectArray("roles", EmptyRoles, *this);
			}
		}

		if (!QB_.Select.empty()) {
			ProvObjects::ManagementRoleVec Roles;
			for (const auto &id : SelectedRecords()) {
				ProvObjects::ManagementRole role;
				if (!DB_.GetRecord("id", id, role)) {
					return BadRequest(RESTAPI::Errors::UnknownId);
				}

				if (!policyParam.empty() && role.managementPolicy != policyParam) {
					continue;
				}
				if (!entityParam.empty() && role.entity != entityParam) {
					continue;
				}
				if (!venueParam.empty() && role.venue != venueParam) {
					continue;
				}
				if (!userParam.empty() &&
					std::find(role.users.begin(), role.users.end(), userParam) == role.users.end()) {
					continue;
				}

				if (!isRoot) {
					if (!role.venue.empty() && DeniedVenues.count(role.venue)) {
						continue;
					}
					bool inScope = false;
					if (!role.venue.empty() && AllowedVenues.count(role.venue)) {
						inScope = true;
					} else if (!role.entity.empty() && AllowedEntities.count(role.entity)) {
						inScope = true;
					}
					if (!inScope) {
						continue;
					}
				}

				Roles.push_back(role);
			}

			if (QB_.CountOnly) {
				return ReturnCountOnly(Roles.size());
			}

			return MakeJSONObjectArray("roles", Roles, *this);
		}

		std::string Where;
		if (!policyParam.empty()) {
			Where = "managementPolicy='" + policyParam + "'";
		}
		if (!entityParam.empty()) {
			if (!Where.empty()) {
				Where += " AND ";
			}
			Where += "entity='" + entityParam + "'";
		}
		if (!venueParam.empty()) {
			if (!Where.empty()) {
				Where += " AND ";
			}
			Where += "venue='" + venueParam + "'";
		}

		if (!isRoot) {
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
			std::string rbacClause;
			if (!entityClause.empty() && !venueClause.empty()) {
				rbacClause = "(" + entityClause + " OR " + venueClause + ")";
			} else if (!entityClause.empty()) {
				rbacClause = entityClause;
			} else if (!venueClause.empty()) {
				rbacClause = venueClause;
			}
			if (!rbacClause.empty()) {
				if (!Where.empty()) {
					Where += " AND ";
				}
				Where += rbacClause;
			}

			if (!DeniedVenues.empty()) {
				std::string deniedClause = "venue NOT IN (";
				bool first = true;
				for (const auto &id : DeniedVenues) {
					if (!first) deniedClause += ",";
					deniedClause += "'" + ORM::Escape(id) + "'";
					first = false;
				}
				deniedClause += ")";
				if (!Where.empty()) {
					Where += " AND ";
				}
				Where += deniedClause;
			}
		}

		if (!userParam.empty()) {
			ProvObjects::ManagementRoleVec Roles;
			auto lambda = [&](const ProvObjects::ManagementRole &role) {
				if (std::find(role.users.begin(), role.users.end(), userParam) != role.users.end()) {
					Roles.push_back(role);
				}
				return true;
			};
			DB_.Iterate(lambda, Where);

			if (QB_.CountOnly) {
				return ReturnCountOnly(Roles.size());
			}

			return MakeJSONObjectArray("roles", Roles, *this);
		}

		if (!policyParam.empty() || !venueParam.empty() || !entityParam.empty() || !isRoot) {
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