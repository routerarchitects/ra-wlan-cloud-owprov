/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include "RESTAPI_inventory_handler.h"

#include "APConfig.h"
#include "AutoDiscovery.h"
#include "DeviceTypeCache.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "SerialNumberCache.h"
#include "StorageService.h"
#include "Tasks/VenueConfigUpdater.h"
#include "framework/utils.h"
#include "sdks/SDK_gw.h"
#include "sdks/SDK_sec.h"

#ifdef CGW_INTEGRATION
#include "sdks/SDK_cgw.h"
#endif
namespace OpenWifi {

	void RESTAPI_inventory_handler::DoGet() {

		ProvObjects::InventoryTag Existing;
		std::string SerialNumber = GetBinding(RESTAPI::Protocol::SERIALNUMBER, "");
		poco_debug(Logger(), fmt::format("{}: Retrieving inventory information.", SerialNumber));
		if (SerialNumber.empty() ||
			!DB_.GetRecord(RESTAPI::Protocol::SERIALNUMBER, SerialNumber, Existing)) {
			return NotFound();
		}
		poco_debug(Logger(), fmt::format("{},{}: Retrieving inventory information.",
										 Existing.serialNumber, Existing.info.id));

		Poco::JSON::Object Answer;
		std::string Arg;
		if (GetBoolParameter("config", false)) {
			bool Explain = GetBoolParameter("explain", false);

			APConfig Device(SerialNumber, Existing.deviceType, Logger(), Explain);

			auto Configuration = Poco::makeShared<Poco::JSON::Object>();
			if (Device.Get(Configuration)) {
				Answer.set("config", Configuration);
				if (Explain)
					Answer.set("explanation", Device.Explanation());
			} else {
				Answer.set("config", "none");
			}
			return ReturnObject(Answer);
		} else if (GetBoolParameter("firmwareOptions", false)) {
			ProvObjects::DeviceRules Rules;
			StorageService()->InventoryDB().EvaluateDeviceSerialNumberRules(SerialNumber, Rules);
			Answer.set("firmwareUpgrade", Rules.firmwareUpgrade);
			Answer.set("firmwareRCOnly", Rules.rcOnly == "yes");
			return ReturnObject(Answer);
		} else if (GetBoolParameter("rrmSettings", false)) {
			ProvObjects::DeviceRules Rules;
			StorageService()->InventoryDB().EvaluateDeviceSerialNumberRules(SerialNumber, Rules);
			if (Rules.rrm == "no" || Rules.rrm == "inherit") {
				Answer.set("rrm", Rules.rrm);
			} else {
				ProvObjects::RRMDetails D;
				Poco::JSON::Parser P;
				try {
					auto Obj = P.parse(Rules.rrm).extract<Poco::JSON::Object::Ptr>();
					Answer.set("rrm", Obj);
				} catch (...) {
					Answer.set("rrm", "invalid");
				}
			}
			return ReturnObject(Answer);
		} else if (GetBoolParameter("applyConfiguration", false)) {
			poco_debug(Logger(),
					   fmt::format("{}: Retrieving configuration.", Existing.serialNumber));
			auto Device =
				std::make_shared<APConfig>(SerialNumber, Existing.deviceType, Logger(), false);
			auto Configuration = Poco::makeShared<Poco::JSON::Object>();
			Poco::JSON::Object ErrorsObj, WarningsObj;
			ProvObjects::InventoryConfigApplyResult Results;
			poco_debug(Logger(),
					   fmt::format("{}: Computing configuration.", Existing.serialNumber));
			if (Device->Get(Configuration)) {
				std::ostringstream OS;
				Configuration->stringify(OS);
				Results.appliedConfiguration = OS.str();
				auto Response = Poco::makeShared<Poco::JSON::Object>();
				poco_debug(Logger(),
						   fmt::format("{}: Sending configuration push.", Existing.serialNumber));
				if (SDK::GW::Device::Configure(this, SerialNumber, Configuration, Response)) {
					poco_debug(Logger(), fmt::format("{}: Sending configuration pushed.",
													 Existing.serialNumber));
					GetRejectedLines(Response, Results.warnings);
					Results.errorCode = 0;
				} else {
					poco_debug(Logger(), fmt::format("{}: Sending configuration failed.",
													 Existing.serialNumber));
					Results.errorCode = 1;
				}
			} else {
				poco_debug(Logger(),
						   fmt::format("{}: Configuration is bad.", Existing.serialNumber));
				Results.errorCode = 1;
			}
			Results.to_json(Answer);
			return ReturnObject(Answer);
		} else if (GetBoolParameter("resolveConfig", false)) {
			poco_debug(Logger(),
					   fmt::format("{}: Retrieving configuration.", Existing.serialNumber));
			auto Device =
				std::make_shared<APConfig>(SerialNumber, Existing.deviceType, Logger(), false);
			auto Configuration = Poco::makeShared<Poco::JSON::Object>();
			Poco::JSON::Object ErrorsObj, WarningsObj;
			ProvObjects::InventoryConfigApplyResult Results;
			poco_debug(Logger(),
					   Poco::format("{}: Computing configuration.", Existing.serialNumber));
			if (Device->Get(Configuration)) {
				Answer.set("configuration", Configuration);
			} else {
				Answer.set("error", 1);
			}
			return ReturnObject(Answer);
		} else if (QB_.AdditionalInfo) {
			AddExtendedInfo(Existing, Answer);
		}
		Existing.to_json(Answer);
		ReturnObject(Answer);
	}

	void RESTAPI_inventory_handler::DoDelete() {
		ProvObjects::InventoryTag Existing;
		std::string SerialNumber = GetBinding(RESTAPI::Protocol::SERIALNUMBER, "");
		if (SerialNumber.empty() ||
			!DB_.GetRecord(RESTAPI::Protocol::SERIALNUMBER, SerialNumber, Existing)) {
			return NotFound();
		}

		/* When device is removed from the venue, if group present in groupsmap table.
		*  Call cgw-rest DeleteDeviceFromGroup
		*/
#ifdef CGW_INTEGRATION
		if (!Existing.venue.empty()) {
			uint64_t groupId = 0;
			if (!StorageService()->GroupsMapDB().GetGroup(Existing.venue, groupId)) {
				return InternalError(RESTAPI::Errors::CouldNotBeDeleted);
				poco_warning(Logger(), fmt::format("{}: CGW groupsmap lookup failed for venue {}", SerialNumber, Existing.venue));
			}

			std::string macHex = SerialNumber;
			Poco::replaceInPlace(macHex, ":", "");
			Poco::replaceInPlace(macHex, "-", "");
			Poco::toLowerInPlace(macHex);
			if (macHex.size() != 12) {
				poco_warning(Logger(), fmt::format("{}: Invalid MAC/Serial for CGW mapping: {}", SerialNumber, macHex));
				return BadRequest(RESTAPI::Errors::InvalidSerialNumber);
			}
			auto macColon = Utils::SerialToMAC(macHex);
			// Call CGW API to remove device from infra for this group
			if (!SDK::CGW::DeleteDeviceFromGroup(groupId, macColon)) {
				poco_warning(Logger(), fmt::format("{}: CGW DeleteDeviceFromGroup failed gid={} mac={}", SerialNumber, groupId, macColon));
				return InternalError(RESTAPI::Errors::CouldNotBeDeleted);
			}
		}
		else{
			poco_warning(Logger(), fmt::format("Device doesn't exist in venue:{}", SerialNumber));
		}

#endif
		MoveUsage(StorageService()->PolicyDB(), DB_, Existing.managementPolicy, "",
				  Existing.info.id);
		RemoveMembership(StorageService()->VenueDB(), &ProvObjects::Venue::configurations,
						 Existing.venue, Existing.info.id);
		RemoveMembership(StorageService()->EntityDB(), &ProvObjects::Entity::configurations,
						 Existing.entity, Existing.info.id);
		MoveUsage(StorageService()->LocationDB(), DB_, Existing.location, "", Existing.info.id);
		MoveUsage(StorageService()->ContactDB(), DB_, Existing.contact, "", Existing.info.id);

		if (!Existing.deviceConfiguration.empty()) {
			ProvObjects::DeviceConfiguration DC;
			if (StorageService()->ConfigurationDB().GetRecord("id", Existing.deviceConfiguration,
															  DC)) {
				if (DC.subscriberOnly)
					StorageService()->ConfigurationDB().DeleteRecord("id",
																	 Existing.deviceConfiguration);
				else
					StorageService()->ConfigurationDB().DeleteInUse(
						"id", Existing.deviceConfiguration, DB_.Prefix(), Existing.info.id);
			}
		}

		MoveUsage(StorageService()->PolicyDB(), DB_, Existing.managementPolicy, "",
				  Existing.info.id);
		MoveUsage(StorageService()->LocationDB(), DB_, Existing.location, "", Existing.info.id);
		MoveUsage(StorageService()->ContactDB(), DB_, Existing.contact, "", Existing.info.id);
		MoveUsage(StorageService()->ConfigurationDB(), DB_, Existing.deviceConfiguration, "",
				  Existing.info.id);
		ManageMembership(StorageService()->EntityDB(), &ProvObjects::Entity::devices,
						 Existing.entity, "", Existing.info.id);
		ManageMembership(StorageService()->VenueDB(), &ProvObjects::Venue::devices, Existing.venue,
						 "", Existing.info.id);
		DB_.DeleteRecord("id", Existing.info.id);
		SerialNumberCache()->DeleteSerialNumber(SerialNumber);
		return OK();
	}

	void RESTAPI_inventory_handler::DoPost() {
		std::string SerialNumber = GetBinding(RESTAPI::Protocol::SERIALNUMBER, "");
		Poco::toLowerInPlace(SerialNumber);
		if (SerialNumber.empty()) {
			return BadRequest(RESTAPI::Errors::MissingSerialNumber);
		}

		if (!NormalizeMac(SerialNumber)) {
			return BadRequest(RESTAPI::Errors::InvalidSerialNumber);
		}

		if (DB_.Exists(RESTAPI::Protocol::SERIALNUMBER, SerialNumber)) {
			return BadRequest(RESTAPI::Errors::SerialNumberExists);
		}

		const auto &RawObject = ParsedBody_;
		ProvObjects::InventoryTag NewObject;
		if (!NewObject.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		NormalizeMac(NewObject.serialNumber);
		if (SerialNumber != NewObject.serialNumber) {
			return BadRequest(RESTAPI::Errors::SerialNumberMismatch);
		}

		if ((RawObject->has("deviceRules") && !ValidDeviceRules(NewObject.deviceRules, *this))) {
			return;
		}

		if (!Provisioning::DeviceClass::Validate(NewObject.devClass.c_str())) {
			return BadRequest(RESTAPI::Errors::InvalidDeviceClass);
		}

		if (NewObject.devClass.empty()) {
			NewObject.devClass = Provisioning::DeviceClass::ANY;
		}

		if (!ProvObjects::CreateObjectInfo(RawObject, UserInfo_.userinfo, NewObject.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		if (NewObject.deviceType.empty() ||
			!DeviceTypeCache()->IsAcceptableDeviceType(NewObject.deviceType)) {
			return BadRequest(RESTAPI::Errors::InvalidDeviceTypes);
		}

		if (OpenWifi::EntityDB::IsRoot(NewObject.entity) ||
			(!NewObject.entity.empty() &&
			 !StorageService()->EntityDB().Exists("id", NewObject.entity))) {
			return BadRequest(RESTAPI::Errors::ValidNonRootUUID);
		}

		if (!NewObject.venue.empty() &&
			!StorageService()->VenueDB().Exists("id", NewObject.venue)) {
			return BadRequest(RESTAPI::Errors::VenueMustExist);
		}

		if (!NewObject.venue.empty() && !NewObject.entity.empty()) {
			return BadRequest(RESTAPI::Errors::NotBoth);
		}

		if (!NewObject.location.empty() &&
			!StorageService()->LocationDB().Exists("id", NewObject.location)) {
			return BadRequest(RESTAPI::Errors::LocationMustExist);
		}

		if (!NewObject.contact.empty() &&
			!StorageService()->ContactDB().Exists("id", NewObject.contact)) {
			return BadRequest(RESTAPI::Errors::ContactMustExist);
		}

		if (!NewObject.deviceConfiguration.empty() &&
			!StorageService()->ConfigurationDB().Exists("id", NewObject.deviceConfiguration)) {
			return BadRequest(RESTAPI::Errors::ConfigurationMustExist);
		}

		if (!NewObject.managementPolicy.empty() &&
			!StorageService()->PolicyDB().Exists("id", NewObject.managementPolicy)) {
			return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
		}

		std::vector<std::string> Errors;
		auto ObjectsCreated = CreateObjects(NewObject, *this, Errors);
		if (!Errors.empty()) {
			return BadRequest(RESTAPI::Errors::ConfigBlockInvalid);
		}

		/* When device is added in the venue, if group present in groupsmap table.
		*  Call cgw-rest AddDeviceToGroup.
		*/
#ifdef CGW_INTEGRATION
		if (!NewObject.venue.empty()) {
			uint64_t groupId = 0;
			if (!StorageService()->GroupsMapDB().GetGroup(NewObject.venue, groupId)) {
				poco_warning(Logger(), fmt::format("{}: No CGW group mapping for venue {}", SerialNumber, NewObject.venue));
				return BadRequest(RESTAPI::Errors::InternalError);
			}
			std::string macHex = SerialNumber;
			Poco::replaceInPlace(macHex, ":", "");
			Poco::replaceInPlace(macHex, "-", "");
			Poco::toLowerInPlace(macHex);
			if (macHex.size() != 12) {
				poco_warning(Logger(), fmt::format("{}: Invalid MAC/Serial for CGW mapping: {}", SerialNumber, macHex));
				return BadRequest(RESTAPI::Errors::InvalidSerialNumber);
			}
			auto macColon = Utils::SerialToMAC(macHex);
			if (!SDK::CGW::AddDeviceToGroup(groupId, macColon)) {
				poco_warning(Logger(), fmt::format("{}: CGW AddDeviceToGroup failed gid={}", SerialNumber, groupId));
				return BadRequest(RESTAPI::Errors::InternalError);
			}
		}
#endif
		if (DB_.CreateRecord(NewObject)) {
			SDK::GW::Device::SetOwnerShip(this, SerialNumber, NewObject.entity, NewObject.venue,
										  NewObject.subscriber);
			SerialNumberCache()->AddSerialNumber(SerialNumber, NewObject.deviceType);
			MoveUsage(StorageService()->PolicyDB(), DB_, "", NewObject.managementPolicy,
					  NewObject.info.id);
			MoveUsage(StorageService()->LocationDB(), DB_, "", NewObject.location,
					  NewObject.info.id);
			MoveUsage(StorageService()->ContactDB(), DB_, "", NewObject.contact, NewObject.info.id);
			MoveUsage(StorageService()->ConfigurationDB(), DB_, "", NewObject.deviceConfiguration,
					  NewObject.info.id);
			ManageMembership(StorageService()->EntityDB(), &ProvObjects::Entity::devices, "",
							 NewObject.entity, NewObject.info.id);
			ManageMembership(StorageService()->VenueDB(), &ProvObjects::Venue::devices, "",
							 NewObject.venue, NewObject.info.id);

			ProvObjects::InventoryTag NewTag;
			DB_.GetRecord("id", NewObject.info.id, NewTag);
			Poco::JSON::Object Answer;
			NewTag.to_json(Answer);
			return ReturnObject(Answer);
		}
		InternalError(RESTAPI::Errors::RecordNotCreated);
	}

	void RESTAPI_inventory_handler::DoPut() {

		std::string SerialNumber = GetBinding(RESTAPI::Protocol::SERIALNUMBER, "");
		if (SerialNumber.empty() || !Utils::ValidSerialNumber(SerialNumber)) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
		}

		ProvObjects::InventoryTag Existing;
		if (SerialNumber.empty() ||
			!DB_.GetRecord(RESTAPI::Protocol::SERIALNUMBER, SerialNumber, Existing)) {
			return NotFound();
		}

		std::string previous_venue = Existing.venue;

		auto RemoveSubscriber = GetParameter("removeSubscriber");
		if (!RemoveSubscriber.empty()) {
			if (Existing.subscriber == RemoveSubscriber) {
				poco_information(Logger(), fmt::format("{}: removing subscriber ({})", SerialNumber,
													   RemoveSubscriber));
				ProvObjects::DeviceConfiguration DC;
				if (StorageService()->ConfigurationDB().GetRecord(
						"id", Existing.deviceConfiguration, DC)) {
					poco_information(Logger(),
									 fmt::format("{}: removing configuration for subscriber ({})",
												 SerialNumber, RemoveSubscriber));
					if (DC.subscriberOnly) {
						if (!StorageService()->ConfigurationDB().DeleteRecord(
								"id", Existing.deviceConfiguration)) {
							poco_debug(Logger(), "Could not delete the subscriber configuration");
						}
					} else {
						poco_debug(Logger(), "Configurations is not for a subscriber.");
					}
					Existing.deviceConfiguration = "";
				}
				Existing.subscriber = "";
				Existing.devClass = Provisioning::DeviceClass::ANY;
				Poco::JSON::Object state;
				state.set("date", Utils::Now());
				state.set("method", "auto-discovery");
				state.set("last-operation", "returned to inventory");
				std::ostringstream OO;
				state.stringify(OO);
				Existing.state = OO.str();
				StorageService()->InventoryDB().UpdateRecord("id", Existing.info.id, Existing);

				// Delete subsriber and associated device records from other databases.
				StorageService()->SignupDB().DeleteRecord("userid", RemoveSubscriber);
				StorageService()->SubscriberDeviceDB().DeleteRecord("subscriberid", RemoveSubscriber);

				RemoveMembership(StorageService()->EntityDB(), &ProvObjects::Entity::devices, "id",
								 Existing.info.id);
				Poco::JSON::Object Answer;
				Existing.to_json(Answer);
				SDK::GW::Device::SetSubscriber(nullptr, SerialNumber, "");
				return ReturnObject(Answer);
			} else {
				poco_information(Logger(), fmt::format("{}: wrong subscriber ({})", SerialNumber,
													   RemoveSubscriber));
			}
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
		}

		const auto &RawObject = ParsedBody_;
		ProvObjects::InventoryTag NewObject;
		if (!NewObject.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if ((RawObject->has("deviceRules") && !ValidDeviceRules(NewObject.deviceRules, *this))) {
			return;
		}

		if (!Provisioning::DeviceClass::Validate(NewObject.devClass.c_str())) {
			return BadRequest(RESTAPI::Errors::InvalidDeviceClass);
		}

		if (!NewObject.deviceType.empty()) {
			if (!DeviceTypeCache()->IsAcceptableDeviceType(NewObject.deviceType)) {
				return BadRequest(RESTAPI::Errors::InvalidDeviceTypes);
			}
		}

		if (!UpdateObjectInfo(RawObject, UserInfo_.userinfo, Existing.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		if (RawObject->has("deviceRules"))
			Existing.deviceRules = NewObject.deviceRules;

		std::string FromPolicy, ToPolicy;
		if (!CreateMove(RawObject, "managementPolicy", &InventoryDB::RecordName::managementPolicy,
						Existing, FromPolicy, ToPolicy, StorageService()->PolicyDB()))
			return BadRequest(RESTAPI::Errors::EntityMustExist);

		std::string FromEntity, ToEntity;
		if (!CreateMove(RawObject, "entity", &InventoryDB::RecordName::entity, Existing, FromEntity,
						ToEntity, StorageService()->EntityDB()))
			return BadRequest(RESTAPI::Errors::EntityMustExist);

		std::string FromVenue, ToVenue;
		if (!CreateMove(RawObject, "venue", &InventoryDB::RecordName::venue, Existing, FromVenue,
						ToVenue, StorageService()->VenueDB()))
			return BadRequest(RESTAPI::Errors::VenueMustExist);

		std::string FromLocation, ToLocation;
		if (!CreateMove(RawObject, "location", &InventoryDB::RecordName::location, Existing,
						FromLocation, ToLocation, StorageService()->LocationDB()))
			return BadRequest(RESTAPI::Errors::VenueMustExist);

		std::string FromContact, ToContact;
		if (!CreateMove(RawObject, "contact", &InventoryDB::RecordName::contact, Existing,
						FromContact, ToContact, StorageService()->ContactDB()))
			return BadRequest(RESTAPI::Errors::VenueMustExist);

		std::string FromConfiguration, ToConfiguration;
		if (!CreateMove(RawObject, "deviceConfiguration",
						&InventoryDB::RecordName::deviceConfiguration, Existing, FromConfiguration,
						ToConfiguration, StorageService()->ConfigurationDB()))
			return BadRequest(RESTAPI::Errors::ConfigurationMustExist);

		std::string NewSubScriber;
		if (AssignIfPresent(RawObject, "subscriber", NewSubScriber)) {
			if (!NewSubScriber.empty()) {
				if (NewSubScriber != Existing.subscriber) {
					SecurityObjects::UserInfo U;
					if (SDK::Sec::Subscriber::Get(this, NewSubScriber, U)) {
						Existing.subscriber = NewSubScriber;
					} else {
						return BadRequest(RESTAPI::Errors::SubscriberMustExist);
					}
				}
			} else {
				Existing.subscriber = "";
			}
		}

		AssignIfPresent(RawObject, "doNotAllowOverrides", Existing.doNotAllowOverrides);

		if (RawObject->has("devClass") && NewObject.devClass != Existing.devClass) {
			Existing.devClass = NewObject.devClass;
		}

		if (RawObject->has("state") && NewObject.state != Existing.state) {
			Existing.state = NewObject.state;
		}

		std::vector<std::string> Errors;
		auto ObjectsCreated = CreateObjects(NewObject, *this, Errors);
		if (!Errors.empty()) {
			return BadRequest(RESTAPI::Errors::ConfigBlockInvalid);
		}

		if (!ObjectsCreated.empty()) {
			auto it = ObjectsCreated.find("configuration");
			if (it != ObjectsCreated.end()) {
				FromConfiguration = "";
				ToConfiguration = it->second;
				Existing.deviceConfiguration = ToConfiguration;
			}
		}

		if (StorageService()->InventoryDB().UpdateRecord("id", Existing.info.id, Existing)) {
			MoveUsage(StorageService()->PolicyDB(), DB_, FromPolicy, ToPolicy, Existing.info.id);
			MoveUsage(StorageService()->LocationDB(), DB_, FromLocation, ToLocation,
					  Existing.info.id);
			MoveUsage(StorageService()->ContactDB(), DB_, FromContact, ToContact, Existing.info.id);
			MoveUsage(StorageService()->ConfigurationDB(), DB_, FromConfiguration, ToConfiguration,
					  Existing.info.id);
			ManageMembership(StorageService()->EntityDB(), &ProvObjects::Entity::devices,
							 FromEntity, ToEntity, Existing.info.id);
			ManageMembership(StorageService()->VenueDB(), &ProvObjects::Venue::devices, FromVenue,
							 ToVenue, Existing.info.id);

			SDK::GW::Device::SetOwnerShip(this, SerialNumber, Existing.entity, Existing.venue,
										  Existing.subscriber);

			// Attempt an automatic config push when the venue is set and different than what is
			// in DB.
			poco_information(Logger(), fmt::format("New Venue {} Old Venue {}", NewObject.venue, previous_venue));
			if (!NewObject.venue.empty() && NewObject.venue != previous_venue) {
				ComputeAndPushConfig(SerialNumber, NewObject.deviceType, Logger());
			}

			ProvObjects::InventoryTag NewObjectCreated;
			DB_.GetRecord("id", Existing.info.id, NewObjectCreated);
			Poco::JSON::Object Answer;
			NewObject.to_json(Answer);
			return ReturnObject(Answer);
		}
		InternalError(RESTAPI::Errors::RecordNotUpdated);
	}
} // namespace OpenWifi
