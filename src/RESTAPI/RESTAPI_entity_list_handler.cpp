//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include "RESTAPI_entity_list_handler.h"
#include "RESTAPI_db_helpers.h"
#include "RESTAPI/RESTAPI_rbac_helpers.h"
#include "StorageService.h"
#include <algorithm>

namespace OpenWifi {
	namespace {
		void CollectVisibleTree(RESTAPIHandler &handler, EntityDB &entityDB,
								const std::string &nodeId, Poco::JSON::Array &nodesOut) {
			ProvObjects::Entity entity;
			if (!entityDB.GetRecord("id", nodeId, entity)) {
				return;
			}

			const bool selfVisible = RBAC::IsEntityVisible(handler, entity.info.id);

			Poco::JSON::Array collectedChildren;
			for (const auto &childId : entity.children) {
				CollectVisibleTree(handler, entityDB, childId, collectedChildren);
			}

			Poco::JSON::Array collectedVenues;
			for (const auto &venueId : entity.venues) {
				if (!RBAC::IsVenueVisible(handler, venueId)) {
					continue;
				}
				Poco::JSON::Object venueNode;
				entityDB.AddVenues(venueNode, venueId);
				collectedVenues.add(venueNode);
			}

			if (selfVisible) {
				Poco::JSON::Object entityNode;
				entityNode.set("type", "entity");
				entityNode.set("name", entity.info.name);
				entityNode.set("uuid", entity.info.id);
				entityNode.set("children", collectedChildren);
				entityNode.set("venues", collectedVenues);
				nodesOut.add(entityNode);
				return;
			}

			for (unsigned i = 0; i < collectedChildren.size(); ++i) {
				nodesOut.add(collectedChildren.get(i));
			}
			for (unsigned i = 0; i < collectedVenues.size(); ++i) {
				nodesOut.add(collectedVenues.get(i));
			}
		}

		std::vector<ProvObjects::Entity> CollectVisibleEntities(RESTAPIHandler &handler,
																EntityDB &entityDB) {
			std::vector<ProvObjects::Entity> visible;
			entityDB.Iterate([&](const ProvObjects::Entity &entity) {
				if (RBAC::IsEntityVisible(handler, entity.info.id)) {
					visible.push_back(entity);
				}
				return true;
			});
			return visible;
		}

		template <typename T>
		std::vector<T> ApplyPagination(const std::vector<T> &items, uint64_t offset,
									   uint64_t limit) {
			if (offset >= items.size()) {
				return {};
			}
			auto start = static_cast<std::size_t>(offset);
			auto end = limit == 0 ? start : std::min<std::size_t>(items.size(),
																 start + static_cast<std::size_t>(limit));
			return std::vector<T>(items.begin() + start, items.begin() + end);
		}
	} // namespace

	void RESTAPI_entity_list_handler::DoGet() {
		if (RBAC::IsRootUser(*this)) {
			if (!QB_.Select.empty()) {
				EntityDB::RecordVec selectedEntities;
				for (const auto &id : SelectedRecords()) {
					ProvObjects::Entity entity;
					if (DB_.GetRecord("id", id, entity)) {
						selectedEntities.push_back(entity);
					}
				}
				if (QB_.CountOnly) {
					return ReturnCountOnly(selectedEntities.size());
				}
				return MakeJSONObjectArray("entities", selectedEntities, *this);
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
		} else if (GetBoolParameter("getTree", false)) {
			Poco::JSON::Object emptyTree;
			emptyTree.set("type", "entity");
			emptyTree.set("name", "root");
			emptyTree.set("uuid", EntityDB::RootUUID());
			Poco::JSON::Array filteredChildren;
			CollectVisibleTree(*this, DB_, EntityDB::RootUUID(), filteredChildren);
			emptyTree.set("children", filteredChildren);
			emptyTree.set("venues", Poco::JSON::Array());
			return ReturnObject(emptyTree);
		} else {
			if (!QB_.Select.empty()) {
				EntityDB::RecordVec selectedVisible;
				for (const auto &id : SelectedRecords()) {
					ProvObjects::Entity entity;
					if (DB_.GetRecord("id", id, entity) &&
						RBAC::IsEntityVisible(*this, entity.info.id)) {
						selectedVisible.push_back(entity);
					}
				}
				if (QB_.CountOnly) {
					return ReturnCountOnly(selectedVisible.size());
				}
				return MakeJSONObjectArray("entities", selectedVisible, *this);
			}
			EntityDB::RecordVec visibleEntities = CollectVisibleEntities(*this, DB_);
			if (QB_.CountOnly) {
				return ReturnCountOnly(visibleEntities.size());
			}
			return MakeJSONObjectArray("entities",
									   ApplyPagination(visibleEntities, QB_.Offset, QB_.Limit),
									   *this);
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
