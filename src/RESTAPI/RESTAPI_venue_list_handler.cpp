//
// Created by stephane bourque on 2021-08-23.
//

#include "RESTAPI_venue_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "StorageService.h"
#include "framework/utils.h"

namespace OpenWifi {
	void RESTAPI_venue_list_handler::DoGet() {
		auto subscriberId = GetParameter("subscriberId", "");

		// Pure internal microservice calls (e.g. UserPortal owsub location/topology synthesis)
		if (IsInternalServiceOnly() && !subscriberId.empty()) {
			if (!Utils::ValidUUID(subscriberId)) {
				return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
			}
			std::string WhereClause = fmt::format(" subscriber='{}' ", ORM::Escape(subscriberId));
			auto RRMvendor = GetParameter("RRMvendor", "");
			if (!RRMvendor.empty()) {
				WhereClause += fmt::format(" AND deviceRules LIKE '%{}%' ", ORM::Escape(RRMvendor));
			}
			if (QB_.CountOnly) {
				auto C = DB_.Count(WhereClause);
				return ReturnCountOnly(C);
			}
			VenueDB::RecordVec Venues;
			DB_.GetRecords(QB_.Offset, QB_.Limit, Venues, WhereClause, " ORDER BY name ");
			return ReturnObject("venues", Venues);
		}

		bool isRoot = (UserInfo_.userinfo.userRole == SecurityObjects::ROOT);
		auto LocationsForVenue = GetParameter("locationsForVenue", "");
		if (!LocationsForVenue.empty()) {
			ProvObjects::Venue Venue;
			if (!StorageService()->VenueDB().GetRecord("id", LocationsForVenue, Venue)) {
				return UnAuthorized(RESTAPI::Errors::ACCESS_DENIED);
			}

			const auto EntityId = Venue.entity.empty() ? FindParentEntity(Venue) : Venue.entity;
			if (!isRoot && !IsInternalServiceOnly()) {
				std::string Reason;
				if (!IsScopeAuthorized(EntityId, LocationsForVenue, "location",
									   Poco::Net::HTTPRequest::HTTP_GET, &Reason)) {
					return UnAuthorized(RESTAPI::Errors::ACCESS_DENIED);
				}
			}

			std::vector<triplet_t> Eligible;
			GetLocationsForVenue(LocationsForVenue, Eligible);
			if (!Venue.location.empty()) {
				ProvObjects::Location Assigned;
				if (StorageService()->LocationDB().GetRecord("id", Venue.location, Assigned)) {
					Eligible.emplace_back(Assigned.info.name, Assigned.info.description,
										  Assigned.info.id);
				}
			}

			std::set<std::string> Seen;
			Poco::JSON::Array Locations;
			for (const auto &[name, description, uuid] : Eligible) {
				if (!Seen.insert(uuid).second) {
					continue;
				}
				Poco::JSON::Object Location;
				Location.set("name", name);
				Location.set("description", description);
				Location.set("uuid", uuid);
				Locations.add(Location);
			}
			Poco::JSON::Object Answer;
			Answer.set("locations", Locations);
			return ReturnObject(Answer);
		}

		if (isRoot) {
			auto RRMvendor = GetParameter("RRMvendor","");
			std::string Where;
			if (!RRMvendor.empty()) {
				Where = fmt::format(" deviceRules LIKE '%{}%' ", ORM::Escape(RRMvendor));
			}
			if (!subscriberId.empty()) {
				if (!Utils::ValidUUID(subscriberId)) {
					return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
				}
				std::string SubWhere = fmt::format(" subscriber='{}' ", ORM::Escape(subscriberId));
				Where = Where.empty() ? SubWhere : "(" + Where + ") AND (" + SubWhere + ")";
			}
			if (Where.empty()) {
				return ListHandler<VenueDB>("venues", DB_, *this);
			}
			if (QB_.CountOnly) {
				auto C = DB_.Count(Where);
				return ReturnCountOnly(C);
			}
			VenueDB::RecordVec Venues;
			DB_.GetRecords(QB_.Offset, QB_.Limit, Venues, Where, " ORDER BY name ");
			return ReturnObject("venues", Venues);
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
		if (!subscriberId.empty()) {
			if (!Utils::ValidUUID(subscriberId)) {
				return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
			}
			std::string SubWhere = fmt::format(" subscriber='{}' ", ORM::Escape(subscriberId));
			if (!FinalWhere.empty()) {
				FinalWhere = "(" + FinalWhere + ") AND (" + SubWhere + ")";
			} else {
				FinalWhere = SubWhere;
			}
		}

		if (QB_.CountOnly) {
			auto C = DB_.Count(FinalWhere);
			return ReturnCountOnly(C);
		}

		VenueDB::RecordVec FilteredVenues;
		DB_.GetRecords(QB_.Offset, QB_.Limit, FilteredVenues, FinalWhere, " ORDER BY name ");
		return ReturnObject("venues", FilteredVenues);
	}
} // namespace OpenWifi
