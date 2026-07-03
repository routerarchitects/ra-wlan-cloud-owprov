//
// Created by stephane bourque on 2021-11-09.
//

#include "RESTAPI_map_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"

namespace OpenWifi {
	void RESTAPI_map_list_handler::DoGet() {
		const char *BlockName{"list"};
		auto allow = [&](const auto &record) {
			RBAC::TargetScope scope;
			if (!RBAC::ResolveMapScope(record.info.id, scope)) {
				return RBAC::IsRootUser(*this);
			}
			return RBAC::HasAccess(*this, "map", "LIST", scope) ||
				   RBAC::HasAccess(*this, "map", "READ", scope);
		};
		auto collect = [&](auto &&predicate) {
			MapDB::RecordVec maps;
			DB_.Iterate([&](const MapDB::RecordName &record) {
				if (predicate(record) && allow(record)) {
					maps.push_back(record);
				}
				return true;
			});
			return maps;
		};
		if (GetBoolParameter("myMaps", false)) {
			auto Maps = collect([&](const auto &record) { return record.creator == UserInfo_.userinfo.id; });
			if (QB_.CountOnly) {
				return ReturnCountOnly(Maps.size());
			}
			return MakeJSONObjectArray(BlockName,
									   RESTAPI::ApplyPagination(Maps, QB_.Offset, QB_.Limit), *this);
		} else if (GetBoolParameter("sharedWithMe", false)) {
			auto Maps = collect([](const auto &) { return true; });
			if (QB_.CountOnly) {
				return ReturnCountOnly(Maps.size());
			}
			return MakeJSONObjectArray(BlockName,
									   RESTAPI::ApplyPagination(Maps, QB_.Offset, QB_.Limit), *this);
		} else {
			auto Maps = collect([](const auto &) { return true; });
			if (QB_.CountOnly) {
				return ReturnCountOnly(Maps.size());
			}
			return MakeJSONObjectArray(BlockName,
									   RESTAPI::ApplyPagination(Maps, QB_.Offset, QB_.Limit), *this);
		}
	}
} // namespace OpenWifi
