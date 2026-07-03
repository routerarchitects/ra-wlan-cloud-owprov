//
// Created by stephane bourque on 2021-08-23.
//

#include "RESTAPI_location_list_handler.h"

#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"

namespace OpenWifi {

	void RESTAPI_location_list_handler::DoGet() {
		std::string VenueId;
		if (HasParameter("locationsForVenue", VenueId)) {
			ProvObjects::Venue RequestedVenue;
			if (!StorageService()->VenueDB().GetRecord("id", VenueId, RequestedVenue)) {
				return NotFound();
			}
			if (!RBAC::RequireAccess(
					*this,
					"venue",
					"READ",
					RBAC::TargetScope{RequestedVenue.entity, VenueId})) {
				return;
			}

			std::vector<triplet_t> IDs;
			GetLocationsForVenue(VenueId, IDs);
			Poco::JSON::Array Locations;
			for (const auto &[name, description, uuid] : IDs) {
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

		std::vector<LocationDB::RecordName> entries;
		auto allow = [&](const auto &record) {
			RBAC::TargetScope scope;
			if (!RBAC::ResolveLocationScope(record.info.id, scope)) {
				return RBAC::IsRootUser(*this);
			}
			return RBAC::HasAccess(*this, "location", "LIST", scope) ||
				   RBAC::HasAccess(*this, "location", "READ", scope);
		};

		if (!QB_.Select.empty()) {
			for (const auto &id : SelectedRecords()) {
				LocationDB::RecordName record;
				if (DB_.GetRecord("id", id, record) && allow(record)) {
					entries.push_back(record);
				}
			}
		} else {
			DB_.Iterate([&](const LocationDB::RecordName &record) {
				if (allow(record)) {
					entries.push_back(record);
				}
				return true;
			});
		}

		if (QB_.CountOnly) {
			return ReturnCountOnly(entries.size());
		}
		return MakeJSONObjectArray("locations",
								   RESTAPI::ApplyPagination(entries, QB_.Offset, QB_.Limit), *this);
	}
} // namespace OpenWifi
