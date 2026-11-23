//
// Created by stephane bourque on 2021-08-23.
//

#include "RESTAPI_contact_list_handler.h"

#include "RESTAPI_db_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"

namespace OpenWifi {
	void RESTAPI_contact_list_handler::DoGet() {
		if (UserInfo_.userinfo.userRole == SecurityObjects::ROOT || IsInternalServiceOnly()) {
			return ListHandler<ContactDB>("contacts", DB_, *this);
		}

		auto Access = ResolveScopedReadAuthorization(*this, "contact");
		std::set<std::string> ContactIds;
		auto RequestedVenue = GetParameter("venue", "");
		for (const auto &VenueId : Access.venues) {
			if (!RequestedVenue.empty() && RequestedVenue != VenueId) {
				continue;
			}
			ProvObjects::Venue Venue;
			if (StorageService()->VenueDB().GetRecord("id", VenueId, Venue)) {
				ContactIds.insert(Venue.contacts.begin(), Venue.contacts.end());
			}
		}

		return ScopedEntityOwnedListHandler<ContactDB>("contacts", DB_, *this, Access,
													 ContactIds);
	}
} // namespace OpenWifi
