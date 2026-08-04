//
// Created by stephane bourque on 2021-08-23.
//

#include "RESTAPI_venue_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "StorageService.h"

namespace OpenWifi {
	void RESTAPI_venue_list_handler::DoGet() {
		bool isRoot = (UserInfo_.userinfo.userRole == SecurityObjects::ROOT);

		if (isRoot) {
			auto RRMvendor = GetParameter("RRMvendor","");
			if(RRMvendor.empty()) {
				return ListHandler<VenueDB>("venues", DB_, *this);
			}
			VenueDB::RecordVec Venues;
			auto Where = fmt::format(" deviceRules LIKE '%{}%' ", RRMvendor);
			DB_.GetRecords(QB_.Offset, QB_.Limit, Venues, Where, " ORDER BY name ");
			return ReturnObject("venues",Venues);
		}

		// Standard user flow:
		std::vector<ProvObjects::ManagementRole> Roles;
		std::set<std::string> VisibleVenues;
		auto policyAllowsGet = [&](const ProvObjects::ManagementRole &role) -> bool {
			ProvObjects::ManagementPolicy Policy;
			if (!AuthCache::GetInstance()->GetPolicy(role.managementPolicy, Policy)) {
				if (!StorageService()->PolicyDB().GetRecord("id", role.managementPolicy, Policy)) {
					return false;
				}
				AuthCache::GetInstance()->SetPolicy(role.managementPolicy, Policy);
			}
			return PolicyAllows(Policy, "venue", Poco::Net::HTTPRequest::HTTP_GET);
		};
		std::set<std::string> DeniedVenues;
		if (FindAllUserRoles(UserInfo_.userinfo.id, Roles)) {
			for (const auto &role : Roles) {
				if (!role.venue.empty()) {
					if (policyAllowsGet(role)) {
						VisibleVenues.insert(role.venue);
					} else {
						DeniedVenues.insert(role.venue);
					}
				} else if (!role.entity.empty()) {
					if (policyAllowsGet(role)) {
						ProvObjects::Entity EntRec;
						if (StorageService()->EntityDB().GetRecord("id", role.entity, EntRec)) {
							for (const auto &vId : EntRec.venues) {
								GetDescendantVenues(vId, VisibleVenues);
							}
						}
					}
				}
			}
			for (const auto &vId : DeniedVenues) {
				VisibleVenues.erase(vId);
			}
		}

		if (VisibleVenues.empty()) {
			VenueDB::RecordVec Venues;
			return ReturnObject("venues", Venues);
		}

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

		std::string ScopeWhere = makeInClause("id", VisibleVenues);

		if (QB_.CountOnly) {
			auto C = DB_.Count(ScopeWhere);
			return ReturnCountOnly(C);
		}

		auto RRMvendor = GetParameter("RRMvendor", "");
		std::string FinalWhere = ScopeWhere;
		if (!RRMvendor.empty()) {
			std::string RRMWhere = fmt::format(" deviceRules LIKE '%{}%' ", ORM::Escape(RRMvendor));
			if (!FinalWhere.empty()) {
				FinalWhere = "(" + FinalWhere + ") AND (" + RRMWhere + ")";
			} else {
				FinalWhere = RRMWhere;
			}
		}

		VenueDB::RecordVec FilteredVenues;
		DB_.GetRecords(QB_.Offset, QB_.Limit, FilteredVenues, FinalWhere, " ORDER BY name ");
		return ReturnObject("venues", FilteredVenues);
	}
} // namespace OpenWifi
