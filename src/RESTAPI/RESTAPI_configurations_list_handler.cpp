//
// Created by stephane bourque on 2021-08-29.
//

#include "RESTAPI_configurations_list_handler.h"

#include "RESTAPI_db_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"

namespace OpenWifi {
	void RESTAPI_configurations_list_handler::DoGet() {
		std::vector<ConfigurationDB::RecordName> entries;
		auto allow = [&](const auto &record) {
			RBAC::TargetScope scope;
			if (!RBAC::ResolveConfigurationScope(record.info.id, scope)) {
				return RBAC::IsRootUser(*this);
			}
			return RBAC::HasAccess(*this, "configuration", "LIST", scope) ||
				   RBAC::HasAccess(*this, "configuration", "READ", scope);
		};

		if (!QB_.Select.empty()) {
			for (const auto &id : SelectedRecords()) {
				ConfigurationDB::RecordName record;
				if (DB_.GetRecord("id", id, record) && allow(record)) {
					entries.push_back(record);
				}
			}
		} else {
			DB_.Iterate([&](const ConfigurationDB::RecordName &record) {
				if (allow(record)) {
					entries.push_back(record);
				}
				return true;
			});
		}

		if (QB_.CountOnly) {
			return ReturnCountOnly(entries.size());
		}
		return MakeJSONObjectArray("configurations",
								   RESTAPI::ApplyPagination(entries, QB_.Offset, QB_.Limit), *this);
	}
} // namespace OpenWifi
