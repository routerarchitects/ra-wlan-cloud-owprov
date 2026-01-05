/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
//
// Created by stephane bourque on 2022-04-06.
//

#include "RESTAPI_sub_devices_handler.h"
#include "APConfig.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "fmt/format.h"
#include "sdks/SDK_gw.h"
#include "sdks/SDK_sec.h"
#ifdef CGW_INTEGRATION
#include "sdks/SDK_cgw.h"
#endif

namespace OpenWifi {

	void RESTAPI_sub_devices_handler::DoGet() {
		auto uuid = GetBinding("uuid");
		if (uuid.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		ProvObjects::SubscriberDevice SD;
		if (Utils::ValidUUID(uuid)) {
			if (!DB_.GetRecord("id", uuid, SD)) {
				return NotFound();
			}
		} else if (Utils::ValidSerialNumber(uuid)) {
			if (!DB_.GetRecord("serialNumber", uuid, SD)) {
				return NotFound();
			}
		} else {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
		}

		Poco::JSON::Object Answer;
		SD.to_json(Answer);
		return ReturnObject(Answer);
	}

	void RESTAPI_sub_devices_handler::DoDelete() {
		auto uuid = GetBinding("uuid");
		if (uuid.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		if (!DB_.Exists("id", uuid)) {
			return NotFound();
		}
		/* When device is removed from the subscriber, if subscriberID is present in groupsmap table.
		*  Call DELETE /DeleteDeviceFromGroup OF CGW-REST
		*/
#ifdef CGW_INTEGRATION
		// Load existing record to obtain subscriberId and MAC/serial
		ProvObjects::SubscriberDevice Existing;
		if (!DB_.GetRecord("id", uuid, Existing)) {
			return NotFound();
		}

		// Resolve CGW groupId from subscriberId via groupsmap
		uint64_t groupId = 0;
		if (!StorageService()->GroupsMapDB().GetGroup(Existing.subscriberId, groupId) ) {
			poco_error(Logger(), fmt::format("Subscriber {} has no CGW groupsmap entry", Existing.subscriberId));
			return BadRequest(RESTAPI::Errors::InvalidSubscriberId);
		}

		if (!Utils::ValidSerialNumber(Existing.serialNumber)) {
			poco_error(Logger(), fmt::format("Invalid MAC/Serial format for CGW delete: {}", Existing.serialNumber));
			return BadRequest(RESTAPI::Errors::InvalidSerialNumber);
		}
		std::string macHex =  Existing.serialNumber;
		Poco::toLowerInPlace(macHex);
		Poco::trimInPlace(macHex);
		auto macColon = Utils::SerialToMAC(macHex);

			// Call CGW to remove device from group
			if (!SDK::CGW::DeleteDeviceFromGroup(groupId, macColon)) {
				poco_error(Logger(), fmt::format("CGW DeleteDeviceFromGroup failed gid={} mac={}", groupId, macColon));
				return BadRequest(RESTAPI::Errors::InternalError);
			}

#endif

		DB_.DeleteRecord("id", uuid);
		return OK();
	}

	void RESTAPI_sub_devices_handler::DoPost() {

		const auto &RawObject = ParsedBody_;
		SubscriberDeviceDB::RecordName NewObject;
		if (!NewObject.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if (!ValidDbId(NewObject.managementPolicy, StorageService()->PolicyDB(), true,
					   RESTAPI::Errors::UnknownManagementPolicyUUID, *this) ||
			!ValidDbId(NewObject.operatorId, StorageService()->OperatorDB(), true,
					   RESTAPI::Errors::InvalidOperatorId, *this) ||
			!ValidDbId(NewObject.serviceClass, StorageService()->ServiceClassDB(), true,
					   RESTAPI::Errors::InvalidServiceClassId, *this) ||
			!ValidSubscriberId(NewObject.subscriberId, true, *this) ||
			(RawObject->has("deviceRules") && !ValidDeviceRules(NewObject.deviceRules, *this)) ||
			!ValidSerialNumber(NewObject.serialNumber, false, *this)) {
			return;
		}

		ProvObjects::CreateObjectInfo(RawObject, UserInfo_.userinfo, NewObject.info);
		/* When device is added in the subscriber, if subscriberID is present in groupsmap table.
		*  Call POST /AddDeviceToGroup of CGW-REST
		*/
#ifdef CGW_INTEGRATION
		// Lookup groupId for this subscriber in groupsmap
		uint64_t groupId = 0;
		if (!StorageService()->GroupsMapDB().GetGroup(NewObject.subscriberId, groupId)) {
			poco_error(Logger(), fmt::format("Subscriber {} has no CGW groupsmap entry", NewObject.subscriberId));
			return BadRequest(RESTAPI::Errors::InvalidSubscriberId);
		}
		
		if (!Utils::ValidSerialNumber(NewObject.serialNumber)) {
			poco_error(Logger(), fmt::format("Invalid MAC/Serial format for CGW mapping: {}", NewObject.serialNumber));
			return BadRequest(RESTAPI::Errors::InvalidSerialNumber);
		}
		
		std::string macHex = NewObject.serialNumber;
		Poco::trimInPlace(macHex);
		Poco::toLowerInPlace(macHex);
		auto macColon = Utils::SerialToMAC(macHex);

		// Call CGW to add device to group
		if (!SDK::CGW::AddDeviceToGroup(groupId, macColon)) {
			poco_error(Logger(), fmt::format("CGW AddDeviceToGroup failed gid={} mac={}", groupId, macColon));
			return BadRequest(RESTAPI::Errors::InternalError);
		}
#endif
		return ReturnCreatedObject(DB_, NewObject, *this);
	}

	void RESTAPI_sub_devices_handler::DoPut() {
		auto uuid = GetBinding("uuid");

		const auto &RawObject = ParsedBody_;
		SubscriberDeviceDB::RecordName UpdateObj;
		if (!UpdateObj.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		SubscriberDeviceDB::RecordName Existing;
		if (!DB_.GetRecord("id", uuid, Existing)) {
			return NotFound();
		}

		if (!ValidDbId(UpdateObj.managementPolicy, StorageService()->PolicyDB(), true,
					   RESTAPI::Errors::UnknownManagementPolicyUUID, *this) ||
			!ValidDbId(UpdateObj.operatorId, StorageService()->OperatorDB(), true,
					   RESTAPI::Errors::InvalidOperatorId, *this) ||
			!ValidDbId(UpdateObj.serviceClass, StorageService()->ServiceClassDB(), true,
					   RESTAPI::Errors::InvalidServiceClassId, *this) ||
			!ValidSubscriberId(UpdateObj.subscriberId, true, *this) ||
			(RawObject->has("deviceRules") && !ValidDeviceRules(UpdateObj.deviceRules, *this)) ||
			!ValidSerialNumber(UpdateObj.serialNumber, false, *this)) {
			return;
		}

		ProvObjects::UpdateObjectInfo(RawObject, UserInfo_.userinfo, Existing.info);
		AssignIfPresent(RawObject, "deviceType", Existing.deviceType);
		AssignIfPresent(RawObject, "subscriberId", Existing.subscriberId);
		AssignIfPresent(RawObject, "managementPolicy", Existing.managementPolicy);
		AssignIfPresent(RawObject, "serviceClass", Existing.serviceClass);
		AssignIfPresent(RawObject, "qrCode", Existing.qrCode);
		AssignIfPresent(RawObject, "geoCode", Existing.geoCode);
		if (RawObject->has("deviceRules"))
			Existing.deviceRules = UpdateObj.deviceRules;
		AssignIfPresent(RawObject, "state", Existing.state);
		AssignIfPresent(RawObject, "locale", Existing.locale);
		AssignIfPresent(RawObject, "billingCode", Existing.billingCode);
		AssignIfPresent(RawObject, "realMacAddress", Existing.realMacAddress);
		AssignIfPresent(RawObject, "contact", UpdateObj.contact, Existing.contact);
		AssignIfPresent(RawObject, "location", UpdateObj.location, Existing.location);

		if (RawObject->has("configuration")) {
			Existing.configuration = UpdateObj.configuration;
		}
		StorageService()->SubscriberDeviceDB().UpdateRecord("id", uuid, Existing);
		ApplyConfiguration(Existing.serialNumber);
		return ReturnUpdatedObject(DB_, Existing, *this);
	}

	bool RESTAPI_sub_devices_handler::ApplyConfiguration(const std::string &SerialNumber) {
		auto Device = std::make_shared<APConfig>(SerialNumber, Logger());
		auto Configuration = Poco::makeShared<Poco::JSON::Object>();
		Poco::JSON::Object ErrorsObj, WarningsObj;
		Logger().debug(Poco::format("%s: Computing configuration.", SerialNumber));
		if (Device->Get(Configuration)) {
			std::ostringstream OS;
			Configuration->stringify(OS);
			auto Response = Poco::makeShared<Poco::JSON::Object>();
			Logger().debug(Poco::format("%s: Sending configuration push.", SerialNumber));
			if (SDK::GW::Device::Configure(this, SerialNumber, Configuration, Response)) {
				Logger().debug(Poco::format("%s: Sending configuration pushed.", SerialNumber));
				return true;
			} else {
				Logger().debug(Poco::format("%s: Sending configuration failed.", SerialNumber));
				return false;
			}
		} else {
			Logger().debug(Poco::format("%s: Configuration is bad.", SerialNumber));
			return false;
		}
	}

} // namespace OpenWifi