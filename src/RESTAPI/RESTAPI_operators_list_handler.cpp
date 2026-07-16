//
// Created by stephane bourque on 2022-04-06.
//

#include "RESTAPI_operators_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"

namespace OpenWifi {
	void RESTAPI_operators_list_handler::DoGet() {
		bool IsRoot = (UserInfo_.userinfo.userRole == SecurityObjects::ROOT || UserInfo_.userinfo.userRole == SecurityObjects::SYSTEM);

		std::set<std::string> AllowedOperatorIds;
		bool AllOperatorsAllowed = IsRoot;

		if (!AllOperatorsAllowed) {
			std::vector<ProvObjects::ManagementRole> Roles;
			if (FindAllUserRoles(UserInfo_.userinfo.id, Roles)) {
				auto &EntityDB = StorageService()->EntityDB();
				for (const auto &role : Roles) {
					if (role.entity.empty()) continue;
					if (role.entity == EntityDB.RootUUID()) {
						AllOperatorsAllowed = true;
						break;
					}
					std::string currentId = role.entity;
					std::set<std::string> visited;
					while (!currentId.empty() && currentId != EntityDB.RootUUID()) {
						if (visited.count(currentId)) {
							break;
						}
						visited.insert(currentId);
						ProvObjects::Entity ent;
						if (!EntityDB.GetRecord("id", currentId, ent)) {
							break;
						}
						if (!ent.operatorId.empty()) {
							AllowedOperatorIds.insert(ent.operatorId);
							break;
						}
						currentId = ent.parent;
					}
				}
			}
		}

		if (QB_.CountOnly) {
			if (AllOperatorsAllowed) {
				auto Count = DB_.Count();
				return ReturnCountOnly(Count);
			} else {
				std::vector<ProvObjects::Operator> AllEntries;
				DB_.GetRecords(0, 100000, AllEntries);
				uint64_t Count = 0;
				for (const auto &op : AllEntries) {
					if (AllowedOperatorIds.count(op.info.id)) {
						Count++;
					}
				}
				return ReturnCountOnly(Count);
			}
		}

		if (!QB_.Select.empty()) {
			if (AllOperatorsAllowed) {
				return ReturnRecordList<decltype(DB_), ProvObjects::Operator>("operators", DB_, *this);
			} else {
				Poco::JSON::Array ObjArr;
				for (const auto &i : SelectedRecords()) {
					ProvObjects::Operator E;
					if (DB_.GetRecord("id", i, E)) {
						if (AllowedOperatorIds.count(E.info.id)) {
							Poco::JSON::Object Obj;
							E.to_json(Obj);
							if (NeedAdditionalInfo())
								AddExtendedInfo(E, Obj);
							ObjArr.add(Obj);
						}
					} else {
						return BadRequest(RESTAPI::Errors::UnknownId);
					}
				}
				Poco::JSON::Object Answer;
				Answer.set("operators", ObjArr);
				return ReturnObject(Answer);
			}
		}

		std::vector<ProvObjects::Operator> Entries;
		if (AllOperatorsAllowed) {
			DB_.GetRecords(QB_.Offset, QB_.Limit, Entries);
		} else {
			std::vector<ProvObjects::Operator> AllEntries;
			DB_.GetRecords(0, 100000, AllEntries);
			std::vector<ProvObjects::Operator> FilteredEntries;
			for (const auto &op : AllEntries) {
				if (AllowedOperatorIds.count(op.info.id)) {
					FilteredEntries.push_back(op);
				}
			}
			for (size_t i = QB_.Offset; i < FilteredEntries.size() && Entries.size() < QB_.Limit; ++i) {
				Entries.push_back(FilteredEntries[i]);
			}
		}
		return MakeJSONObjectArray("operators", Entries, *this);
	}
} // namespace OpenWifi