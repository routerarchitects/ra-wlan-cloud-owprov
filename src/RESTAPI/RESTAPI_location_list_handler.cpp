//
// Created by stephane bourque on 2021-08-23.
//

#include "RESTAPI_location_list_handler.h"

#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"

namespace OpenWifi {

	void RESTAPI_location_list_handler::DoGet() {
		if (UserInfo_.userinfo.userRole == SecurityObjects::ROOT || IsInternalServiceOnly()) {
			return ListHandler<LocationDB>("locations", DB_, *this);
		}

		auto Access = ResolveScopedReadAuthorization(*this, "location");
		std::set<std::string> LocationIds;
		auto RequestedVenue = GetParameter("venue", "");
		for (const auto &VenueId : Access.venues) {
			if (!RequestedVenue.empty() && RequestedVenue != VenueId) {
				continue;
			}
			ProvObjects::Venue Venue;
			if (!StorageService()->VenueDB().GetRecord("id", VenueId, Venue)) {
				continue;
			}
			if (!Venue.location.empty()) {
				LocationIds.insert(Venue.location);
			}
			std::vector<triplet_t> Eligible;
			GetLocationsForVenue(VenueId, Eligible);
			for (const auto &[name, description, uuid] : Eligible) {
				(void)name;
				(void)description;
				LocationIds.insert(uuid);
			}
		}

		return ScopedEntityOwnedListHandler<LocationDB>("locations", DB_, *this, Access,
													  LocationIds);
	}
} // namespace OpenWifi
