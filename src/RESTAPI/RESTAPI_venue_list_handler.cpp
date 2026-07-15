//
// Created by stephane bourque on 2021-08-23.
//

#include "RESTAPI_venue_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "StorageService.h"

namespace OpenWifi {
	void RESTAPI_venue_list_handler::DoGet() {
		bool isRootOrSystem = (UserInfo_.userinfo.userRole == SecurityObjects::ROOT || UserInfo_.userinfo.userRole == SecurityObjects::SYSTEM);

		if (isRootOrSystem) {
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
		std::set<std::string> AssignedEntities;
		if (FindAllUserRoles(UserInfo_.userinfo.id, Roles)) {
			for (const auto &role : Roles) {
				if (!role.entity.empty()) {
					AssignedEntities.insert(role.entity);
				}
			}
		}

		if (AssignedEntities.empty()) {
			VenueDB::RecordVec Venues;
			return ReturnObject("venues", Venues);
		}

		// 1. Get all descendant entities
		std::set<std::string> descendantEntities;
		for (const auto &entId : AssignedEntities) {
			GetDescendantEntities(entId, descendantEntities);
		}

		// 2. Get all venues associated with these entities
		std::set<std::string> allowedVenues;
		for (const auto &entId : descendantEntities) {
			ProvObjects::Entity E;
			if (StorageService()->EntityDB().GetRecord("id", entId, E)) {
				for (const auto &vId : E.venues) {
					GetDescendantVenues(vId, allowedVenues);
				}
			}
		}

		// 3. Retrieve all venues and filter by allowedVenues
		VenueDB::RecordVec AllVenues;
		auto RRMvendor = GetParameter("RRMvendor","");
		if (!RRMvendor.empty()) {
			auto Where = fmt::format(" deviceRules LIKE '%{}%' ", RRMvendor);
			DB_.GetRecords(0, 10000, AllVenues, Where, " ORDER BY name ");
		} else {
			DB_.GetRecords(0, 10000, AllVenues);
		}

		VenueDB::RecordVec FilteredVenues;
		for (const auto &v : AllVenues) {
			if (allowedVenues.count(v.info.id)) {
				FilteredVenues.push_back(v);
			}
		}

		// Apply pagination (Offset, Limit) manually
		VenueDB::RecordVec PaginatedVenues;
		uint64_t offset = QB_.Offset;
		uint64_t limit = QB_.Limit;
		if (limit == 0) limit = 100;
		uint64_t count = 0;
		for (size_t i = offset; i < FilteredVenues.size() && count < limit; ++i) {
			PaginatedVenues.push_back(FilteredVenues[i]);
			count++;
		}
		return ReturnObject("venues", PaginatedVenues);
	}
} // namespace OpenWifi