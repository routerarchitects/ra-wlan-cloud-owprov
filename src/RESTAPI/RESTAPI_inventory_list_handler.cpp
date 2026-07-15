//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include "RESTAPI_inventory_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "StorageService.h"

namespace OpenWifi {
	void RESTAPI_inventory_list_handler::SendList(const ProvObjects::InventoryTagVec &Tags,
												  bool SerialOnly) {
		Poco::JSON::Array Array;
		for (const auto &i : Tags) {
			if (SerialOnly) {
				Array.add(i.serialNumber);
			} else {
				Poco::JSON::Object O;
				i.to_json(O);
				if (QB_.AdditionalInfo)
					AddExtendedInfo(i, O);
				Array.add(O);
			}
		}
		Poco::JSON::Object Answer;
		if (SerialOnly)
			Answer.set("serialNumbers", Array);
		else
			Answer.set("taglist", Array);
		ReturnObject(Answer);
	}

	void RESTAPI_inventory_list_handler::DoGet() {

		if (GetBoolParameter("orderSpec")) {
			return ReturnFieldList(DB_, *this);
		}

		bool SerialOnly = GetBoolParameter("serialOnly");
		bool isRootOrSystem = (UserInfo_.userinfo.userRole == SecurityObjects::ROOT || UserInfo_.userinfo.userRole == SecurityObjects::SYSTEM);

		if (isRootOrSystem) {
			std::string UUID;
			std::string Arg, Arg2;

			std::string OrderBy{" ORDER BY serialNumber ASC "};
			if (HasParameter("orderBy", Arg)) {
				if (!DB_.PrepareOrderBy(Arg, OrderBy)) {
					return BadRequest(RESTAPI::Errors::InvalidLOrderBy);
				}
			}

			if (!QB_.Select.empty()) {
				return ReturnRecordList<decltype(DB_)>("taglist", DB_, *this);
			} else if (HasParameter("entity", UUID)) {
				if (QB_.CountOnly) {
					auto C = DB_.Count(StorageService()->InventoryDB().OP("entity", ORM::EQ, UUID));
					return ReturnCountOnly(C);
				}
				ProvObjects::InventoryTagVec Tags;
				DB_.GetRecords(QB_.Offset, QB_.Limit, Tags, DB_.OP("entity", ORM::EQ, UUID), OrderBy);
				return SendList(Tags, SerialOnly);
			} else if (HasParameter("venue", UUID)) {
				if (QB_.CountOnly) {
					auto C = DB_.Count(DB_.OP("venue", ORM::EQ, UUID));
					return ReturnCountOnly(C);
				}
				ProvObjects::InventoryTagVec Tags;
				DB_.GetRecords(QB_.Offset, QB_.Limit, Tags, DB_.OP("venue", ORM::EQ, UUID), OrderBy);
				return SendList(Tags, SerialOnly);
			} else if (GetBoolParameter("subscribersOnly") && GetBoolParameter("unassigned")) {
				if (QB_.CountOnly) {
					auto C = DB_.Count(" devClass='subscriber' and subscriber='' ");
					return ReturnCountOnly(C);
				}
				ProvObjects::InventoryTagVec Tags;
				DB_.GetRecords(QB_.Offset, QB_.Limit, Tags, " devClass='subscriber' and subscriber='' ",
							   OrderBy);
				if (QB_.CountOnly) {
					auto C = DB_.Count(DB_.OP("venue", ORM::EQ, UUID));
					return ReturnCountOnly(C);
				}
				return SendList(Tags, SerialOnly);
			} else if (GetBoolParameter("subscribersOnly")) {
				if (QB_.CountOnly) {
					auto C = DB_.Count(" devClass='subscriber' and subscriber!='' ");
					return ReturnCountOnly(C);
				}
				ProvObjects::InventoryTagVec Tags;
				DB_.GetRecords(QB_.Offset, QB_.Limit, Tags,
							   " devClass='subscriber' and subscriber!='' ", OrderBy);
				return SendList(Tags, SerialOnly);
			} else if (GetBoolParameter("unassigned")) {
				if (QB_.CountOnly) {
					std::string Empty;
					auto C = DB_.Count(InventoryDB::OP(DB_.OP("venue", ORM::EQ, Empty), ORM::AND,
													   DB_.OP("entity", ORM::EQ, Empty)));
					return ReturnCountOnly(C);
				}
				ProvObjects::InventoryTagVec Tags;
				std::string Empty;
				DB_.GetRecords(QB_.Offset, QB_.Limit, Tags,
							   InventoryDB::OP(DB_.OP("venue", ORM::EQ, Empty), ORM::AND,
											   DB_.OP("entity", ORM::EQ, Empty)),
							   OrderBy);
				return SendList(Tags, SerialOnly);
			} else if (HasParameter("subscriber", Arg) && !Arg.empty()) {
				ProvObjects::InventoryTagVec Tags;
				DB_.GetRecords(0, 100, Tags, " subscriber='" + ORM::Escape(Arg) + "'");
				if (SerialOnly) {
					std::vector<std::string> SerialNumbers;
					std::transform(cbegin(Tags), cend(Tags), std::back_inserter(SerialNumbers),
								   [](const auto &T) { return T.serialNumber; });
					return ReturnObject("serialNumbers", SerialNumbers);
				} else {
					return MakeJSONObjectArray("taglist", Tags, *this);
				}
			} else if (QB_.CountOnly) {
				auto C = DB_.Count();
				return ReturnCountOnly(C);
			} else if (GetBoolParameter("rrmOnly")) {
				Types::UUIDvec_t DeviceList;
				DB_.GetRRMDeviceList(DeviceList);
				if (QB_.CountOnly)
					return ReturnCountOnly(DeviceList.size());
				else {
					return ReturnObject("serialNumbers", DeviceList);
				}
			} else {
				ProvObjects::InventoryTagVec Tags;
				DB_.GetRecords(QB_.Offset, QB_.Limit, Tags, "", OrderBy);
				return SendList(Tags, SerialOnly);
			}
		}

		// Standard user flow:
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
			ProvObjects::InventoryTagVec Tags;
			return SendList(Tags, SerialOnly);
		}

		// 1. Get all descendant entities
		std::set<std::string> descendantEntities;
		for (const auto &entId : AssignedEntities) {
			GetDescendantEntities(entId, descendantEntities);
		}

		// 2. Get all venues associated with these entities
		std::set<std::string> allowedVenues;
		for (const auto &entId : descendantEntities) {
			ProvObjects::Entity E;
			if (StorageService()->EntityDB().GetRecord("id", entId, E)) {
				for (const auto &vId : E.venues) {
					GetDescendantVenues(vId, allowedVenues);
				}
			}
		}

		// 3. Resolve order parameter
		std::string OrderBy{" ORDER BY serialNumber ASC "};
		std::string Arg;
		if (HasParameter("orderBy", Arg)) {
			if (!DB_.PrepareOrderBy(Arg, OrderBy)) {
				return BadRequest(RESTAPI::Errors::InvalidLOrderBy);
			}
		}

		// 4. Retrieve requested or matching tags
		ProvObjects::InventoryTagVec AllTags;
		std::string paramUUID;
		if (!QB_.Select.empty()) {
			for (const auto &id : QB_.Select) {
				ProvObjects::InventoryTag tag;
				if (DB_.GetRecord("id", id, tag)) {
					AllTags.push_back(tag);
				}
			}
		} else if (HasParameter("entity", paramUUID)) {
			if (descendantEntities.count(paramUUID)) {
				DB_.GetRecords(0, 10000, AllTags, DB_.OP("entity", ORM::EQ, paramUUID), OrderBy);
			}
		} else if (HasParameter("venue", paramUUID)) {
			if (allowedVenues.count(paramUUID)) {
				DB_.GetRecords(0, 10000, AllTags, DB_.OP("venue", ORM::EQ, paramUUID), OrderBy);
			}
		} else {
			// Retrieve all tags (up to 10000)
			DB_.GetRecords(0, 10000, AllTags, "", OrderBy);
		}

		// 5. Apply visibility filter
		ProvObjects::InventoryTagVec FilteredTags;
		for (const auto &tag : AllTags) {
			if (descendantEntities.count(tag.entity) || allowedVenues.count(tag.venue)) {
				FilteredTags.push_back(tag);
			}
		}

		if (QB_.CountOnly) {
			return ReturnCountOnly(FilteredTags.size());
		}

		// 6. Apply manual offset/limit pagination
		ProvObjects::InventoryTagVec PaginatedTags;
		uint64_t offset = QB_.Offset;
		uint64_t limit = QB_.Limit;
		if (limit == 0) limit = 100;
		uint64_t count = 0;
		for (size_t i = offset; i < FilteredTags.size() && count < limit; ++i) {
			PaginatedTags.push_back(FilteredTags[i]);
			count++;
		}
		return SendList(PaginatedTags, SerialOnly);
	}
} // namespace OpenWifi