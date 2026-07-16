//
// Created by stephane bourque on 2022-02-23.
//

#include "RESTAPI_variables_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"

namespace OpenWifi {

	void RESTAPI_variables_list_handler::DoGet() {
		std::vector<VariablesDB::RecordName> entries;
		auto allow = [&](const auto &record) {
			RBAC::TargetScope scope;
			if (!RBAC::ResolveVariableScope(record.info.id, scope)) {
				return RBAC::IsRootUser(*this);
			}
			return RBAC::HasAccess(*this, "variable", "LIST", scope) ||
				   RBAC::HasAccess(*this, "variable", "READ", scope);
		};

		if (!QB_.Select.empty()) {
			for (const auto &id : SelectedRecords()) {
				VariablesDB::RecordName record;
				if (DB_.GetRecord("id", id, record) && allow(record)) {
					entries.push_back(record);
				}
			}
		} else {
			DB_.Iterate([&](const VariablesDB::RecordName &record) {
				if (allow(record)) {
					entries.push_back(record);
				}
				return true;
			});
		}

		if (QB_.CountOnly) {
			return ReturnCountOnly(entries.size());
		}
		return MakeJSONObjectArray("variableBlocks",
								   RESTAPI::ApplyPagination(entries, QB_.Offset, QB_.Limit), *this);
	}

} // namespace OpenWifi
