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
#include <map>

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

		std::vector<ProvObjects::ManagementRole> Roles;
		std::set<std::string> VisibleEntities;
		std::set<std::string> VisibleVenues;
		auto policyAllowsGet = [&](const ProvObjects::ManagementRole &role, const std::string &resource) -> bool {
			ProvObjects::ManagementPolicy Policy;
			if (!AuthCache::GetInstance()->GetPolicy(role.managementPolicy, Policy)) {
				if (!StorageService()->PolicyDB().GetRecord("id", role.managementPolicy, Policy)) {
					return false;
				}
				AuthCache::GetInstance()->SetPolicy(role.managementPolicy, Policy);
			}
			return PolicyAllows(Policy, resource, Poco::Net::HTTPRequest::HTTP_GET);
		};

		if (FindAllUserRoles(UserInfo_.userinfo.id, Roles)) {
			for (const auto &role : Roles) {
				if (!role.venue.empty()) {
					if (policyAllowsGet(role, "venue")) {
						VisibleVenues.insert(role.venue);
					}
				} else if (!role.entity.empty() && policyAllowsGet(role, "entity")) {
					VisibleEntities.insert(role.entity);
				}
			}
		}

		if (VisibleEntities.empty() && VisibleVenues.empty()) {
			if (GetBoolParameter("getTree", false)) {
				Poco::JSON::Object EmptyTree;
				return ReturnObject(EmptyTree);
			}
			EntityDB::RecordVec Entities;
			return MakeJSONObjectArray("entities", Entities, *this);
		}

		if (GetBoolParameter("getTree", false)) {
			std::map<std::string, ProvObjects::Entity> EntityRecords;
			for (const auto &entityId : VisibleEntities) {
				ProvObjects::Entity Entity;
				if (StorageService()->EntityDB().GetRecord("id", entityId, Entity)) {
					EntityRecords[entityId] = Entity;
				}
			}

			std::map<std::string, ProvObjects::Venue> VenueRecords;
			for (const auto &venueId : VisibleVenues) {
				ProvObjects::Venue Venue;
				if (StorageService()->VenueDB().GetRecord("id", venueId, Venue)) {
					VenueRecords[venueId] = Venue;
				}
			}

			std::function<Poco::JSON::Object::Ptr(const std::string &)> buildVenueNode;
			buildVenueNode = [&](const std::string &venueId) -> Poco::JSON::Object::Ptr {
				auto venueIt = VenueRecords.find(venueId);
				if (venueIt == VenueRecords.end()) {
					return nullptr;
				}
				Poco::JSON::Object::Ptr Node = new Poco::JSON::Object;
				Poco::JSON::Array Children;
				for (const auto &childId : venueIt->second.children) {
					auto ChildNode = buildVenueNode(childId);
					if (ChildNode) {
						Children.add(ChildNode);
					}
				}
				Node->set("type", "venue");
				Node->set("name", venueIt->second.info.name);
				Node->set("uuid", venueIt->second.info.id);
				Node->set("children", Children);
				return Node;
			};

			std::function<Poco::JSON::Object::Ptr(const std::string &)> buildEntityNode;
			buildEntityNode = [&](const std::string &entityId) -> Poco::JSON::Object::Ptr {
				auto entityIt = EntityRecords.find(entityId);
				if (entityIt == EntityRecords.end()) {
					return nullptr;
				}
				Poco::JSON::Object::Ptr Node = new Poco::JSON::Object;
				Poco::JSON::Array Children;
				for (const auto &childId : entityIt->second.children) {
					auto ChildNode = buildEntityNode(childId);
					if (ChildNode) {
						Children.add(ChildNode);
					}
				}
				Poco::JSON::Array Venues;
				for (const auto &venueId : entityIt->second.venues) {
					auto venueIt = VenueRecords.find(venueId);
					if (venueIt != VenueRecords.end() && venueIt->second.parent.empty()) {
						auto VenueNode = buildVenueNode(venueId);
						if (VenueNode) {
							Venues.add(VenueNode);
						}
					}
				}
				Node->set("type", "entity");
				Node->set("name", entityIt->second.info.name);
				Node->set("uuid", entityIt->second.info.id);
				Node->set("children", Children);
				Node->set("venues", Venues);
				return Node;
			};

			Poco::JSON::Array RootEntities;
			size_t rootEntityCount = 0;
			Poco::JSON::Object::Ptr SingleEntityTree;
			for (const auto &[entityId, Entity] : EntityRecords) {
				if (!Entity.parent.empty() && VisibleEntities.count(Entity.parent)) {
					continue;
				}
				auto EntityNode = buildEntityNode(entityId);
				if (EntityNode) {
					RootEntities.add(EntityNode);
					SingleEntityTree = EntityNode;
					++rootEntityCount;
				}
			}

			Poco::JSON::Array RootVenues;
			size_t rootVenueCount = 0;
			Poco::JSON::Object::Ptr SingleVenueTree;
			for (const auto &[venueId, Venue] : VenueRecords) {
				if (!Venue.parent.empty() && VisibleVenues.count(Venue.parent)) {
					continue;
				}
				if (Venue.parent.empty() && VisibleEntities.count(Venue.entity)) {
					continue;
				}
				auto VenueNode = buildVenueNode(venueId);
				if (VenueNode) {
					RootVenues.add(VenueNode);
					SingleVenueTree = VenueNode;
					++rootVenueCount;
				}
			}

			if (rootEntityCount == 0 && rootVenueCount == 0) {
				Poco::JSON::Object EmptyTree;
				return ReturnObject(EmptyTree);
			}

			if (rootEntityCount == 1 && rootVenueCount == 0 && SingleEntityTree) {
				return ReturnObject(*SingleEntityTree);
			}

			if (rootEntityCount == 0 && rootVenueCount == 1 && SingleVenueTree) {
				return ReturnObject(*SingleVenueTree);
			}

			Poco::JSON::Object ScopedTree;
			ScopedTree.set("type", "entity");
			ScopedTree.set("name", "Assigned Scopes");
			ScopedTree.set("uuid", "0000-0000-0000");
			ScopedTree.set("children", RootEntities);
			ScopedTree.set("venues", RootVenues);
			return ReturnObject(ScopedTree);
		}

		if (!QB_.Select.empty()) {
			std::vector<std::string> FilteredSelect;
			for (const auto &id : QB_.Select) {
				if (VisibleEntities.count(id)) {
					FilteredSelect.push_back(id);
				}
			}
			auto origSelect = QB_.Select;
			QB_.Select = FilteredSelect;
			ReturnRecordList<decltype(DB_), ProvObjects::Entity>("entities", DB_, *this);
			QB_.Select = origSelect;
			return;
		} else if (QB_.CountOnly) {
			return ReturnCountOnly(VisibleEntities.size());
		} else {
			EntityDB::RecordVec AllEntities;
			DB_.GetRecords(0, 10000, AllEntities);
			EntityDB::RecordVec FilteredEntities;
			for (const auto &ent : AllEntities) {
				if (VisibleEntities.count(ent.info.id)) {
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
