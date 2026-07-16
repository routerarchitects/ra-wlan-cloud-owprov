//
// Created by stephane bourque on 2021-08-23.
//

#include "RESTAPI_contact_list_handler.h"

#include "RESTAPI_db_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"

namespace OpenWifi {
	void RESTAPI_contact_list_handler::DoGet() {
		std::vector<ContactDB::RecordName> entries;
		auto allow = [&](const auto &record) {
			RBAC::TargetScope scope;
			if (!RBAC::ResolveContactScope(record.info.id, scope)) {
				return RBAC::IsRootUser(*this);
			}
			return RBAC::HasAccess(*this, "contact", "LIST", scope) ||
				   RBAC::HasAccess(*this, "contact", "READ", scope);
		};

		if (!QB_.Select.empty()) {
			for (const auto &id : SelectedRecords()) {
				ContactDB::RecordName record;
				if (DB_.GetRecord("id", id, record) && allow(record)) {
					entries.push_back(record);
				}
			}
		} else {
			DB_.Iterate([&](const ContactDB::RecordName &record) {
				if (allow(record)) {
					entries.push_back(record);
				}
				return true;
			});
		}

		if (QB_.CountOnly) {
			return ReturnCountOnly(entries.size());
		}
		return MakeJSONObjectArray("contacts",
								   RESTAPI::ApplyPagination(entries, QB_.Offset, QB_.Limit), *this);
	}
} // namespace OpenWifi
