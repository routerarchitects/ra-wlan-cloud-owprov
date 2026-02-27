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
#include "Poco/String.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "SerialNumberCache.h"
#include "framework/RESTAPI_utils.h"
#include "framework/SubscriberProvisioning.h"
#include "framework/utils.h"
#include "sdks/SDK_gw.h"
#include "sdks/SDK_sec.h"
#include <set>

namespace OpenWifi {
	namespace {
		void CleanupSubscriberConfigurationRecord(const std::string &configurationId,
												 const std::string &inventoryId,
												 Poco::Logger &logger) {
			if (configurationId.empty()) {
				return;
			}

			auto &configurationDB = StorageService()->ConfigurationDB();
			ProvObjects::DeviceConfiguration configurationRecord;
			if (!configurationDB.GetRecord("id", configurationId, configurationRecord)) {
				return;
			}

			if (!inventoryId.empty()) {
				configurationDB.DeleteInUse("id", configurationId,
											StorageService()->InventoryDB().Prefix(), inventoryId);
			}

			if (!configurationDB.GetRecord("id", configurationId, configurationRecord)) {
				return;
			}

			const auto looksLikeSubscriberModalConfiguration =
				configurationRecord.info.name.rfind("subscriber-device:", 0) == 0;
			if ((configurationRecord.subscriberOnly || looksLikeSubscriberModalConfiguration) &&
				configurationRecord.inUse.empty() &&
				!configurationDB.DeleteRecord("id", configurationId)) {
				poco_warning(logger,
							 fmt::format("[SUBSCRIBER_DEVICE_DELETE]: Failed to delete "
										 "subscriber configuration [{}].",
										 configurationId));
			}
		}

		bool EnsureSubscriberConfigurationOwnership(const std::string &serialNumber,
												   const std::string &subscriberId,
												   const std::string &deviceType,
												   const std::string &managementPolicy,
												   std::string &configurationId,
												   const SecurityObjects::UserInfo &actor,
												   Poco::Logger &logger) {
			if (configurationId.empty()) {
				return true;
			}

			auto &configurationDB = StorageService()->ConfigurationDB();
			ProvObjects::DeviceConfiguration configurationRecord;
			if (!configurationDB.GetRecord("id", configurationId, configurationRecord)) {
				return false;
			}

			const auto requiresSubscriberScopedCopy =
				!configurationRecord.subscriberOnly || configurationRecord.subscriber != subscriberId ||
				!configurationRecord.entity.empty() || !configurationRecord.venue.empty();

			if (requiresSubscriberScopedCopy) {
				const auto looksLikeSubscriberModalConfiguration =
					configurationRecord.info.name.rfind("subscriber-device:", 0) == 0;
				const auto canNormalizeInPlace =
					configurationRecord.inUse.empty() &&
					(looksLikeSubscriberModalConfiguration || configurationRecord.subscriber.empty());

				if (canNormalizeInPlace) {
					configurationRecord.subscriberOnly = true;
					configurationRecord.subscriber = subscriberId;
					configurationRecord.entity.clear();
					configurationRecord.venue.clear();
					configurationRecord.managementPolicy = managementPolicy;
					configurationRecord.deviceTypes = {
						deviceType.empty() ? std::string("*") : deviceType};
					configurationRecord.info.modified = Utils::Now();
					if (!configurationDB.UpdateRecord("id", configurationRecord.info.id,
													  configurationRecord)) {
						poco_warning(
							logger,
							fmt::format("[SUBSCRIBER_DEVICE_CONFIG]: Failed to normalize "
										"configuration [{}] in place for serial [{}].",
										configurationId, serialNumber));
						return false;
					}
					return true;
				}

				ProvObjects::DeviceConfiguration subscriberConfiguration{configurationRecord};
				ProvObjects::CreateObjectInfo(actor, subscriberConfiguration.info);
				subscriberConfiguration.info.name = fmt::format("subscriber-device:{}", serialNumber);
				subscriberConfiguration.inUse.clear();
				subscriberConfiguration.subscriberOnly = true;
				subscriberConfiguration.subscriber = subscriberId;
				subscriberConfiguration.entity.clear();
				subscriberConfiguration.venue.clear();
				subscriberConfiguration.managementPolicy = managementPolicy;
				subscriberConfiguration.deviceTypes = {
					deviceType.empty() ? std::string("*") : deviceType};

				if (!configurationDB.CreateRecord(subscriberConfiguration)) {
					poco_warning(
						logger,
						fmt::format("[SUBSCRIBER_DEVICE_CONFIG]: Failed to clone configuration [{}] "
									"for serial [{}].",
									configurationId, serialNumber));
					return false;
				}

				configurationId = subscriberConfiguration.info.id;
				return true;
			}

			auto updated = false;
			if (!configurationRecord.subscriberOnly) {
				configurationRecord.subscriberOnly = true;
				updated = true;
			}
			if (configurationRecord.subscriber != subscriberId) {
				configurationRecord.subscriber = subscriberId;
				updated = true;
			}
			if (!configurationRecord.entity.empty()) {
				configurationRecord.entity.clear();
				updated = true;
			}
			if (!configurationRecord.venue.empty()) {
				configurationRecord.venue.clear();
				updated = true;
			}
			if (configurationRecord.managementPolicy != managementPolicy) {
				configurationRecord.managementPolicy = managementPolicy;
				updated = true;
			}
			const Types::StringVec expectedDeviceTypes{
				deviceType.empty() ? std::string("*") : deviceType};
			if (configurationRecord.deviceTypes != expectedDeviceTypes) {
				configurationRecord.deviceTypes = expectedDeviceTypes;
				updated = true;
			}
			if (updated) {
				configurationRecord.info.modified = Utils::Now();
				if (!configurationDB.UpdateRecord("id", configurationRecord.info.id,
												  configurationRecord)) {
					poco_warning(
						logger,
						fmt::format("[SUBSCRIBER_DEVICE_CONFIG]: Failed to normalize configuration "
									"[{}] for serial [{}].",
									configurationId, serialNumber));
					return false;
				}
			}
			return true;
		}
	} // namespace

	bool RESTAPI_sub_devices_handler::PushConfigurationToDevice(
		const std::string &SerialNumber) {
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

	bool RESTAPI_sub_devices_handler::PushConfigurationIfInInventory(
		const std::string &SerialNumber) {
		ProvObjects::InventoryTag inventoryRecord;
		if (!StorageService()->InventoryDB().GetRecord("serialNumber", SerialNumber,
													   inventoryRecord)) {
			poco_debug(Logger(), fmt::format("[SUBSCRIBER_DEVICE_CONFIG]: Inventory not found for "
											 "serial [{}], skipping configuration push.",
											 SerialNumber));
			return true;
		}
		return PushConfigurationToDevice(SerialNumber);
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

	bool RESTAPI_sub_devices_handler::ValidateDeleteRequest(
		const std::string &deviceIdOrSerial, ProvObjects::SubscriberDevice &device) {
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

	bool RESTAPI_sub_devices_handler::ParseAndValidatePutRequest(
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

		if (!ValidDbId(updateObject.managementPolicy, StorageService()->PolicyDB(), true,
					   RESTAPI::Errors::UnknownManagementPolicyUUID, *this) ||
			!ValidDbId(updateObject.operatorId, StorageService()->OperatorDB(), true,
					   RESTAPI::Errors::InvalidOperatorId, *this) ||
			!ValidDbId(updateObject.serviceClass, StorageService()->ServiceClassDB(), true,
					   RESTAPI::Errors::InvalidServiceClassId, *this) ||
			!ValidDbId(updateObject.configurationId, StorageService()->ConfigurationDB(), true,
					   RESTAPI::Errors::ConfigurationMustExist, *this) ||
			!ValidSubscriberId(updateObject.subscriberId, true, *this) ||
			(rawObject->has("deviceRules") && !ValidDeviceRules(updateObject.deviceRules, *this)) ||
			!ValidSerialNumber(updateObject.serialNumber, false, *this)) {
			return false;
		}

		SubscriberDeviceDB::RecordName existingSerialNumber;
		if (DB_.GetRecord("serialNumber", updateObject.serialNumber, existingSerialNumber) &&
			existingSerialNumber.info.id != uuid) {
			BadRequest(RESTAPI::Errors::SerialNumberExists);
			return false;
		}

		if (!ValidateAndNormalizeDeviceGroupForUpdate(existingObject, updateObject, rawObject)) {
			return false;
		}

			if (rawObject->has("configuration")) {
				ProvObjects::DeviceConfiguration subscriberConfiguration;
				auto targetConfigurationId = updateObject.configurationId;
				const auto targetSubscriberId =
					rawObject->has("subscriberId") ? updateObject.subscriberId
												   : existingObject.subscriberId;
				if (targetConfigurationId.empty()) {
					targetConfigurationId = existingObject.configurationId;
				}

				bool updateExistingConfiguration = false;
				if (!targetConfigurationId.empty() &&
					StorageService()->ConfigurationDB().GetRecord("id", targetConfigurationId,
																  subscriberConfiguration) &&
					subscriberConfiguration.subscriberOnly &&
					subscriberConfiguration.subscriber == targetSubscriberId &&
					subscriberConfiguration.entity.empty() &&
					subscriberConfiguration.venue.empty()) {
					updateExistingConfiguration = true;
				} else {
					subscriberConfiguration = {};
					ProvObjects::CreateObjectInfo(UserInfo_.userinfo, subscriberConfiguration.info);
					subscriberConfiguration.info.name =
						fmt::format("subscriber-device:{}", existingObject.serialNumber);
				}

			subscriberConfiguration.configuration = updateObject.configuration;
			subscriberConfiguration.managementPolicy =
				rawObject->has("managementPolicy") ? updateObject.managementPolicy
												   : existingObject.managementPolicy;
			const auto &targetDeviceType =
				rawObject->has("deviceType") ? updateObject.deviceType : existingObject.deviceType;
				subscriberConfiguration.deviceTypes = {
					targetDeviceType.empty() ? std::string("*") : targetDeviceType};
				subscriberConfiguration.subscriberOnly = true;
				subscriberConfiguration.subscriber = targetSubscriberId;
				subscriberConfiguration.entity.clear();
				subscriberConfiguration.venue.clear();

			std::vector<std::string> errors;
			const auto targetDeviceGroup =
				rawObject->has("deviceGroup") ? updateObject.deviceGroup : existingObject.deviceGroup;
			if (!ValidateConfigBlock(ConfigurationValidator::GetType(targetDeviceGroup),
									 subscriberConfiguration, errors)) {
				BadRequest(RESTAPI::Errors::ConfigBlockInvalid);
				return false;
			}

			if (updateExistingConfiguration) {
				if (!StorageService()->ConfigurationDB().UpdateRecord("id",
																	  subscriberConfiguration.info.id,
																	  subscriberConfiguration)) {
					InternalError(RESTAPI::Errors::RecordNotUpdated);
					return false;
				}
			} else {
				if (!StorageService()->ConfigurationDB().CreateRecord(subscriberConfiguration)) {
					InternalError(RESTAPI::Errors::RecordNotCreated);
					return false;
				}
			}
			updateObject.configurationId = subscriberConfiguration.info.id;
			updateObject.configuration.clear();
		}
		if (!EnsureSubscriberConfigurationOwnership(
				existingObject.serialNumber,
				rawObject->has("subscriberId") ? updateObject.subscriberId : existingObject.subscriberId,
				rawObject->has("deviceType") ? updateObject.deviceType : existingObject.deviceType,
				rawObject->has("managementPolicy") ? updateObject.managementPolicy
												   : existingObject.managementPolicy,
				updateObject.configurationId, UserInfo_.userinfo, Logger())) {
			InternalError(RESTAPI::Errors::RecordNotUpdated);
			return false;
		}

		return true;
	}

	bool RESTAPI_sub_devices_handler::ParseAndValidatePostRequest(
		SubscriberDeviceDB::RecordName &newObject) {
		const auto &rawObject = ParsedBody_;
		if (!newObject.from_json(rawObject)) {
			BadRequest(RESTAPI::Errors::InvalidJSONDocument);
			return false;
		}

		if (!ValidDbId(newObject.managementPolicy, StorageService()->PolicyDB(), true,
					   RESTAPI::Errors::UnknownManagementPolicyUUID, *this) ||
			!ValidDbId(newObject.operatorId, StorageService()->OperatorDB(), false,
					   RESTAPI::Errors::InvalidOperatorId, *this) ||
			!ValidDbId(newObject.serviceClass, StorageService()->ServiceClassDB(), true,
					   RESTAPI::Errors::InvalidServiceClassId, *this) ||
			!ValidDbId(newObject.configurationId, StorageService()->ConfigurationDB(), true,
					   RESTAPI::Errors::ConfigurationMustExist, *this) ||
			!ValidSubscriberId(newObject.subscriberId, true, *this) ||
			(rawObject->has("deviceRules") && !ValidDeviceRules(newObject.deviceRules, *this)) ||
			!ValidSerialNumber(newObject.serialNumber, false, *this)) {
			return false;
		}

		if (DB_.Exists("serialNumber", newObject.serialNumber)) {
			BadRequest(RESTAPI::Errors::SerialNumberExists);
			return false;
		}

			if (rawObject->has("configuration")) {
				ProvObjects::DeviceConfiguration subscriberConfiguration;
				ProvObjects::CreateObjectInfo(UserInfo_.userinfo, subscriberConfiguration.info);
				subscriberConfiguration.info.name =
					fmt::format("subscriber-device:{}", newObject.serialNumber);
			subscriberConfiguration.configuration = newObject.configuration;
			subscriberConfiguration.managementPolicy = newObject.managementPolicy;
				subscriberConfiguration.deviceTypes = {
					newObject.deviceType.empty() ? std::string("*") : newObject.deviceType};
				subscriberConfiguration.subscriberOnly = true;
				subscriberConfiguration.subscriber = newObject.subscriberId;
				subscriberConfiguration.entity.clear();
				subscriberConfiguration.venue.clear();

			std::vector<std::string> errors;
			if (!ValidateConfigBlock(ConfigurationValidator::GetType(newObject.deviceGroup),
									 subscriberConfiguration, errors)) {
				BadRequest(RESTAPI::Errors::ConfigBlockInvalid);
				return false;
			}
			if (!StorageService()->ConfigurationDB().CreateRecord(subscriberConfiguration)) {
				InternalError(RESTAPI::Errors::RecordNotCreated);
				return false;
			}
			newObject.configurationId = subscriberConfiguration.info.id;
			newObject.configuration.clear();
		}
		if (!EnsureSubscriberConfigurationOwnership(newObject.serialNumber, newObject.subscriberId,
													newObject.deviceType, newObject.managementPolicy,
													newObject.configurationId, UserInfo_.userinfo,
													Logger())) {
			InternalError(RESTAPI::Errors::RecordNotCreated);
			return false;
		}

		return true;
	}

	bool RESTAPI_sub_devices_handler::ValidateAndNormalizeDeviceGroup(
		SubscriberDeviceDB::RecordName &newObject) {
		Poco::toLowerInPlace(newObject.deviceGroup);
		if (newObject.deviceGroup.empty()) {
			newObject.deviceGroup = "ap";
		}

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

	bool RESTAPI_sub_devices_handler::ValidateAndNormalizeDeviceGroupForUpdate(
		SubscriberDeviceDB::RecordName &existingObject,
		const SubscriberDeviceDB::RecordName &updateObject,
		const Poco::JSON::Object::Ptr &rawObject) {
		if (!rawObject->has("deviceGroup") && !rawObject->has("subscriberId")) {
			return true;
		}

		std::string existingGroup = existingObject.deviceGroup;
		Poco::toLowerInPlace(existingGroup);
		if (existingGroup.empty()) {
			existingGroup = "ap";
		}

		auto targetGroup = existingGroup;
		if (rawObject->has("deviceGroup")) {
			targetGroup = updateObject.deviceGroup;
			Poco::toLowerInPlace(targetGroup);
			if (targetGroup.empty()) {
				targetGroup = "ap";
			}
		}

		if (targetGroup != "olg" && targetGroup != "ap") {
			poco_warning(Logger(),
						 fmt::format("[SUBSCRIBER_DEVICE_UPDATE]: Rejecting serial [{}]. "
									 "Invalid deviceGroup [{}].",
									 existingObject.serialNumber, targetGroup));
			BadRequest(RESTAPI::Errors::InvalidSubscriberDeviceGroup);
			return false;
		}

		const auto sourceSubscriberId = existingObject.subscriberId;
		auto targetSubscriberId = sourceSubscriberId;
		if (rawObject->has("subscriberId")) {
			targetSubscriberId = updateObject.subscriberId;
		}

		if (targetSubscriberId == sourceSubscriberId && targetGroup == existingGroup) {
			return true;
		}

		const auto sourceCount =
			DB_.Count(DB_.OP("subscriberId", ORM::EQ, sourceSubscriberId));
		if (existingGroup == "olg" &&
			(targetSubscriberId != sourceSubscriberId || targetGroup != "olg") &&
			sourceCount > 1) {
			BadRequest(RESTAPI::Errors::MustDeleteAPDevicesBeforeOLG);
			return false;
		}

		auto targetCount = DB_.Count(DB_.OP("subscriberId", ORM::EQ, targetSubscriberId));
		if (targetSubscriberId == sourceSubscriberId && targetCount > 0) {
			--targetCount;
		}

		if (targetCount == 0) {
			if (targetGroup != "olg") {
				BadRequest(RESTAPI::Errors::FirstSubscriberDeviceMustBeOLG);
				return false;
			}
		} else if (targetGroup == "olg") {
			BadRequest(RESTAPI::Errors::OnlyFirstSubscriberDeviceCanBeOLG);
			return false;
		}

		return true;
	}

	bool RESTAPI_sub_devices_handler::HasPutRequestChanges(
		const SubscriberDeviceDB::RecordName &existingObject,
		const SubscriberDeviceDB::RecordName &updateObject,
		const Poco::JSON::Object::Ptr &rawObject) const {
		if (rawObject->has("name") && existingObject.info.name != updateObject.info.name) {
			return true;
		}
		if (rawObject->has("description") &&
			existingObject.info.description != updateObject.info.description) {
			return true;
		}
		if (rawObject->has("notes")) {
			return true;
		}
		if (rawObject->has("deviceType") && existingObject.deviceType != updateObject.deviceType) {
			return true;
		}
		if (rawObject->has("subscriberId") && existingObject.subscriberId != updateObject.subscriberId) {
			return true;
		}
		if (rawObject->has("managementPolicy") &&
			existingObject.managementPolicy != updateObject.managementPolicy) {
			return true;
		}
		if (rawObject->has("serviceClass") && existingObject.serviceClass != updateObject.serviceClass) {
			return true;
		}
		if (rawObject->has("qrCode") && existingObject.qrCode != updateObject.qrCode) {
			return true;
		}
		if (rawObject->has("geoCode") && existingObject.geoCode != updateObject.geoCode) {
			return true;
		}
		if (rawObject->has("deviceRules") &&
			RESTAPI_utils::to_string(existingObject.deviceRules) !=
				RESTAPI_utils::to_string(updateObject.deviceRules)) {
			return true;
		}
		if (rawObject->has("state") && existingObject.state != updateObject.state) {
			return true;
		}
		if (rawObject->has("locale") && existingObject.locale != updateObject.locale) {
			return true;
		}
		if (rawObject->has("billingCode") && existingObject.billingCode != updateObject.billingCode) {
			return true;
		}
		if (rawObject->has("realMacAddress") &&
			existingObject.realMacAddress != updateObject.realMacAddress) {
			return true;
		}
		if (rawObject->has("contact") &&
			RESTAPI_utils::to_string(existingObject.contact) !=
				RESTAPI_utils::to_string(updateObject.contact)) {
			return true;
		}
		if (rawObject->has("location") &&
			RESTAPI_utils::to_string(existingObject.location) !=
				RESTAPI_utils::to_string(updateObject.location)) {
			return true;
		}
		if ((rawObject->has("configurationId") || rawObject->has("configuration")) &&
			existingObject.configurationId != updateObject.configurationId) {
			return true;
		}
		if (rawObject->has("deviceGroup")) {
			std::string existingGroup = existingObject.deviceGroup;
			std::string updateGroup = updateObject.deviceGroup;
			Poco::toLowerInPlace(existingGroup);
			Poco::toLowerInPlace(updateGroup);
			if (existingGroup.empty()) {
				existingGroup = "ap";
			}
			if (updateGroup.empty()) {
				updateGroup = "ap";
			}
			if (existingGroup != updateGroup) {
				return true;
			}
		}
		return false;
	}

	bool RESTAPI_sub_devices_handler::HasPutConfigurationChange(
		const SubscriberDeviceDB::RecordName &existingObject,
		const SubscriberDeviceDB::RecordName &updateObject,
		const Poco::JSON::Object::Ptr &rawObject) const {
		if (rawObject->has("configuration")) {
			// Inline configuration updates may modify an existing configuration record in place.
			// Push is still required even when the configuration UUID remains unchanged.
			return true;
		}
		if (!rawObject->has("configurationId")) {
			return false;
		}
		return existingObject.configurationId != updateObject.configurationId;
	}

	bool RESTAPI_sub_devices_handler::ApplyPutRequest(
		const SubscriberDeviceDB::RecordName &updateObject,
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
		if (rawObject->has("configurationId") || rawObject->has("configuration")) {
			existingObject.configurationId = updateObject.configurationId;
		}
		return true;
	}

	void RESTAPI_sub_devices_handler::ApplyPostCreateSync(
		const SubscriberDeviceDB::RecordName &newObject) {
		SubscriberProvisioning::SyncOptions syncOptions;
		syncOptions.createVenueIfMissing = true;
		syncOptions.enableMonitoringForOlg = true;
		syncOptions.client = this;
		syncOptions.actor = &UserInfo_.userinfo;
		if (!SubscriberProvisioning::SyncInventoryForSubscriberDevice(newObject, Logger(),
																	  syncOptions)) {
			poco_warning(Logger(), fmt::format("[SUBSCRIBER_DEVICE_CREATE]: Post-create sync failed "
											   "for serial [{}].",
											   newObject.serialNumber));
		}
	}

	void RESTAPI_sub_devices_handler::ApplyPostUpdateSync(
		const SubscriberDeviceDB::RecordName &beforeUpdate,
		const SubscriberDeviceDB::RecordName &afterUpdate) {
		std::string beforeGroup = beforeUpdate.deviceGroup;
		std::string afterGroup = afterUpdate.deviceGroup;
		Poco::toLowerInPlace(beforeGroup);
		Poco::toLowerInPlace(afterGroup);
		if (beforeGroup.empty()) {
			beforeGroup = "ap";
		}
		if (afterGroup.empty()) {
			afterGroup = "ap";
		}

		if (beforeGroup == "olg" &&
			(beforeUpdate.subscriberId != afterUpdate.subscriberId || afterGroup != "olg")) {
			if (!SubscriberProvisioning::StopMonitoringForSubscriberDevice(beforeUpdate, Logger())) {
				poco_warning(Logger(), fmt::format("[SUBSCRIBER_DEVICE_UPDATE]: Failed to stop "
												   "monitoring for serial [{}].",
												   beforeUpdate.serialNumber));
			}
		}

		SubscriberProvisioning::SyncOptions syncOptions;
		syncOptions.createVenueIfMissing = true;
		syncOptions.enableMonitoringForOlg = true;
		syncOptions.client = this;
		syncOptions.actor = &UserInfo_.userinfo;
		if (!SubscriberProvisioning::SyncInventoryForSubscriberDevice(afterUpdate, Logger(),
																	  syncOptions)) {
			poco_warning(Logger(), fmt::format("[SUBSCRIBER_DEVICE_UPDATE]: Post-update sync failed "
											   "for serial [{}].",
											   afterUpdate.serialNumber));
		}
	}

	bool RESTAPI_sub_devices_handler::CreateSubscriberDeviceRecord(
		const SubscriberDeviceDB::RecordName &newObject) {
		if (!DB_.CreateRecord(newObject)) {
			InternalError(RESTAPI::Errors::RecordNotCreated);
			return false;
		}
		return true;
	}

	bool RESTAPI_sub_devices_handler::EnsureInventoryRecordForSubscriberDevice(
		const SubscriberDeviceDB::RecordName &newObject) {
		auto &inventoryDB = StorageService()->InventoryDB();
		ProvObjects::InventoryTag inventoryRecord;
		if (inventoryDB.GetRecord("serialNumber", newObject.serialNumber, inventoryRecord)) {
			return true;
		}

		ProvObjects::CreateObjectInfo(UserInfo_.userinfo, inventoryRecord.info);
		inventoryRecord.info.name = newObject.serialNumber;
		inventoryRecord.serialNumber = newObject.serialNumber;
		inventoryRecord.subscriber = newObject.subscriberId;
		inventoryRecord.deviceType = newObject.deviceType;
		inventoryRecord.managementPolicy = newObject.managementPolicy;
		inventoryRecord.deviceConfiguration = newObject.configurationId;
		inventoryRecord.deviceRules = newObject.deviceRules;
		inventoryRecord.state = newObject.state;
		inventoryRecord.locale = newObject.locale;
		inventoryRecord.realMacAddress = newObject.realMacAddress;
		inventoryRecord.devClass = "any";
		inventoryRecord.platform = "AP";

		if (!inventoryDB.CreateRecord(inventoryRecord)) {
			// The record may have been created by auto-discovery concurrently.
			if (inventoryDB.GetRecord("serialNumber", newObject.serialNumber, inventoryRecord)) {
				return true;
			}
			poco_warning(Logger(), fmt::format("[SUBSCRIBER_DEVICE_CREATE]: Failed to create "
											   "inventory record for serial [{}].",
											   newObject.serialNumber));
			return false;
		}

		SerialNumberCache()->AddSerialNumber(newObject.serialNumber, newObject.deviceType);
		MoveUsage(StorageService()->PolicyDB(), inventoryDB, "", inventoryRecord.managementPolicy,
				  inventoryRecord.info.id);
		MoveUsage(StorageService()->ConfigurationDB(), inventoryDB, "",
				  inventoryRecord.deviceConfiguration, inventoryRecord.info.id);
		return true;
	}

	void RESTAPI_sub_devices_handler::ReturnSubscriberDeviceObject(
		const ProvObjects::SubscriberDevice &device) {
		Poco::JSON::Object answer;
		device.to_json(answer);
		ReturnObject(answer);
	}

	bool RESTAPI_sub_devices_handler::StopMonitoringForDeleteIfNeeded(
		const ProvObjects::SubscriberDevice &existingObject, bool &monitoringStopped) {
		monitoringStopped = false;
		if (existingObject.deviceGroup != "olg") {
			return true;
		}
		if (!SubscriberProvisioning::StopMonitoringForSubscriberDevice(existingObject, Logger())) {
			InternalError(RESTAPI::Errors::RecordNotUpdated);
			return false;
		}
		monitoringStopped = true;
		return true;
	}

	void RESTAPI_sub_devices_handler::RestoreMonitoringAfterDeleteFailure(
		const ProvObjects::SubscriberDevice &existingObject) {
		SubscriberProvisioning::SyncOptions syncOptions;
		syncOptions.enableMonitoringForOlg = true;
		syncOptions.client = this;
		if (!SubscriberProvisioning::SyncInventoryForSubscriberDevice(existingObject, Logger(),
																	  syncOptions)) {
			poco_warning(Logger(), fmt::format("[SUBSCRIBER_DEVICE_DELETE]: Failed to restore "
											   "monitoring state for serial [{}] after delete "
											   "failure.",
											   existingObject.serialNumber));
		}
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
			CleanupSubscriberConfigurationRecord(existingObject.configurationId, "", Logger());
			return;
		}

		std::set<std::string> configurationsToCleanup;
		if (!existingObject.configurationId.empty()) {
			configurationsToCleanup.insert(existingObject.configurationId);
		}
		if (!inventoryRecord.deviceConfiguration.empty()) {
			configurationsToCleanup.insert(inventoryRecord.deviceConfiguration);
		}

		CleanupInventoryAssociations(inventoryRecord);
		for (const auto &configurationId : configurationsToCleanup) {
			CleanupSubscriberConfigurationRecord(configurationId, inventoryRecord.info.id, Logger());
		}
		if (!StorageService()->InventoryDB().DeleteRecord("id", inventoryRecord.info.id)) {
			poco_warning(Logger(), fmt::format("[SUBSCRIBER_DEVICE_DELETE]: Failed to delete "
											   "inventory [{}] for serial [{}].",
											   inventoryRecord.info.id,
											   existingObject.serialNumber));
			return;
		}
		SerialNumberCache()->DeleteSerialNumber(existingObject.serialNumber);
	}

	bool RESTAPI_sub_devices_handler::DeleteSubscriberDevice(
		const ProvObjects::SubscriberDevice &existingObject) {
		bool monitoringStopped = false;
		if (!StopMonitoringForDeleteIfNeeded(existingObject, monitoringStopped)) {
			return false;
		}

		if (!DB_.DeleteRecord("id", existingObject.info.id)) {
			if (monitoringStopped) {
				RestoreMonitoringAfterDeleteFailure(existingObject);
			}
			BadRequest(RESTAPI::Errors::NoRecordsDeleted);
			return false;
		}

		DeleteInventoryForSubscriberDevice(existingObject);
		return true;
	}

	void RESTAPI_sub_devices_handler::ReturnSubscriberDeviceRecord(const std::string &uuid) {
		SubscriberDeviceDB::RecordName createdObject;
		if (!DB_.GetRecord("id", uuid, createdObject)) {
			return InternalError(RESTAPI::Errors::RecordNotCreated);
		}

		Poco::JSON::Object answer;
		createdObject.to_json(answer);
		ReturnObject(answer);
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

		ProvObjects::SubscriberDevice existingObject;
		if (!ValidateDeleteRequest(deviceIdOrSerial, existingObject)) {
			return;
		}
		if (!DeleteSubscriberDevice(existingObject)) {
			return;
		}
		return OK();
	}

	void RESTAPI_sub_devices_handler::DoPost() {
		SubscriberDeviceDB::RecordName newObject;
		if (!ParseAndValidatePostRequest(newObject)) {
			return;
		}
		const auto shouldPushConfiguration = !newObject.configurationId.empty();

		if (!ValidateAndNormalizeDeviceGroup(newObject)) {
			return;
		}

		poco_information(Logger(),
						 fmt::format("[SUBSCRIBER_DEVICE_CREATE]: subscriber [{}], serial [{}], "
									 "deviceGroup [{}].",
									 newObject.subscriberId, newObject.serialNumber,
									 newObject.deviceGroup));

		ProvObjects::CreateObjectInfo(ParsedBody_, UserInfo_.userinfo, newObject.info);
		if (!CreateSubscriberDeviceRecord(newObject)) {
			return;
		}

		if (!EnsureInventoryRecordForSubscriberDevice(newObject)) {
			if (!DB_.DeleteRecord("id", newObject.info.id)) {
				poco_warning(Logger(), fmt::format("[SUBSCRIBER_DEVICE_CREATE]: Failed to rollback "
												   "subscriber-device [{}] after inventory create "
												   "failure.",
												   newObject.info.id));
			}
			return InternalError(RESTAPI::Errors::RecordNotCreated);
		}

		ApplyPostCreateSync(newObject);
		if (shouldPushConfiguration && !PushConfigurationIfInInventory(newObject.serialNumber)) {
			poco_warning(Logger(), fmt::format("[SUBSCRIBER_DEVICE_CREATE]: Configuration push "
											   "did not converge for serial [{}]. Returning "
											   "created record and relying on subsequent sync.",
											   newObject.serialNumber));
		}
		return ReturnSubscriberDeviceRecord(newObject.info.id);
	}

	void RESTAPI_sub_devices_handler::DoPut() {
		auto uuid = GetBinding("uuid");
		SubscriberDeviceDB::RecordName existingObject;
		SubscriberDeviceDB::RecordName updateObject;
		if (!ParseAndValidatePutRequest(uuid, existingObject, updateObject)) {
			return;
		}

		const auto &rawObject = ParsedBody_;
		const auto hasConfigurationChange =
			HasPutConfigurationChange(existingObject, updateObject, rawObject);
		if (!HasPutRequestChanges(existingObject, updateObject, rawObject)) {
			// No DB changes, but retry convergence should still attempt sync.
			ApplyPostUpdateSync(existingObject, existingObject);
			if (hasConfigurationChange &&
				!PushConfigurationIfInInventory(existingObject.serialNumber)) {
				return BadRequest(RESTAPI::Errors::SubConfigNotRefreshed);
			}
			return ReturnSubscriberDeviceObject(existingObject);
		}

		const auto beforeUpdate = existingObject;
		if (!ApplyPutRequest(updateObject, existingObject, rawObject)) {
			return;
		}
		if (!DB_.UpdateRecord("id", uuid, existingObject)) {
			return InternalError(RESTAPI::Errors::RecordNotUpdated);
		}

		ApplyPostUpdateSync(beforeUpdate, existingObject);
		if (hasConfigurationChange &&
			!PushConfigurationIfInInventory(existingObject.serialNumber)) {
			return BadRequest(RESTAPI::Errors::SubConfigNotRefreshed);
		}

		return ReturnSubscriberDeviceObject(existingObject);
	}

} // namespace OpenWifi
