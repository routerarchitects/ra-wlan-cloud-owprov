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
		std::set<std::string> AllowedEntities;
		std::set<std::string> AllowedVenues;
		if (FindAllUserRoles(UserInfo_.userinfo.id, Roles)) {
			for (const auto &role : Roles) {
				if (!role.venue.empty()) {
					AllowedVenues.insert(role.venue);
				} else if (!role.entity.empty()) {
					AllowedEntities.insert(role.entity);
				}
			}
		}

		std::set<std::string> expandedAllowedEntities;
		for (const auto &entId : AllowedEntities) {
			GetDescendantEntities(entId, expandedAllowedEntities);
		}

		std::set<std::string> expandedAllowedVenues;
		for (const auto &entId : expandedAllowedEntities) {
			ProvObjects::Entity E;
			if (StorageService()->EntityDB().GetRecord("id", entId, E)) {
				for (const auto &vId : E.venues) {
					GetDescendantVenues(vId, expandedAllowedVenues);
				}
			}
		}
		for (const auto &vId : AllowedVenues) {
			GetDescendantVenues(vId, expandedAllowedVenues);
		}

		if (expandedAllowedEntities.empty() && expandedAllowedVenues.empty()) {
			if (GetBoolParameter("getTree", false)) {
				Poco::JSON::Object EmptyTree;
				return ReturnObject(EmptyTree);
			}
			EntityDB::RecordVec Entities;
			return MakeJSONObjectArray("entities", Entities, *this);
		}

		if (GetBoolParameter("getTree", false)) {
			std::vector<ProvObjects::ManagementRole> ScopedRoles;
			if (!FindAllUserRoles(UserInfo_.userinfo.id, ScopedRoles) || ScopedRoles.empty()) {
				Poco::JSON::Object EmptyTree;
				return ReturnObject(EmptyTree);
			}

			std::set<std::string> AddedScopes;
			Poco::JSON::Array ScopedEntities;
			Poco::JSON::Array ScopedVenues;
			Poco::JSON::Object::Ptr SingleEntityTree;
			Poco::JSON::Object::Ptr SingleVenueTree;
			size_t entityCount = 0;
			size_t venueCount = 0;

			for (const auto &role : ScopedRoles) {
				if (!role.venue.empty()) {
					std::string scopeKey = "venue:" + role.venue;
					if (AddedScopes.insert(scopeKey).second) {
						Poco::JSON::Object::Ptr VenueTree = new Poco::JSON::Object;
						DB_.AddVenues(*VenueTree, role.venue);
						if (VenueTree->has("uuid")) {
							ScopedVenues.add(VenueTree);
							SingleVenueTree = VenueTree;
							++venueCount;
						}
					}
				} else if (!role.entity.empty()) {
					std::string scopeKey = "entity:" + role.entity;
					if (AddedScopes.insert(scopeKey).second) {
						Poco::JSON::Object::Ptr EntityTree = new Poco::JSON::Object;
						DB_.BuildTree(*EntityTree, role.entity);
						if (EntityTree->has("uuid")) {
							ScopedEntities.add(EntityTree);
							SingleEntityTree = EntityTree;
							++entityCount;
						}
					}
				}
			}

			if (ScopedEntities.empty() && ScopedVenues.empty()) {
				Poco::JSON::Object EmptyTree;
				return ReturnObject(EmptyTree);
			}

			if (entityCount == 1 && venueCount == 0 && SingleEntityTree) {
				return ReturnObject(*SingleEntityTree);
			}

			if (entityCount == 0 && venueCount == 1 && SingleVenueTree) {
				return ReturnObject(*SingleVenueTree);
			}

			Poco::JSON::Object ScopedTree;
			ScopedTree.set("type", "entity");
			ScopedTree.set("name", "Assigned Scopes");
			ScopedTree.set("uuid", "0000-0000-0000");
			ScopedTree.set("children", ScopedEntities);
			ScopedTree.set("venues", ScopedVenues);
			return ReturnObject(ScopedTree);
		}

		if (!QB_.Select.empty()) {
			std::vector<std::string> FilteredSelect;
			for (const auto &id : QB_.Select) {
				if (expandedAllowedEntities.count(id)) {
					FilteredSelect.push_back(id);
				}
			}
			auto origSelect = QB_.Select;
			QB_.Select = FilteredSelect;
			ReturnRecordList<decltype(DB_), ProvObjects::Entity>("entities", DB_, *this);
			QB_.Select = origSelect;
			return;
		} else if (QB_.CountOnly) {
			return ReturnCountOnly(expandedAllowedEntities.size());
		} else {
			EntityDB::RecordVec AllEntities;
			DB_.GetRecords(0, 10000, AllEntities);
			EntityDB::RecordVec FilteredEntities;
			for (const auto &ent : AllEntities) {
				if (expandedAllowedEntities.count(ent.info.id)) {
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
