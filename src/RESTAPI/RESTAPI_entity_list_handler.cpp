//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include "RESTAPI_entity_list_handler.h"
#include "RESTAPI_db_helpers.h"
#include "StorageService.h"

namespace OpenWifi {

	void RESTAPI_entity_list_handler::DoGet() {
		bool isRootOrSystem = (UserInfo_.userinfo.userRole == SecurityObjects::ROOT || UserInfo_.userinfo.userRole == SecurityObjects::SYSTEM);

		if (isRootOrSystem) {
			if (!QB_.Select.empty()) {
				return ReturnRecordList<decltype(DB_), ProvObjects::Entity>("entities", DB_, *this);
			} else if (QB_.CountOnly) {
				auto C = DB_.Count();
				return ReturnCountOnly(C);
			} else if (GetBoolParameter("getTree", false)) {
				Poco::JSON::Object FullTree;
				DB_.BuildTree(FullTree);
				return ReturnObject(FullTree);
			} else {
				EntityDB::RecordVec Entities;
				DB_.GetRecords(QB_.Offset, QB_.Limit, Entities);
				return MakeJSONObjectArray("entities", Entities, *this);
			}
		}

		// Standard user scope filtering
		std::vector<ProvObjects::ManagementRole> Roles;
		std::set<std::string> AssignedEntities;
		if (FindAllUserRoles(UserInfo_.userinfo.id, Roles)) {
			for (const auto &role : Roles) {
				if (!role.entity.empty()) {
					AssignedEntities.insert(role.entity);
				}
			}
		}

		if (AssignedEntities.empty()) {
			if (GetBoolParameter("getTree", false)) {
				Poco::JSON::Object EmptyTree;
				return ReturnObject(EmptyTree);
			}
			EntityDB::RecordVec Entities;
			return MakeJSONObjectArray("entities", Entities, *this);
		}

		if (GetBoolParameter("getTree", false)) {
			if (AssignedEntities.size() == 1) {
				Poco::JSON::Object Tree;
				DB_.BuildTree(Tree, *AssignedEntities.begin());
				return ReturnObject(Tree);
			} else {
				Poco::JSON::Object VirtualRoot;
				VirtualRoot.set("type", "entity");
				VirtualRoot.set("name", "Assigned Entities");
				VirtualRoot.set("uuid", "0000-0000-0000");
				Poco::JSON::Array Children;
				for (const auto &entId : AssignedEntities) {
					Poco::JSON::Object SubTree;
					DB_.BuildTree(SubTree, entId);
					Children.add(SubTree);
				}
				VirtualRoot.set("children", Children);
				Poco::JSON::Array Venues;
				VirtualRoot.set("venues", Venues);
				return ReturnObject(VirtualRoot);
			}
		}

		std::set<std::string> descendants;
		for (const auto &entId : AssignedEntities) {
			GetDescendantEntities(entId, descendants);
		}

		if (!QB_.Select.empty()) {
			std::vector<std::string> FilteredSelect;
			for (const auto &id : QB_.Select) {
				if (descendants.count(id)) {
					FilteredSelect.push_back(id);
				}
			}
			auto origSelect = QB_.Select;
			QB_.Select = FilteredSelect;
			ReturnRecordList<decltype(DB_), ProvObjects::Entity>("entities", DB_, *this);
			QB_.Select = origSelect;
			return;
		} else if (QB_.CountOnly) {
			return ReturnCountOnly(descendants.size());
		} else {
			EntityDB::RecordVec AllEntities;
			DB_.GetRecords(0, 10000, AllEntities);
			EntityDB::RecordVec FilteredEntities;
			for (const auto &ent : AllEntities) {
				if (descendants.count(ent.info.id)) {
					FilteredEntities.push_back(ent);
				}
			}

			EntityDB::RecordVec PaginatedEntities;
			uint64_t offset = QB_.Offset;
			uint64_t limit = QB_.Limit;
			if (limit == 0) limit = 100;
			uint64_t count = 0;
			for (size_t i = offset; i < FilteredEntities.size() && count < limit; ++i) {
				PaginatedEntities.push_back(FilteredEntities[i]);
				count++;
			}
			return MakeJSONObjectArray("entities", PaginatedEntities, *this);
		}
	}

	void RESTAPI_entity_list_handler::DoPost() {
		if (GetBoolParameter("setTree", false)) {
			const auto &FullTree = ParsedBody_;
			DB_.ImportTree(FullTree);
			return OK();
		}
		BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
	}
} // namespace OpenWifi