//
// Created by stephane bourque on 2022-04-06.
//

#include "RESTAPI_operators_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTAPI/RESTAPI_rbac_helpers.h"
#include "RESTAPI/RESTAPI_list_helpers.h"

namespace OpenWifi {
	void RESTAPI_operators_list_handler::DoGet() {
		if (!QB_.Select.empty()) {
			if (RBAC::IsRootUser(*this)) {
				return ReturnRecordList<decltype(DB_), ProvObjects::Operator>("operators", DB_,
																			  *this);
			}

			std::vector<ProvObjects::Operator> Filtered;
			for (const auto &id : SelectedRecords()) {
				ProvObjects::Operator Existing;
				if (DB_.GetRecord("id", id, Existing) &&
					RBAC::HasAccess(*this, "operator", "LIST",
									RBAC::TargetScope{Existing.entityId, ""})) {
					Filtered.push_back(Existing);
				}
			}
			if (QB_.CountOnly) {
				return ReturnCountOnly(Filtered.size());
			}
			return MakeJSONObjectArray("operators", Filtered, *this);
		}

		std::vector<ProvObjects::Operator> Entries;
		auto total = DB_.Count();
		if (total > 0) {
			DB_.GetRecords(0, total, Entries);
		}

		if (!RBAC::IsRootUser(*this)) {
			// RBAC filtering must happen before CountOnly and pagination, so fetch the full candidate set first.
			Entries = RESTAPI::FilterRecords(
				Entries,
				[&](const auto &Entry) {
					return RBAC::HasAccess(*this, "operator", "LIST",
										   RBAC::TargetScope{Entry.entityId, ""});
				});
		}

		if (QB_.CountOnly) {
			return ReturnCountOnly(Entries.size());
		}

		auto page = RESTAPI::ApplyPagination(Entries, QB_.Offset, QB_.Limit);
		return MakeJSONObjectArray("operators", page, *this);
	}
} // namespace OpenWifi
