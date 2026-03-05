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
#include "Poco/Net/HTTPServerResponse.h"
#include "Poco/String.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "SerialNumberCache.h"
#include "framework/RESTAPI_utils.h"
#include "framework/utils.h"
#include "sdks/SDK_analytics.h"
#include "sdks/SDK_gw.h"
#include "sdks/SDK_sec.h"
#include <algorithm>
#include <set>

namespace OpenWifi {
	void RESTAPI_sub_devices_handler::CleanupSubscriberConfigurationRecord(
		const std::string &deviceConfiguration, const std::string &inventoryId) {
		if (deviceConfiguration.empty()) {
			return;
		}

		auto &configurationDB = StorageService()->ConfigurationDB();

		if (!inventoryId.empty()) {
			configurationDB.DeleteInUse("id", deviceConfiguration,
										StorageService()->InventoryDB().Prefix(), inventoryId);
		}

		if (!configurationDB.DeleteRecord("id", deviceConfiguration)) {
			poco_warning(Logger(), fmt::format("[SUBSCRIBER_DEVICE_DELETE]: Failed to delete "
											   "configuration [{}].",
											   deviceConfiguration));
		}
	}

	bool RESTAPI_sub_devices_handler::PushConfigurationToDevice(const std::string &SerialNumber) {
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

	bool RESTAPI_sub_devices_handler::LoadDeviceFromBinding(ProvObjects::SubscriberDevice &device,
															bool allowSerialLookup) {
		auto uuid = GetBinding("uuid");
		if (uuid.empty()) {
			BadRequest(RESTAPI::Errors::MissingUUID);
			return false;
		}

		if (Utils::ValidUUID(uuid)) {
			if (!DB_.GetRecord("id", uuid, device)) {
				NotFound();
				return false;
			}
			return true;
		}

		if (allowSerialLookup && Utils::ValidSerialNumber(uuid)) {
			if (!DB_.GetRecord("serialNumber", uuid, device)) {
				NotFound();
				return false;
			}
			return true;
		}

		BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
		return false;
	}

	bool RESTAPI_sub_devices_handler::ValidateDeleteRequest(const std::string &deviceIdOrSerial,
															ProvObjects::SubscriberDevice &device) {
		if (deviceIdOrSerial.empty()) {
			BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
			return false;
		}

		if (Utils::ValidSerialNumber(deviceIdOrSerial)) {
			if (!DB_.GetRecord("serialNumber", deviceIdOrSerial, device)) {
				NotFound();
				return false;
			}
		} else if (Utils::ValidUUID(deviceIdOrSerial)) {
			if (!DB_.GetRecord("id", deviceIdOrSerial, device)) {
				NotFound();
				return false;
			}
		} else {
			BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
			return false;
		}

		Poco::toLowerInPlace(device.deviceGroup);
		if (device.deviceGroup != "olg") {
			return true;
		}

		const auto subscriberDeviceCount =
			DB_.Count(DB_.OP("subscriberId", ORM::EQ, device.subscriberId));
		if (subscriberDeviceCount > 1) {
			poco_warning(Logger(),
						 fmt::format("[SUBSCRIBER_DEVICE_DELETE]: Rejecting delete for OLG "
									 "device [{}] of subscriber [{}]. Device count is [{}]; "
									 "AP devices must be deleted first.",
									 device.serialNumber, device.subscriberId,
									 subscriberDeviceCount));
			BadRequest(RESTAPI::Errors::MustDeleteAPDevicesBeforeOLG);
			return false;
		}

		return true;
	}

	bool RESTAPI_sub_devices_handler::ValidateRequestFields(
		const SubscriberDeviceDB::RecordName &deviceObject) {
		if (!ValidDbId(deviceObject.managementPolicy, StorageService()->PolicyDB(), true,
					   RESTAPI::Errors::UnknownManagementPolicyUUID, *this) ||
			!ValidDbId(deviceObject.operatorId, StorageService()->OperatorDB(), false,
					   RESTAPI::Errors::InvalidOperatorId, *this) ||
			!ValidDbId(deviceObject.serviceClass, StorageService()->ServiceClassDB(), true,
					   RESTAPI::Errors::InvalidServiceClassId, *this) ||
			!ValidDbId(deviceObject.deviceConfiguration, StorageService()->ConfigurationDB(), true,
					   RESTAPI::Errors::ConfigurationMustExist, *this) ||
			!ValidSubscriberId(deviceObject.subscriberId, true, *this) ||
			!ValidDeviceRules(deviceObject.deviceRules, *this) ||
			!ValidSerialNumber(deviceObject.serialNumber, false, *this)) {
			return false;
		}
		return true;
	}

	bool RESTAPI_sub_devices_handler::UpdateConfiguration(
		const std::string &uuid, SubscriberDeviceDB::RecordName &existingObject,
		SubscriberDeviceDB::RecordName &updateObject, const Poco::JSON::Object::Ptr &rawObject,
		bool &hasUpdatedConfiguration) {
		hasUpdatedConfiguration = false;
		const auto beforeUpdate = existingObject;

		if (rawObject->has("configuration")) {
			auto effectiveConfigurationId = updateObject.deviceConfiguration;
			if (effectiveConfigurationId.empty()) {
				effectiveConfigurationId = existingObject.deviceConfiguration;
			}

			// Explicit empty "configuration" in PUT means clear deviceConfiguration.
			if (updateObject.configuration.empty()) {
				hasUpdatedConfiguration = !effectiveConfigurationId.empty();
				updateObject.deviceConfiguration.clear();
			} else {
				auto effectiveManagementPolicy =
					rawObject->has("managementPolicy") ? updateObject.managementPolicy
													   : existingObject.managementPolicy;
				auto effectiveDeviceType =
					rawObject->has("deviceType") ? updateObject.deviceType : existingObject.deviceType;
				auto effectiveDeviceGroup =
					rawObject->has("deviceGroup") ? updateObject.deviceGroup : existingObject.deviceGroup;
				Poco::toLowerInPlace(effectiveDeviceGroup);

				const auto desiredDeviceTypes =
					std::vector<std::string>{effectiveDeviceType.empty() ? std::string("*")
																		 : effectiveDeviceType};

				auto &configurationDB = StorageService()->ConfigurationDB();
				ProvObjects::DeviceConfiguration existingConfigurationRecord;
				const auto hasExistingConfigurationRecord =
					!effectiveConfigurationId.empty() &&
					configurationDB.GetRecord("id", effectiveConfigurationId, existingConfigurationRecord);

				if (hasExistingConfigurationRecord) {
					const auto sameConfiguration =
						RESTAPI_utils::to_string(existingConfigurationRecord.configuration) ==
						RESTAPI_utils::to_string(updateObject.configuration);
					const auto sameDeviceTypes =
						RESTAPI_utils::to_string(existingConfigurationRecord.deviceTypes) ==
						RESTAPI_utils::to_string(desiredDeviceTypes);
					const auto sameRecord =
						sameConfiguration &&
						existingConfigurationRecord.managementPolicy == effectiveManagementPolicy &&
						sameDeviceTypes;
					if (!sameRecord) {
						existingConfigurationRecord.configuration = updateObject.configuration;
						existingConfigurationRecord.managementPolicy = effectiveManagementPolicy;
						existingConfigurationRecord.deviceTypes = desiredDeviceTypes;

						std::vector<std::string> errors;
						if (!ValidateConfigBlock(ConfigurationValidator::GetType(effectiveDeviceGroup),
												 existingConfigurationRecord, errors)) {
							BadRequest(RESTAPI::Errors::ConfigBlockInvalid);
							return false;
						}

						if (!configurationDB.UpdateRecord("id", existingConfigurationRecord.info.id,
														  existingConfigurationRecord)) {
							InternalError(RESTAPI::Errors::RecordNotUpdated);
							return false;
						}
						hasUpdatedConfiguration = true;
					}
					updateObject.deviceConfiguration = existingConfigurationRecord.info.id;
					updateObject.configuration.clear();
				} else {
					if (!CreateConfigurationRecord(updateObject)) {
						return false;
					}
					hasUpdatedConfiguration = true;
				}
			}
		}

		auto projectedObject = existingObject;
		if (!ApplyPutRequest(updateObject, projectedObject, rawObject)) {
			return false;
		}

		// Ignore UpdateObjectInfo() timestamp-only differences for no-op PUT.
		projectedObject.info.modified = existingObject.info.modified;
		if (RESTAPI_utils::to_string(projectedObject) == RESTAPI_utils::to_string(existingObject)) {
			return true;
		}

		existingObject = projectedObject;
		if (!DB_.UpdateRecord("id", uuid, existingObject)) {
			InternalError(RESTAPI::Errors::RecordNotUpdated);
			return false;
		}
		if (rawObject->has("configuration") && updateObject.configuration.empty() &&
			!beforeUpdate.deviceConfiguration.empty() &&
			existingObject.deviceConfiguration.empty()) {
			CleanupSubscriberConfigurationRecord(beforeUpdate.deviceConfiguration, "");
		}

		const auto hasDeviceConfigurationReferenceChange =
			rawObject->has("deviceConfiguration") &&
			beforeUpdate.deviceConfiguration != existingObject.deviceConfiguration;
		if (hasDeviceConfigurationReferenceChange) {
			hasUpdatedConfiguration = true;
		}
		return true;
	}

	bool RESTAPI_sub_devices_handler::CreateConfigurationRecord(
		SubscriberDeviceDB::RecordName &newObject) {
		if (newObject.configuration.empty()) {
			return true;
		}

		auto targetDeviceGroup = newObject.deviceGroup;
		Poco::toLowerInPlace(targetDeviceGroup);

		ProvObjects::DeviceConfiguration subscriberConfiguration{};
		ProvObjects::CreateObjectInfo(UserInfo_.userinfo, subscriberConfiguration.info);
		subscriberConfiguration.info.name =
			fmt::format("subscriber-device:{}", newObject.serialNumber);
		subscriberConfiguration.configuration = newObject.configuration;
		subscriberConfiguration.managementPolicy = newObject.managementPolicy;
		subscriberConfiguration.deviceTypes = {newObject.deviceType.empty() ? std::string("*")
																			: newObject.deviceType};

		std::vector<std::string> errors;
		if (!ValidateConfigBlock(ConfigurationValidator::GetType(targetDeviceGroup),
								 subscriberConfiguration, errors)) {
			BadRequest(RESTAPI::Errors::ConfigBlockInvalid);
			return false;
		}

		if (!StorageService()->ConfigurationDB().CreateRecord(subscriberConfiguration)) {
			InternalError(RESTAPI::Errors::RecordNotCreated);
			return false;
		}

		newObject.deviceConfiguration = subscriberConfiguration.info.id;
		newObject.configuration.clear();
		return true;
	}

	bool RESTAPI_sub_devices_handler::ParsePutRequest(
		const std::string &uuid, SubscriberDeviceDB::RecordName &existingObject,
		SubscriberDeviceDB::RecordName &updateObject) {
		if (uuid.empty()) {
			BadRequest(RESTAPI::Errors::MissingUUID);
			return false;
		}

		const auto &rawObject = ParsedBody_;
		if (!updateObject.from_json(rawObject)) {
			BadRequest(RESTAPI::Errors::InvalidJSONDocument);
			return false;
		}

		if (!DB_.GetRecord("id", uuid, existingObject)) {
			NotFound();
			return false;
		}

		if (!ValidateRequestFields(updateObject)) {
			return false;
		}

		SubscriberDeviceDB::RecordName existingSerialNumber;
		if (DB_.GetRecord("serialNumber", updateObject.serialNumber, existingSerialNumber) &&
			existingSerialNumber.info.id != uuid) {
			BadRequest(RESTAPI::Errors::SerialNumberExists);
			return false;
		}

		if (!ValidateDeviceGroupForUpdate(existingObject, updateObject)) {
			return false;
		}
		return true;
	}

	bool RESTAPI_sub_devices_handler::ParsePostRequest(SubscriberDeviceDB::RecordName &newObject) {
		const auto &rawObject = ParsedBody_;
		if (!newObject.from_json(rawObject)) {
			BadRequest(RESTAPI::Errors::InvalidJSONDocument);
			return false;
		}

		if (DB_.Exists("serialNumber", newObject.serialNumber)) {
			BadRequest(RESTAPI::Errors::SerialNumberExists);
			return false;
		}

		if (!ValidateRequestFields(newObject)) {
			return false;
		}

		return true;
	}

	bool
	RESTAPI_sub_devices_handler::ValidateDeviceGroup(SubscriberDeviceDB::RecordName &newObject) {
		if (!ParsedBody_->has("deviceGroup")) {
			poco_warning(Logger(),
						 fmt::format("[SUBSCRIBER_DEVICE_CREATE]: Rejecting serial [{}] for "
									 "subscriber [{}]. deviceGroup is required.",
									 newObject.serialNumber, newObject.subscriberId));
			BadRequest(RESTAPI::Errors::InvalidSubscriberDeviceGroup);
			return false;
		}

		Poco::toLowerInPlace(newObject.deviceGroup);

		if (newObject.deviceGroup != "olg" && newObject.deviceGroup != "ap") {
			poco_warning(Logger(),
						 fmt::format("[SUBSCRIBER_DEVICE_CREATE]: Rejecting serial [{}] for "
									 "subscriber [{}]. Invalid deviceGroup [{}].",
									 newObject.serialNumber, newObject.subscriberId,
									 newObject.deviceGroup));
			BadRequest(RESTAPI::Errors::InvalidSubscriberDeviceGroup);
			return false;
		}

		const auto subscriberDeviceCount =
			DB_.Count(DB_.OP("subscriberId", ORM::EQ, newObject.subscriberId));

		if (subscriberDeviceCount == 0) {
			if (newObject.deviceGroup != "olg") {
				poco_warning(Logger(),
							 fmt::format("[SUBSCRIBER_DEVICE_CREATE]: Rejecting serial [{}] for "
										 "subscriber [{}]. First device must be olg, got [{}].",
										 newObject.serialNumber, newObject.subscriberId,
										 newObject.deviceGroup));
				BadRequest(RESTAPI::Errors::FirstSubscriberDeviceMustBeOLG);
				return false;
			}
		} else if (newObject.deviceGroup == "olg") {
			poco_warning(Logger(),
						 fmt::format("[SUBSCRIBER_DEVICE_CREATE]: Rejecting serial [{}] for "
									 "subscriber [{}]. Existing device count [{}]; "
									 "only first device can be olg.",
									 newObject.serialNumber, newObject.subscriberId,
									 subscriberDeviceCount));
			BadRequest(RESTAPI::Errors::OnlyFirstSubscriberDeviceCanBeOLG);
			return false;
		}

		return true;
	}

	bool RESTAPI_sub_devices_handler::ValidateDeviceGroupForUpdate(
		SubscriberDeviceDB::RecordName &existingObject,
		const SubscriberDeviceDB::RecordName &updateObject) {
		auto targetGroup = updateObject.deviceGroup;
		Poco::toLowerInPlace(targetGroup);
		if (targetGroup.empty()) {
			poco_warning(Logger(),
						 fmt::format("[SUBSCRIBER_DEVICE_UPDATE]: Rejecting serial [{}]. "
									 "deviceGroup is required.",
									 existingObject.serialNumber));
			BadRequest(RESTAPI::Errors::InvalidSubscriberDeviceGroup);
			return false;
		}

		if (targetGroup != "olg" && targetGroup != "ap") {
			poco_warning(Logger(), fmt::format("[SUBSCRIBER_DEVICE_UPDATE]: Rejecting serial [{}]. "
											   "Invalid deviceGroup [{}].",
											   existingObject.serialNumber, targetGroup));
			BadRequest(RESTAPI::Errors::InvalidSubscriberDeviceGroup);
			return false;
		}

		if (targetGroup != "olg") {
			return true;
		}

		const auto subscriberDeviceCount =
			DB_.Count(DB_.OP("subscriberId", ORM::EQ, existingObject.subscriberId));
		if (subscriberDeviceCount > 1) {
			poco_warning(Logger(), fmt::format("[SUBSCRIBER_DEVICE_UPDATE]: Rejecting serial [{}]. "
											   "Only first device can be olg. subscriber [{}], "
											   "device count [{}].",
											   existingObject.serialNumber, existingObject.subscriberId,
											   subscriberDeviceCount));
			BadRequest(RESTAPI::Errors::OnlyFirstSubscriberDeviceCanBeOLG);
			return false;
		}

		return true;
	}

	bool
	RESTAPI_sub_devices_handler::ApplyPutRequest(const SubscriberDeviceDB::RecordName &updateObject,
												 SubscriberDeviceDB::RecordName &existingObject,
												 const Poco::JSON::Object::Ptr &rawObject) {
		if (!ProvObjects::UpdateObjectInfo(rawObject, UserInfo_.userinfo, existingObject.info)) {
			BadRequest(RESTAPI::Errors::NameMustBeSet);
			return false;
		}
		AssignIfPresent(rawObject, "deviceType", existingObject.deviceType);
		AssignIfPresent(rawObject, "subscriberId", existingObject.subscriberId);
		AssignIfPresent(rawObject, "managementPolicy", existingObject.managementPolicy);
		AssignIfPresent(rawObject, "serviceClass", existingObject.serviceClass);
		AssignIfPresent(rawObject, "qrCode", existingObject.qrCode);
		AssignIfPresent(rawObject, "geoCode", existingObject.geoCode);
		if (rawObject->has("deviceRules")) {
			existingObject.deviceRules = updateObject.deviceRules;
		}
		AssignIfPresent(rawObject, "state", existingObject.state);
		AssignIfPresent(rawObject, "locale", existingObject.locale);
		AssignIfPresent(rawObject, "billingCode", existingObject.billingCode);
		AssignIfPresent(rawObject, "realMacAddress", existingObject.realMacAddress);
		AssignIfPresent(rawObject, "contact", updateObject.contact, existingObject.contact);
		AssignIfPresent(rawObject, "location", updateObject.location, existingObject.location);
		if (rawObject->has("deviceGroup")) {
			existingObject.deviceGroup = updateObject.deviceGroup;
		}
		if (rawObject->has("deviceConfiguration") || rawObject->has("configuration")) {
			existingObject.deviceConfiguration = updateObject.deviceConfiguration;
		}
		return true;
	}

	bool RESTAPI_sub_devices_handler::CreateSubscriberDeviceRecord(
		const SubscriberDeviceDB::RecordName &newObject) {
		if (!DB_.CreateRecord(newObject)) {
			InternalError(RESTAPI::Errors::RecordNotCreated);
			return false;
		}
		return true;
	}

	bool RESTAPI_sub_devices_handler::CreateInventoryRecord(
		const SubscriberDeviceDB::RecordName &newObject) {
		poco_information(Logger(),
						 fmt::format("[SUBSCRIBER_DEVICE_CREATE][CreateInventoryRecord]: start "
									 "serial=[{}], subscriber=[{}], deviceConfiguration=[{}].",
									 newObject.serialNumber, newObject.subscriberId,
									 newObject.deviceConfiguration));
		auto &inventoryDB = StorageService()->InventoryDB();
		ProvObjects::InventoryTag inventoryRecord;
		ProvObjects::Venue venueRecord;
		std::string resolvedVenueId;
		if (StorageService()->VenueDB().GetRecord("subscriber", newObject.subscriberId,
												  venueRecord)) {
			resolvedVenueId = venueRecord.info.id;
			poco_information(Logger(),
							 fmt::format("[SUBSCRIBER_DEVICE_CREATE][CreateInventoryRecord]: "
										 "resolved venue=[{}] for subscriber=[{}].",
										 resolvedVenueId, newObject.subscriberId));
		} else {
			poco_information(Logger(),
							 fmt::format("[SUBSCRIBER_DEVICE_CREATE][CreateInventoryRecord]: "
										 "no venue found for subscriber=[{}]. venue stays "
										 "as-is/empty.",
										 newObject.subscriberId));
		}

		if (inventoryDB.GetRecord("serialNumber", newObject.serialNumber, inventoryRecord)) {
			const auto fromPolicy = inventoryRecord.managementPolicy;
			const auto fromConfiguration = inventoryRecord.deviceConfiguration;
			const auto fromVenue = inventoryRecord.venue;

			inventoryRecord.subscriber = newObject.subscriberId;
			inventoryRecord.deviceType = newObject.deviceType;
			inventoryRecord.managementPolicy = newObject.managementPolicy;
			inventoryRecord.deviceConfiguration = newObject.deviceConfiguration;
			inventoryRecord.deviceRules = newObject.deviceRules;
			inventoryRecord.state = newObject.state;
			inventoryRecord.locale = newObject.locale;
			inventoryRecord.realMacAddress = newObject.realMacAddress;
			if (!resolvedVenueId.empty()) {
				inventoryRecord.venue = resolvedVenueId;
			}
			inventoryRecord.info.modified = Utils::Now();

			if (!inventoryDB.UpdateRecord("id", inventoryRecord.info.id, inventoryRecord)) {
				poco_warning(Logger(),
							 fmt::format("[SUBSCRIBER_DEVICE_CREATE][CreateInventoryRecord]: "
										 "failed to update existing inventory id=[{}] for "
										 "serial=[{}].",
										 inventoryRecord.info.id, newObject.serialNumber));
				return false;
			}

			SerialNumberCache()->AddSerialNumber(newObject.serialNumber, newObject.deviceType);
			MoveUsage(StorageService()->PolicyDB(), inventoryDB, fromPolicy,
					  inventoryRecord.managementPolicy, inventoryRecord.info.id);
			MoveUsage(StorageService()->ConfigurationDB(), inventoryDB, fromConfiguration,
					  inventoryRecord.deviceConfiguration, inventoryRecord.info.id);
			ManageMembership(StorageService()->VenueDB(), &ProvObjects::Venue::devices, fromVenue,
							 inventoryRecord.venue, inventoryRecord.info.id);

			poco_information(Logger(),
							 fmt::format("[SUBSCRIBER_DEVICE_CREATE][CreateInventoryRecord]: "
										 "updated existing inventory id=[{}], venue=[{}], "
										 "deviceConfiguration=[{}], subscriber=[{}].",
										 inventoryRecord.info.id, inventoryRecord.venue,
										 inventoryRecord.deviceConfiguration,
										 inventoryRecord.subscriber));
			return true;
		}

		ProvObjects::CreateObjectInfo(UserInfo_.userinfo, inventoryRecord.info);
		inventoryRecord.info.name = newObject.serialNumber;
		inventoryRecord.serialNumber = newObject.serialNumber;
		inventoryRecord.subscriber = newObject.subscriberId;
		inventoryRecord.deviceType = newObject.deviceType;
		inventoryRecord.managementPolicy = newObject.managementPolicy;
		inventoryRecord.deviceConfiguration = newObject.deviceConfiguration;
		inventoryRecord.deviceRules = newObject.deviceRules;
		inventoryRecord.state = newObject.state;
		inventoryRecord.locale = newObject.locale;
		inventoryRecord.realMacAddress = newObject.realMacAddress;
		inventoryRecord.devClass = "any";
		inventoryRecord.platform = "AP";
		inventoryRecord.venue = resolvedVenueId;

		if (!inventoryDB.CreateRecord(inventoryRecord)) {
			// The record may have been created by auto-discovery concurrently.
			if (inventoryDB.GetRecord("serialNumber", newObject.serialNumber, inventoryRecord)) {
				poco_information(Logger(),
								 fmt::format("[SUBSCRIBER_DEVICE_CREATE][CreateInventoryRecord]: "
											 "create raced; inventory now exists for serial=[{}], "
											 "id=[{}].",
											 newObject.serialNumber, inventoryRecord.info.id));
				return true;
			}
			poco_warning(Logger(), fmt::format("[SUBSCRIBER_DEVICE_CREATE]: Failed to create "
											   "inventory record for serial [{}].",
											   newObject.serialNumber));
			return false;
		}

		poco_information(Logger(), fmt::format("[SUBSCRIBER_DEVICE_CREATE][CreateInventoryRecord]: "
											   "created inventory id=[{}], venue=[{}], "
											   "deviceConfiguration=[{}].",
											   inventoryRecord.info.id, inventoryRecord.venue,
											   inventoryRecord.deviceConfiguration));

		SerialNumberCache()->AddSerialNumber(newObject.serialNumber, newObject.deviceType);
		MoveUsage(StorageService()->PolicyDB(), inventoryDB, "", inventoryRecord.managementPolicy,
				  inventoryRecord.info.id);
		MoveUsage(StorageService()->ConfigurationDB(), inventoryDB, "",
				  inventoryRecord.deviceConfiguration, inventoryRecord.info.id);
		ManageMembership(StorageService()->VenueDB(), &ProvObjects::Venue::devices, "",
						 inventoryRecord.venue, inventoryRecord.info.id);
		poco_information(Logger(), fmt::format("[SUBSCRIBER_DEVICE_CREATE][CreateInventoryRecord]: "
											   "completed serial=[{}], inventoryId=[{}].",
											   newObject.serialNumber, inventoryRecord.info.id));
		return true;
	}

	void RESTAPI_sub_devices_handler::ReturnSubscriberDeviceObject(
		const ProvObjects::SubscriberDevice &device) {
		Poco::JSON::Object answer;
		device.to_json(answer);
		ReturnObject(answer);
	}

	void RESTAPI_sub_devices_handler::CleanupInventoryAssociations(
		const ProvObjects::InventoryTag &inventoryRecord) {
		MoveUsage(StorageService()->PolicyDB(), StorageService()->InventoryDB(),
				  inventoryRecord.managementPolicy, "", inventoryRecord.info.id);
		MoveUsage(StorageService()->LocationDB(), StorageService()->InventoryDB(),
				  inventoryRecord.location, "", inventoryRecord.info.id);
		MoveUsage(StorageService()->ContactDB(), StorageService()->InventoryDB(),
				  inventoryRecord.contact, "", inventoryRecord.info.id);
		ManageMembership(StorageService()->EntityDB(), &ProvObjects::Entity::devices,
						 inventoryRecord.entity, "", inventoryRecord.info.id);
		ManageMembership(StorageService()->VenueDB(), &ProvObjects::Venue::devices,
						 inventoryRecord.venue, "", inventoryRecord.info.id);
	}

	void RESTAPI_sub_devices_handler::DeleteInventoryForSubscriberDevice(
		const ProvObjects::SubscriberDevice &existingObject) {
		ProvObjects::InventoryTag inventoryRecord;
		if (!StorageService()->InventoryDB().GetRecord("serialNumber", existingObject.serialNumber,
													   inventoryRecord)) {
			CleanupSubscriberConfigurationRecord(existingObject.deviceConfiguration, "");
			return;
		}

		std::set<std::string> configurationsToCleanup;
		if (!existingObject.deviceConfiguration.empty()) {
			configurationsToCleanup.insert(existingObject.deviceConfiguration);
		}
		if (!inventoryRecord.deviceConfiguration.empty()) {
			configurationsToCleanup.insert(inventoryRecord.deviceConfiguration);
		}

		CleanupInventoryAssociations(inventoryRecord);
		for (const auto &deviceConfiguration : configurationsToCleanup) {
			CleanupSubscriberConfigurationRecord(deviceConfiguration, inventoryRecord.info.id);
		}
		if (!StorageService()->InventoryDB().DeleteRecord("id", inventoryRecord.info.id)) {
			poco_warning(Logger(),
						 fmt::format("[SUBSCRIBER_DEVICE_DELETE]: Failed to delete "
									 "inventory [{}] for serial [{}].",
									 inventoryRecord.info.id, existingObject.serialNumber));
			return;
		}
		SerialNumberCache()->DeleteSerialNumber(existingObject.serialNumber);
	}

	bool
	RESTAPI_sub_devices_handler::StartMonitoring(const ProvObjects::SubscriberDevice &newObject) {
		std::string newObjectDeviceGroup = newObject.deviceGroup;
		Poco::toLowerInPlace(newObjectDeviceGroup);
		if (newObjectDeviceGroup != "olg") {
			return true;
		}

		std::string venueId;
		std::string venueName;
		ProvObjects::Venue venueRecord;
		if (StorageService()->VenueDB().GetRecord("subscriber", newObject.subscriberId,
												  venueRecord)) {
			venueId = venueRecord.info.id;
			venueName = venueRecord.info.name;
		}
		if (venueId.empty()) {
			return true;
		}

		auto &venueDB = StorageService()->VenueDB();
		if (!venueRecord.boards.empty()) {
			return true;
		}

		Poco::JSON::Object boardBody;
		boardBody.set("name", venueRecord.info.name.empty() ? venueName : venueRecord.info.name);

		Poco::JSON::Array venueList;
		Poco::JSON::Object venueEntry;
		venueEntry.set("id", venueRecord.info.id);
		venueEntry.set("name", venueRecord.info.name.empty() ? venueName : venueRecord.info.name);
		venueEntry.set("retention", static_cast<uint64_t>(604800));
		venueEntry.set("interval", static_cast<uint64_t>(60));
		venueEntry.set("monitorSubVenues", true);
		venueList.add(venueEntry);
		boardBody.set("venueList", venueList);

		Poco::JSON::Object::Ptr boardResponse;
		Poco::Net::HTTPServerResponse::HTTPStatus boardStatus =
			Poco::Net::HTTPServerResponse::HTTP_INTERNAL_SERVER_ERROR;
		if (!SDK::Analytics::StartMonitoring(boardBody, boardResponse, boardStatus)) {
			poco_error(Logger(),
					   fmt::format("[SUBSCRIBER_PROVISIONING]: Failed to start monitoring "
								   "for venue [{}], status [{}].",
								   venueId, static_cast<int>(boardStatus)));
			return false;
		}

		if (!boardResponse || !boardResponse->has("id")) {
			poco_error(Logger(), fmt::format("[SUBSCRIBER_PROVISIONING]: Monitoring started for "
											 "venue [{}] but no board id returned.",
											 venueId));
			return false;
		}

		auto boardId = boardResponse->get("id").toString();
		if (boardId.empty()) {
			poco_error(Logger(), fmt::format("[SUBSCRIBER_PROVISIONING]: Monitoring started for "
											 "venue [{}] but board id is empty.",
											 venueId));
			return false;
		}

		if (!venueDB.GetRecord("id", venueId, venueRecord)) {
			poco_error(Logger(),
					   fmt::format("[SUBSCRIBER_PROVISIONING]: Venue [{}] not found after "
								   "monitoring start.",
								   venueId));
			return false;
		}
		if (std::find(venueRecord.boards.begin(), venueRecord.boards.end(), boardId) !=
			venueRecord.boards.end()) {
			return true;
		}

		venueRecord.boards.push_back(boardId);
		venueRecord.info.modified = Utils::Now();
		if (!venueDB.UpdateRecord("id", venueRecord.info.id, venueRecord)) {
			poco_error(Logger(), fmt::format("[SUBSCRIBER_PROVISIONING]: Failed to persist board "
											 "[{}] for venue [{}].",
											 boardId, venueId));
			return false;
		}

		return true;
	}

	bool RESTAPI_sub_devices_handler::StopMonitoring(
		const ProvObjects::SubscriberDevice &existingObject) {
		std::string existingObjectDeviceGroup = existingObject.deviceGroup;
		Poco::toLowerInPlace(existingObjectDeviceGroup);
		if (existingObjectDeviceGroup != "olg") {
			return true;
		}

		ProvObjects::Venue venueRecord;
		if (!StorageService()->VenueDB().GetRecord("subscriber", existingObject.subscriberId,
												   venueRecord)) {
			return true;
		}
		if (venueRecord.info.id.empty() || venueRecord.boards.empty()) {
			return true;
		}

		for (const auto &boardId : venueRecord.boards) {
			if (boardId.empty()) {
				continue;
			}

			Poco::Net::HTTPServerResponse::HTTPStatus boardStatus =
				Poco::Net::HTTPServerResponse::HTTP_INTERNAL_SERVER_ERROR;
			if (!SDK::Analytics::StopMonitoring(boardId, boardStatus)) {
				poco_error(Logger(),
						   fmt::format("[SUBSCRIBER_PROVISIONING]: Failed to stop "
									   "monitoring for board [{}], venue [{}], status [{}].",
									   boardId, venueRecord.info.id,
									   static_cast<int>(boardStatus)));
				return false;
			}
		}

		venueRecord.boards.clear();
		venueRecord.info.modified = Utils::Now();
		if (!StorageService()->VenueDB().UpdateRecord("id", venueRecord.info.id, venueRecord)) {
			poco_error(Logger(), fmt::format("[SUBSCRIBER_PROVISIONING]: Failed to clear boards "
											 "for venue [{}] after stop monitoring.",
											 venueRecord.info.id));
			return false;
		}
		return true;
	}

	void RESTAPI_sub_devices_handler::DoGet() {
		ProvObjects::SubscriberDevice existingObject;
		if (!LoadDeviceFromBinding(existingObject, true)) {
			return;
		}
		return ReturnSubscriberDeviceObject(existingObject);
	}

	void RESTAPI_sub_devices_handler::DoDelete() {
		auto deviceIdOrSerial = GetBinding("uuid", "");
		poco_debug(Logger(), fmt::format("[SUBSCRIBER_DEVICE_DELETE]: Received delete request for "
								 "[{}].",
								 deviceIdOrSerial));

		ProvObjects::SubscriberDevice existingObject;
		if (!ValidateDeleteRequest(deviceIdOrSerial, existingObject)) {
			return;
		}
		if (!StopMonitoring(existingObject)) {
			return;
		}

		if (!DB_.DeleteRecord("id", existingObject.info.id)) {
			BadRequest(RESTAPI::Errors::NoRecordsDeleted);
			return;
		}
		DeleteInventoryForSubscriberDevice(existingObject);

		if (!SDK::GW::Device::Delete(this, existingObject.serialNumber)) {
			poco_warning(Logger(), fmt::format("[SUBSCRIBER_DEVICE_DELETE]: Failed to delete serial "
											 "[{}] from owgw.",
											 existingObject.serialNumber));
		}
		return OK();
	}

	void RESTAPI_sub_devices_handler::DoPost() {
		SubscriberDeviceDB::RecordName newObject = {};
		if (!ParsePostRequest(newObject)) {
			return;
		}

		if (!ValidateDeviceGroup(newObject)) {
			return;
		}

		poco_information(Logger(),
						 fmt::format("[SUBSCRIBER_DEVICE_CREATE]: subscriber [{}], serial [{}], "
									 "deviceGroup [{}].",
									 newObject.subscriberId, newObject.serialNumber,
									 newObject.deviceGroup));

		if (!CreateConfigurationRecord(newObject)) {
			return;
		}

		ProvObjects::CreateObjectInfo(ParsedBody_, UserInfo_.userinfo, newObject.info);
		if (!CreateSubscriberDeviceRecord(newObject)) {
			return;
		}

		if (!CreateInventoryRecord(newObject)) {
			if (!DB_.DeleteRecord("id", newObject.info.id)) {
				poco_warning(Logger(), fmt::format("[SUBSCRIBER_DEVICE_CREATE]: Failed to rollback "
												   "subscriber-device [{}] after inventory create "
												   "failure.",
												   newObject.info.id));
			}
			return InternalError(RESTAPI::Errors::RecordNotCreated);
		}

		if (!StartMonitoring(newObject)) {
			poco_warning(Logger(),
						 fmt::format("[SUBSCRIBER_DEVICE_CREATE]: Failed to start monitoring "
									 "for subscriber [{}], serial [{}].",
									 newObject.subscriberId, newObject.serialNumber));
		}

		const auto shouldPushConfiguration =
			ParsedBody_->has("configuration") && !newObject.deviceConfiguration.empty();
		if (shouldPushConfiguration && !PushConfigurationToDevice(newObject.serialNumber)) {
			poco_warning(Logger(), fmt::format("[SUBSCRIBER_DEVICE_CREATE]: Configuration push "
											   "did not converge for serial [{}]. Returning "
											   "created record and relying on subsequent sync.",
											   newObject.serialNumber));
		}
		return ReturnSubscriberDeviceObject(newObject);
	}

	void RESTAPI_sub_devices_handler::DoPut() {
		auto uuid = GetBinding("uuid");
		SubscriberDeviceDB::RecordName existingObject;
		SubscriberDeviceDB::RecordName updateObject;
		bool hasUpdatedConfiguration = false;
		if (!ParsePutRequest(uuid, existingObject, updateObject)) {
			return;
		}

		const auto &rawObject = ParsedBody_;
		if (!UpdateConfiguration(uuid, existingObject, updateObject, rawObject,
								 hasUpdatedConfiguration)) {
			return;
		}

		const auto shouldSyncInventory =
			hasUpdatedConfiguration || rawObject->has("subscriberId") ||
			rawObject->has("deviceType") || rawObject->has("managementPolicy") ||
			rawObject->has("deviceRules") || rawObject->has("state") ||
			rawObject->has("locale") || rawObject->has("realMacAddress");
		if (shouldSyncInventory && !CreateInventoryRecord(existingObject)) {
			return InternalError(RESTAPI::Errors::RecordNotUpdated);
		}

		if (hasUpdatedConfiguration) {
			(void)PushConfigurationToDevice(existingObject.serialNumber);
		}

		return ReturnSubscriberDeviceObject(existingObject);
	}

} // namespace OpenWifi
