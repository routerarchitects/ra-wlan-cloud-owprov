//
// Created by stephane bourque on 2021-08-23.
//

#include "RESTAPI_venue_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTAPI/RESTAPI_rbac_helpers.h"
#include "StorageService.h"
#include <algorithm>

namespace OpenWifi {
	void RESTAPI_venue_list_handler::DoGet() {
		auto RRMvendor = GetParameter("RRMvendor", "");
		if (RRMvendor.empty()) {
			return ListHandler<VenueDB>("venues", DB_, *this);
		}

		auto matchesVendor = [&](const ProvObjects::Venue &venue) {
			return venue.deviceRules.rrm.find(RRMvendor) != std::string::npos;
		};

		if (!QB_.Select.empty()) {
			VenueDB::RecordVec Venues;
			for (const auto &id : SelectedRecords()) {
				ProvObjects::Venue Venue;
				if (DB_.GetRecord("id", id, Venue) && matchesVendor(Venue)) {
					Venues.push_back(Venue);
				}
			}
			if (!RBAC::IsRootUser(*this)) {
				VenueDB::RecordVec Visible;
				for (const auto &venue : Venues) {
					if (RBAC::IsVenueVisible(*this, venue.info.id)) {
						Visible.push_back(venue);
					}
				}
				Venues.swap(Visible);
			}
			if (QB_.CountOnly) {
				return ReturnCountOnly(Venues.size());
			}
			return ReturnObject("venues", Venues);
		}

		auto Where = fmt::format(" deviceRules LIKE '%{}%' ", RRMvendor);
		VenueDB::RecordVec Venues;
		auto Count = DB_.Count(Where);
		if (Count > 0) {
			DB_.GetRecords(0, Count, Venues, Where, " ORDER BY name ");
		}

		if (!RBAC::IsRootUser(*this)) {
			VenueDB::RecordVec Visible;
			for (const auto &venue : Venues) {
				if (RBAC::IsVenueVisible(*this, venue.info.id)) {
					Visible.push_back(venue);
				}
			}
			Venues.swap(Visible);
		}

		if (QB_.CountOnly) {
			return ReturnCountOnly(Venues.size());
		}

		if (QB_.Offset < Venues.size()) {
			auto start = static_cast<std::size_t>(QB_.Offset);
			auto end = QB_.Limit == 0
						   ? start
						   : std::min<std::size_t>(Venues.size(),
												   start + static_cast<std::size_t>(QB_.Limit));
			Venues = VenueDB::RecordVec(Venues.begin() + start, Venues.begin() + end);
		} else {
			Venues.clear();
		}
		return ReturnObject("venues", Venues);
	}
} // namespace OpenWifi
