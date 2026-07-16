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

		const bool SerialOnly = GetBoolParameter("serialOnly");
		std::string UUID;
		std::string Arg;
		std::string OrderBy{" ORDER BY serialNumber ASC "};
		if (HasParameter("orderBy", Arg)) {
			if (!DB_.PrepareOrderBy(Arg, OrderBy)) {
				return BadRequest(RESTAPI::Errors::InvalidLOrderBy);
			}
		}

		auto allow = [&](const ProvObjects::InventoryTag &record) {
			RBAC::TargetScope scope;
			if (!RBAC::ResolveInventoryScope(record.serialNumber, scope)) {
				return RBAC::IsRootUser(*this);
			}
			return RBAC::HasAccess(*this, "inventory", "LIST", scope) ||
				   RBAC::HasAccess(*this, "inventory", "READ", scope);
		};

		auto filterAndSend = [&](ProvObjects::InventoryTagVec tags) {
			tags.erase(std::remove_if(tags.begin(), tags.end(),
									  [&](const auto &record) { return !allow(record); }),
					   tags.end());
			if (QB_.CountOnly) {
				return ReturnCountOnly(tags.size());
			}
			return SendList(tags, SerialOnly);
		};

		if (!QB_.Select.empty()) {
			ProvObjects::InventoryTagVec tags;
			for (const auto &id : SelectedRecords()) {
				ProvObjects::InventoryTag record;
				if (DB_.GetRecord(is_uuid(id) ? "id" : "serialNumber", id, record) &&
					allow(record)) {
					tags.push_back(record);
				}
			}
			return filterAndSend(tags);
		}

		if (HasParameter("entity", UUID)) {
			ProvObjects::InventoryTagVec tags;
			DB_.GetRecords(QB_.Offset, QB_.Limit, tags, DB_.OP("entity", ORM::EQ, UUID), OrderBy);
			return filterAndSend(tags);
		}

		if (HasParameter("venue", UUID)) {
			ProvObjects::InventoryTagVec tags;
			DB_.GetRecords(QB_.Offset, QB_.Limit, tags, DB_.OP("venue", ORM::EQ, UUID), OrderBy);
			return filterAndSend(tags);
		}

		if (GetBoolParameter("subscribersOnly") && GetBoolParameter("unassigned")) {
			ProvObjects::InventoryTagVec tags;
			DB_.GetRecords(QB_.Offset, QB_.Limit, tags, " devClass='subscriber' and subscriber='' ",
						   OrderBy);
			return filterAndSend(tags);
		}

		if (GetBoolParameter("subscribersOnly")) {
			ProvObjects::InventoryTagVec tags;
			DB_.GetRecords(QB_.Offset, QB_.Limit, tags,
						   " devClass='subscriber' and subscriber!='' ", OrderBy);
			return filterAndSend(tags);
		}

		if (GetBoolParameter("unassigned")) {
			ProvObjects::InventoryTagVec tags;
			std::string Empty;
			DB_.GetRecords(QB_.Offset, QB_.Limit, tags,
						   InventoryDB::OP(DB_.OP("venue", ORM::EQ, Empty), ORM::AND,
										   DB_.OP("entity", ORM::EQ, Empty)),
						   OrderBy);
			return filterAndSend(tags);
		}

		if (HasParameter("subscriber", Arg) && !Arg.empty()) {
			ProvObjects::InventoryTagVec tags;
			DB_.GetRecords(0, 100, tags, " subscriber='" + ORM::Escape(Arg) + "'");
			return filterAndSend(std::move(tags));
		}

		if (GetBoolParameter("rrmOnly")) {
			Types::UUIDvec_t deviceList;
			DB_.GetRRMDeviceList(deviceList);
			ProvObjects::InventoryTagVec tags;
			for (const auto &serial : deviceList) {
				ProvObjects::InventoryTag record;
				if (DB_.GetRecord("serialNumber", serial, record) && allow(record)) {
					tags.push_back(record);
				}
			}
			return filterAndSend(std::move(tags));
		}

		ProvObjects::InventoryTagVec tags;
		DB_.GetRecords(QB_.Offset, QB_.Limit, tags, "", OrderBy);
		return filterAndSend(tags);
	}
} // namespace OpenWifi
