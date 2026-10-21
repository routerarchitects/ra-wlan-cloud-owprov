//
// Created by stephane bourque on 2022-02-23.
//

#include "RESTAPI_variables_list_handler.h"

#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"
#include "framework/utils.h"

namespace OpenWifi {

	void RESTAPI_variables_list_handler::DoGet() {
		bool isRoot = (UserInfo_.userinfo.userRole == SecurityObjects::ROOT);

		// 1. Root users and internal microservice requests (Internal_ == true) bypass scoping
		if (isRoot || Internal_) {
			return ListHandler<VariablesDB>("variableBlocks", DB_, *this);
		}

		// 2. Non-root users: resolve all roles and policies
		std::vector<ProvObjects::ManagementRole> Roles;
		std::set<std::string> AllowedEntities;
		std::set<std::string> AllowedVenues;
		std::set<std::string> DeniedVenues;

		auto RoleAllowsVariablesRead = [&](const ProvObjects::ManagementRole &role) -> bool {
			ProvObjects::ManagementPolicy Policy;
			if (!AuthCache::GetInstance()->GetPolicy(role.managementPolicy, Policy)) {
				if (!StorageService()->PolicyDB().GetRecord("id", role.managementPolicy, Policy)) {
					return false;
				}
				AuthCache::GetInstance()->SetPolicy(role.managementPolicy, Policy);
			}
			return PolicyAllows(Policy, "variables", Poco::Net::HTTPRequest::HTTP_GET);
		};

		if (FindAllUserRoles(UserInfo_.userinfo.id, Roles)) {
			// 1. Process venue-specific roles:
			// If role specifies a venue (even if entity is also populated),
			// only collect the venue (and its descendant venues) — NEVER the entity.
			for (const auto &role : Roles) {
				if (!role.venue.empty()) {
					if (RoleAllowsVariablesRead(role)) {
						AllowedVenues.insert(role.venue);
						GetDescendantVenues(role.venue, AllowedVenues);
					} else {
						DeniedVenues.insert(role.venue);
					}
				}
			}

			// 2. Process entity-only roles:
			// If venue is missing, collect the entity AND all of its venues having entityId as entity.
			for (const auto &role : Roles) {
				if (role.venue.empty() && !role.entity.empty()) {
					if (RoleAllowsVariablesRead(role)) {
						AllowedEntities.insert(role.entity);

						// Collect venues from Entity record
						ProvObjects::Entity EntRec;
						if (StorageService()->EntityDB().GetRecord("id", role.entity, EntRec)) {
							for (const auto &vId : EntRec.venues) {
								AllowedVenues.insert(vId);
								GetDescendantVenues(vId, AllowedVenues);
							}
						}

						// Also collect any venue from VenueDB where entity == role.entity
						VenueDB::RecordVec EntityVenues;
						if (StorageService()->VenueDB().GetRecords(0, 50000, EntityVenues, " entity='" + ORM::Escape(role.entity) + "'")) {
							for (const auto &v : EntityVenues) {
								AllowedVenues.insert(v.info.id);
								GetDescendantVenues(v.info.id, AllowedVenues);
							}
						}
					}
				}
			}

			// 3. Shadowing: venue-level deny takes precedence over entity-level allow
			for (const auto &vId : DeniedVenues) {
				AllowedVenues.erase(vId);
			}
		}

		// 3. If the user has no roles granting variables:GET on any scope, return empty
		if (AllowedEntities.empty() && AllowedVenues.empty()) {
			VariablesDB::RecordVec Entries;
			if (QB_.CountOnly)
				return ReturnCountOnly(0);
			return MakeJSONObjectArray("variableBlocks", Entries, *this);
		}

		// Helper to build SQL IN clause
		auto makeInClause = [](const std::string &field, const std::set<std::string> &ids) -> std::string {
			if (ids.empty())
				return "";
			std::string res = field + " IN (";
			bool first = true;
			for (const auto &id : ids) {
				if (!first)
					res += ",";
				res += "'" + ORM::Escape(id) + "'";
				first = false;
			}
			res += ")";
			return res;
		};

		std::string rawEntClause = makeInClause("entity", AllowedEntities);
		std::string entClause;
		if (!rawEntClause.empty()) {
			entClause = "((venue IS NULL OR venue='') AND " + rawEntClause + ")";
		}
		std::string venClause = makeInClause("venue", AllowedVenues);
		std::string ScopeWhere;
		if (!entClause.empty() && !venClause.empty()) {
			ScopeWhere = "(" + entClause + " OR " + venClause + ")";
		} else if (!entClause.empty()) {
			ScopeWhere = entClause;
		} else if (!venClause.empty()) {
			ScopeWhere = venClause;
		}

		if (!DeniedVenues.empty()) {
			std::string deniedVenClause = makeInClause("venue", DeniedVenues);
			if (!deniedVenClause.empty()) {
				ScopeWhere = "(" + ScopeWhere + ") AND (venue IS NULL OR venue='' OR NOT " + deniedVenClause + ")";
			}
		}

		// 4. Handle query parameters if provided (?entity=... or ?venue=...)
		auto EntityParam = GetParameter("entity", "");
		auto VenueParam = GetParameter("venue", "");
		std::string ParamWhere;

		if (!EntityParam.empty()) {
			// If requested entity is outside permitted scopes, return empty
			if (!AllowedEntities.count(EntityParam)) {
				VariablesDB::RecordVec Entries;
				if (QB_.CountOnly)
					return ReturnCountOnly(0);
				return MakeJSONObjectArray("variableBlocks", Entries, *this);
			}
			ParamWhere = " entity='" + ORM::Escape(EntityParam) + "'";
		} else if (!VenueParam.empty()) {
			// If requested venue is outside permitted scopes, return empty
			if (!AllowedVenues.count(VenueParam)) {
				VariablesDB::RecordVec Entries;
				if (QB_.CountOnly)
					return ReturnCountOnly(0);
				return MakeJSONObjectArray("variableBlocks", Entries, *this);
			}
			ParamWhere = " venue='" + ORM::Escape(VenueParam) + "'";
		}

		// 5. Combine parameter filter with overall permitted scope
		std::string FinalWhere;
		if (!ParamWhere.empty() && !ScopeWhere.empty()) {
			FinalWhere = "(" + ParamWhere + ") AND " + ScopeWhere;
		} else if (!ParamWhere.empty()) {
			FinalWhere = ParamWhere;
		} else {
			FinalWhere = ScopeWhere;
		}

		// 6. Handle select queries / countOnly / paginated list
		if (!QB_.Select.empty()) {
			VariablesDB::RecordVec SelectedEntries;
			for (const auto &id : QB_.Select) {
				ProvObjects::VariableBlock Var;
				if (DB_.GetRecord("id", id, Var)) {
					// 1. If explicit ?entity= was specified, record must match it
					if (!EntityParam.empty() && Var.entity != EntityParam) {
						continue;
					}
					// 2. If explicit ?venue= was specified, record must match it
					if (!VenueParam.empty() && Var.venue != VenueParam) {
						continue;
					}
					// 3. Explicit venue denials take precedence and shadow entity permissions
					if (!Var.venue.empty() && DeniedVenues.count(Var.venue)) {
						continue;
					}
					// 4. Must belong to permitted entity or venue scopes:
					// Venue-scoped variables require venue in AllowedVenues.
					// Entity-scoped variables (empty venue) require entity in AllowedEntities.
					bool venueAllowed = !Var.venue.empty() && AllowedVenues.count(Var.venue);
					bool entityAllowed = Var.venue.empty() && !Var.entity.empty() && AllowedEntities.count(Var.entity);
					if (entityAllowed || venueAllowed) {
						SelectedEntries.push_back(Var);
					}
				}
			}
			if (QB_.CountOnly) {
				return ReturnCountOnly(SelectedEntries.size());
			}
			return MakeJSONObjectArray("variableBlocks", SelectedEntries, *this);
		}

		if (QB_.CountOnly) {
			auto C = DB_.Count(FinalWhere);
			return ReturnCountOnly(C);
		}

		VariablesDB::RecordVec Entries;
		DB_.GetRecords(QB_.Offset, QB_.Limit, Entries, FinalWhere);
		return MakeJSONObjectArray("variableBlocks", Entries, *this);
	}

} // namespace OpenWifi
