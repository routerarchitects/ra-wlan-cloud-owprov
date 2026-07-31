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
        if (!subscriberId.empty()) {
            if (!Utils::ValidUUID(subscriberId)) {
                return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
            }
            VenueDB::RecordVec Venues;
            auto Where = fmt::format(" subscriber='{}' ", ORM::Escape(subscriberId));
            DB_.GetRecords(QB_.Offset, QB_.Limit, Venues, Where, " ORDER BY name ");
            return ReturnObject("venues", Venues);
        }

        auto RRMvendor = GetParameter("RRMvendor","");
        if(RRMvendor.empty()) {
            return ListHandler<VenueDB>("venues", DB_, *this);
        }
        VenueDB::RecordVec Venues;
        auto Where = fmt::format(" deviceRules LIKE '%{}%' ", RRMvendor);
        DB_.GetRecords(QB_.Offset, QB_.Limit, Venues, Where, " ORDER BY name ");
        return ReturnObject("venues",Venues);
    }
} // namespace OpenWifi