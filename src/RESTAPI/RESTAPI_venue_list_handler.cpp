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
        auto RRMvendor = GetParameter("RRMvendor", "");

        if (!subscriberId.empty() && !Utils::ValidUUID(subscriberId)) {
            return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
        }

        if (subscriberId.empty() && RRMvendor.empty()) {
            return ListHandler<VenueDB>("venues", DB_, *this);
        }

        std::string WhereClause;
        if (!subscriberId.empty()) {
            WhereClause += fmt::format(" subscriber='{}' ", ORM::Escape(subscriberId));
        }
        if (!RRMvendor.empty()) {
            if (!WhereClause.empty()) {
                WhereClause += " AND ";
            }
            WhereClause += fmt::format(" deviceRules LIKE '%{}%' ", ORM::Escape(RRMvendor));
        }

        VenueDB::RecordVec Venues;
        DB_.GetRecords(QB_.Offset, QB_.Limit, Venues, WhereClause, " ORDER BY name ");
        return ReturnObject("venues", Venues);
    }
} // namespace OpenWifi