//
// Created by stephane bourque on 2022-04-06.
//

#include "RESTAPI_operators_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTAPI/RESTAPI_rbac_helpers.h"

namespace OpenWifi {
	void RESTAPI_operators_list_handler::DoGet() {
		if (QB_.CountOnly) {
			std::vector<ProvObjects::Operator> Entries;
			DB_.GetRecords(QB_.Offset, QB_.Limit, Entries);
			std::size_t Count = 0;
			for (const auto &Entry : Entries) {
				if (RBAC::HasAccess(*this, "operator", "LIST",
									RBAC::TargetScope{Entry.entityId, ""})) {
					++Count;
				}
			}
			return ReturnCountOnly(Count);
		}

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
			return MakeJSONObjectArray("operators", Filtered, *this);
		}

		std::vector<ProvObjects::Operator> Entries;
		DB_.GetRecords(QB_.Offset, QB_.Limit, Entries);
		if (RBAC::IsRootUser(*this)) {
			return MakeJSONObjectArray("operators", Entries, *this);
		}

		std::vector<ProvObjects::Operator> Filtered;
		for (const auto &Entry : Entries) {
			if (RBAC::HasAccess(*this, "operator", "LIST",
								RBAC::TargetScope{Entry.entityId, ""})) {
				Filtered.push_back(Entry);
			}
		}
		return MakeJSONObjectArray("operators", Filtered, *this);
	}
} // namespace OpenWifi
