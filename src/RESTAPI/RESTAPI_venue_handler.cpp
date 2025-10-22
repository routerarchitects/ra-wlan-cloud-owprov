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

#include "RESTAPI_venue_handler.h"

#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"
#include "Tasks/VenueConfigUpdater.h"
#include "Tasks/VenueRebooter.h"
#include "Tasks/VenueUpgrade.h"
#include "framework/CIDR.h"
#include "framework/MicroServiceFuncs.h"

#include "Kafka_ProvUpdater.h"
#ifdef CGW_INTEGRATION
#include "sdks/SDK_cgw.h"
#endif

namespace OpenWifi {

	static Types::UUIDvec_t GetDevices(const ProvObjects::Venue &V, bool GetChildren) {
		Types::UUIDvec_t R;
		std::copy(V.devices.begin(), V.devices.end(), std::back_inserter(R));
		if (GetChildren) {
			for (const auto &i : V.children) {
				ProvObjects::Venue V2;
				if (StorageService()->VenueDB().GetRecord("id", i, V2)) {
					std::copy(V2.devices.begin(), V2.devices.end(), std::back_inserter(R));
					auto LowerDevs = GetDevices(V2, GetChildren);
					std::copy(LowerDevs.begin(), LowerDevs.end(), std::back_inserter(R));
				}
			}
		}

		std::sort(R.begin(), R.end());
		auto Last = std::unique(R.begin(), R.end());
		R.erase(Last, R.end());

		std::vector<std::string> SerialNumbers;

		for (const auto &device : R) {
			ProvObjects::InventoryTag IT;
			if (StorageService()->InventoryDB().GetRecord("id", device, IT)) {
				SerialNumbers.push_back(IT.serialNumber);
			}
		}

		return SerialNumbers;
	}

	void RESTAPI_venue_handler::DoGet() {
		std::string UUID = GetBinding("uuid", "");
		ProvObjects::Venue Existing;
		if (UUID.empty() || !DB_.GetRecord("id", UUID, Existing)) {
			return NotFound();
		}

		if (GetBoolParameter("getDevices")) {
			ProvObjects::VenueDeviceList VDL;
			VDL.id = Existing.info.id;
			VDL.name = Existing.info.name;
			VDL.description = Existing.info.description;
			auto GetChildren = GetBoolParameter("getChildren");
			VDL.devices = GetDevices(Existing, GetChildren);
			Poco::JSON::Object Answer;
			VDL.to_json(Answer);
			return ReturnObject(Answer);
		}

		Poco::JSON::Object Answer;
		if (QB_.AdditionalInfo)
			AddExtendedInfo(Existing, Answer);

		Existing.to_json(Answer);
		ReturnObject(Answer);
	}

	void RESTAPI_venue_handler::DoDelete() {
		std::string UUID = GetBinding("uuid", "");
		ProvObjects::Venue Existing;
		if (UUID.empty() || !DB_.GetRecord("id", UUID, Existing)) {
			return NotFound();
		}

		if (!Existing.children.empty() || !Existing.devices.empty()) {
			return BadRequest(RESTAPI::Errors::StillInUse);
		}

		/* When venue is deleted, if group present in groupsmap table.
		*  Call cgw-rest DeleteGroup on Success delete from groupsmap and venue.
		*/
#ifdef CGW_INTEGRATION
                uint64_t groupId = 0;
                if (!StorageService()->GroupsMapDB().GetGroup(UUID, groupId)) {
                        poco_information(Logger(), "RESTAPI_venue_handler::DoDelete groupsmap lookup failure");
                        return InternalError(RESTAPI::Errors::CouldNotBeDeleted);
                }
                if (!SDK::CGW::DeleteGroup(groupId)) {
                        poco_information(Logger(), "RESTAPI_venue_handler::DoDelete CGW deleteGroup failure");
                        return InternalError(RESTAPI::Errors::CouldNotBeDeleted);
                }
#endif
		if (!Existing.contacts.empty()) {
			for (const auto &contact_uuid : Existing.contacts)
				StorageService()->ContactDB().DeleteInUse(
					"id", contact_uuid, StorageService()->VenueDB().Prefix(), UUID);
		}
		if (!Existing.location.empty())
			StorageService()->LocationDB().DeleteInUse("id", Existing.location,
													   StorageService()->VenueDB().Prefix(), UUID);
		if (!Existing.managementPolicy.empty())
			StorageService()->PolicyDB().DeleteInUse("id", Existing.managementPolicy,
													 StorageService()->VenueDB().Prefix(), UUID);
		if (!Existing.deviceConfiguration.empty()) {
			for (auto &configuration_uuid : Existing.deviceConfiguration)
				StorageService()->ConfigurationDB().DeleteInUse(
					"id", configuration_uuid, StorageService()->VenueDB().Prefix(), UUID);
		}
		if (!Existing.parent.empty())
			StorageService()->VenueDB().DeleteChild("id", Existing.parent, UUID);
		if (!Existing.entity.empty())
			StorageService()->EntityDB().DeleteVenue("id", Existing.entity, UUID);
#ifdef CGW_INTEGRATION
                if (!DB_.DeleteRecord("id", UUID)) {
                        poco_information(Logger(), "RESTAPI_venue_handler::DoDelete venue delete failure");
                        return InternalError(RESTAPI::Errors::CouldNotBeDeleted);
                }
                if (!StorageService()->GroupsMapDB().DeleteVenue(UUID)) {
                        poco_information(Logger(), "RESTAPI_venue_handler::DoDelete groupsmap delete failure");
                        return InternalError(RESTAPI::Errors::CouldNotBeDeleted);
                }
#else
		DB_.DeleteRecord("id", UUID);

#endif
		UpdateKafkaProvisioningObject(ProvisioningOperation::removal, Existing);

		return OK();
	}

	void RESTAPI_venue_handler::DoPost() {
		std::string UUID = GetBinding("uuid", "");
		if (UUID.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		const auto &RawObject = ParsedBody_;
		ProvObjects::Venue NewObject;
		if (!NewObject.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if ((RawObject->has("deviceRules") && !ValidDeviceRules(NewObject.deviceRules, *this))) {
			return;
		}

		if (!CreateObjectInfo(RawObject, UserInfo_.userinfo, NewObject.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		if (NewObject.parent.empty() && NewObject.entity.empty()) {
			return BadRequest(RESTAPI::Errors::ParentOrEntityMustBeSet);
		}

		if (!NewObject.parent.empty() && !NewObject.entity.empty()) {
			return BadRequest(RESTAPI::Errors::NotBoth);
		}

		if (!NewObject.parent.empty() && !DB_.Exists("id", NewObject.parent)) {
			return BadRequest(RESTAPI::Errors::VenueMustExist);
		}

		if (NewObject.entity == EntityDB::RootUUID()) {
			return BadRequest(RESTAPI::Errors::ValidNonRootUUID);
		}

		if (!NewObject.entity.empty() &&
			!StorageService()->EntityDB().Exists("id", NewObject.entity)) {
			return BadRequest(RESTAPI::Errors::EntityMustExist);
		}

        if(StorageService()->VenueDB().DoesVenueNameAlreadyExist(NewObject.info.name,NewObject.entity, NewObject.parent)) {
            return BadRequest(RESTAPI::Errors::VenuesNameAlreadyExists);
        }

		if (!NewObject.contacts.empty()) {
			for (const auto &i : NewObject.contacts) {
				if (!StorageService()->ContactDB().Exists("id", i)) {
					return BadRequest(RESTAPI::Errors::ContactMustExist);
				}
			}
		}

		if (!NewObject.location.empty() &&
			!StorageService()->LocationDB().Exists("id", NewObject.location)) {
			return BadRequest(RESTAPI::Errors::LocationMustExist);
		}

		if (!NewObject.managementPolicy.empty() &&
			!StorageService()->PolicyDB().Exists("id", NewObject.managementPolicy)) {
			return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
		}

		if (!NewObject.deviceConfiguration.empty()) {
			for (auto &i : NewObject.deviceConfiguration) {
				if (!StorageService()->ConfigurationDB().Exists("id", i)) {
					return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
				}
			}
		}

		if (!NewObject.sourceIP.empty() && !CIDR::ValidateIpRanges(NewObject.sourceIP)) {
			return BadRequest(RESTAPI::Errors::InvalidIPRanges);
		}

		NewObject.children.clear();

		RESTAPI::Errors::msg Error = RESTAPI::Errors::SUCCESS;
		std::vector<std::string> Errors;
		auto ObjectsCreated = CreateObjects(NewObject, *this, Errors);
		if (Error.err_num != 0) {
			return BadRequest(RESTAPI::Errors::InternalError);
		}

		/* When venue is created,Insert in the groupsmap table.
		*  Call cgw-rest CreateGroup if fail then Delete entry from groupsmap.
		*/
#ifdef CGW_INTEGRATION
                uint64_t groupId = 0;
                if (!StorageService()->GroupsMapDB().AddVenue(NewObject.info.id, groupId)) {
                        poco_information(Logger(), "RESTAPI_venue_handler::DoPost groupsmap failure");
                        return InternalError(RESTAPI::Errors::RecordNotCreated);
                }
                if (!SDK::CGW::CreateGroup(groupId)) {
                        StorageService()->GroupsMapDB().DeleteVenue(NewObject.info.id);
                        poco_information(Logger(), "RESTAPI_venue_handler::DoPost CGW createGroup failure");
                        return InternalError(RESTAPI::Errors::RecordNotCreated);
                }
#endif
		if (DB_.CreateRecord(NewObject)) {
			MoveUsage(StorageService()->ContactDB(), DB_, {}, NewObject.contacts,
					  NewObject.info.id);
			MoveUsage(StorageService()->LocationDB(), DB_, "", NewObject.location,
					  NewObject.info.id);
			MoveUsage(StorageService()->PolicyDB(), DB_, "", NewObject.managementPolicy,
					  NewObject.info.id);
			ManageMembership(StorageService()->EntityDB(), &ProvObjects::Entity::venues, "",
							 NewObject.entity, NewObject.info.id);
			ManageMembership(StorageService()->VenueDB(), &ProvObjects::Venue::children, "",
							 NewObject.parent, NewObject.info.id);
			MoveUsage(StorageService()->ConfigurationDB(), DB_, {}, NewObject.deviceConfiguration,
					  NewObject.info.id);

			ProvObjects::Venue NewRecord;
			DB_.GetRecord("id", NewObject.info.id, NewRecord);
			Poco::JSON::Object Answer;
			NewRecord.to_json(Answer);
			return ReturnObject(Answer);
		}
		InternalError(RESTAPI::Errors::RecordNotCreated);
	}

	/*
	 *      Get all the device types for the devices in the venue
	 *      For each unique device type, get all the firmwares that apply for it
	 *      create the intersection of all resulting firmwares
	 *      create a list ordered in 3 types
	 *      - Releases
	 *      - RC of releases
	 *      - Dev
	 *
	 *      We obtain the revision by stripping and trimming the revision field previous to the "/"
	 *      The sets should be built on the revision and the URI of the images, all per devicetype
	 */

	enum class RevisionTypes { release, release_candidate, development };

	RevisionTypes RevisionIdentify(const std::string &r) {
		auto it = r.find_first_of('/');
		if (it == std::string::npos) {
			return RevisionTypes::development;
		}
		++it;
		if (r.find("TIP-devel-", it) != std::string::npos) {
			return RevisionTypes::development;
		}
		if (r.find("-rc", it) != std::string::npos) {
			return RevisionTypes::release_candidate;
		}
		if (r.find("TIP-v", it) != std::string::npos) {
			return RevisionTypes::release;
		}
		return RevisionTypes::development;
	}

	std::uint64_t RevisionDate(const std::string &R, const std::vector<FMSObjects::Firmware> &F) {

		for (const auto &f : F) {
			if (f.revision == R)
				return f.imageDate;
		}
		return 0;
	}

	void RESTAPI_venue_handler::DoPut() {
		std::string UUID = GetBinding("uuid", "");

		ProvObjects::Venue Existing;
		if (UUID.empty() || !DB_.GetRecord("id", UUID, Existing)) {
			return NotFound();
		}

		auto testUpdateOnly = GetBoolParameter("testUpdateOnly");
		if (testUpdateOnly) {
			ProvObjects::SerialNumberList SNL;
            StorageService()->InventoryDB().GetDevicesForVenue(UUID, SNL.serialNumbers);
			Poco::JSON::Object Answer;
			SNL.to_json(Answer);
			return ReturnObject(Answer);
		}

		if (GetBoolParameter("updateAllDevices")) {
			ProvObjects::SerialNumberList SNL;
            StorageService()->InventoryDB().GetDevicesForVenue(UUID, SNL.serialNumbers);

			Poco::JSON::Object Answer;
			auto JobId = MicroServiceCreateUUID();
			Types::StringVec Parameters{UUID};
			auto NewJob = new VenueConfigUpdater(JobId, "VenueConfigurationUpdater", Parameters, 0,
												 UserInfo_.userinfo, Logger());
			JobController()->AddJob(dynamic_cast<Job *>(NewJob));
			SNL.to_json(Answer);
			Answer.set("jobId", JobId);
			return ReturnObject(Answer);
		}

		if (GetBoolParameter("upgradeAllDevices")) {
			if (GetBoolParameter("revisionsAvailable")) {
				std::set<std::string> DeviceTypes;
                std::vector<ProvObjects::InventoryTag> ExistingDevices;
                StorageService()->InventoryDB().GetDevicesForVenue(UUID, ExistingDevices);
				for (const auto &device : ExistingDevices) {
                    DeviceTypes.insert(device.deviceType);
				}

				//  Get all the revisions for all the device types
				using FirmwareList = std::vector<FMSObjects::Firmware>;
				using FirmwareRevisions = std::set<std::string>;

				std::map<std::string, FirmwareList> AllFMs;
				FirmwareRevisions AllRevisions;
				bool first_pass = true;
				for (const auto &device_type : DeviceTypes) {
					FirmwareList list;
					if (SDK::FMS::Firmware::GetDeviceTypeFirmwares(device_type, list)) {
						AllFMs[device_type] = list;
						FirmwareRevisions DeviceRevisions;
						if (first_pass) {
							for (const auto &revision : list) {
								AllRevisions.insert(revision.revision);
							}
						} else {
							FirmwareRevisions DeviceTypeRevisions;
							for (const auto &revision : list) {
								DeviceTypeRevisions.insert(revision.revision);
							}
							FirmwareRevisions NewRevisions;
							std::set_intersection(
								AllRevisions.begin(), AllRevisions.end(),
								DeviceTypeRevisions.begin(), DeviceTypeRevisions.end(),
								std::inserter(NewRevisions, NewRevisions.begin()));
							AllRevisions = NewRevisions;
						}
					}
					first_pass = false;
				}

				//  OK we start the solution map with Our first device, and then we
				Poco::JSON::Array Releases, ReleaseCandidates, DevelReleases;
				for (const auto &revision : AllRevisions) {
					auto Date = RevisionDate(revision, AllFMs.begin()->second);
					switch (RevisionIdentify(revision)) {
					case RevisionTypes::release_candidate: {
						Poco::JSON::Object E;
						E.set("revision", revision);
						E.set("date", Date);
						ReleaseCandidates.add(E);
					} break;
					case RevisionTypes::release: {
						Poco::JSON::Object E;
						E.set("revision", revision);
						E.set("date", Date);
						Releases.add(E);
					} break;
					case RevisionTypes::development:
					default: {
						Poco::JSON::Object E;
						E.set("revision", revision);
						E.set("date", Date);
						DevelReleases.add(E);
					}
					}
				}
				Poco::JSON::Object Answer;
				Answer.set("releases", Releases);
				Answer.set("releasesCandidates", ReleaseCandidates);
				Answer.set("developmentReleases", DevelReleases);
				return ReturnObject(Answer);
			}

			auto Revision = GetParameter("revision", "");
			if (Revision.empty()) {
				return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
			}

            ProvObjects::SerialNumberList SNL;
            StorageService()->InventoryDB().GetDevicesForVenue(UUID, SNL.serialNumbers);

			Poco::JSON::Object Answer;
			auto JobId = MicroServiceCreateUUID();
			Types::StringVec Parameters{UUID, Revision};
			auto NewJob = new VenueUpgrade(JobId, "VenueFirmwareUpgrade", Parameters, 0,
										   UserInfo_.userinfo, Logger());
			JobController()->AddJob(dynamic_cast<Job *>(NewJob));
			SNL.to_json(Answer);
			Answer.set("jobId", JobId);
			return ReturnObject(Answer);
		}

		if (GetBoolParameter("rebootAllDevices")) {
			ProvObjects::SerialNumberList SNL;
            StorageService()->InventoryDB().GetDevicesForVenue(UUID, SNL.serialNumbers);

			Poco::JSON::Object Answer;
			auto JobId = MicroServiceCreateUUID();
			Types::StringVec Parameters{UUID};
			;
			auto NewJob = new VenueRebooter(JobId, "VenueRebooter", Parameters, 0,
											UserInfo_.userinfo, Logger());
			JobController()->AddJob(dynamic_cast<Job *>(NewJob));
			SNL.to_json(Answer);
			Answer.set("jobId", JobId);
			return ReturnObject(Answer);
		}

		const auto &RawObject = ParsedBody_;
		ProvObjects::Venue NewObject;
		if (!NewObject.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if ((RawObject->has("deviceRules") && !ValidDeviceRules(NewObject.deviceRules, *this))) {
			return;
		}

		if (!UpdateObjectInfo(RawObject, UserInfo_.userinfo, Existing.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		if (RawObject->has("deviceRules"))
			Existing.deviceRules = NewObject.deviceRules;

		if (RawObject->has("sourceIP")) {
			if (!NewObject.sourceIP.empty() && !CIDR::ValidateIpRanges(NewObject.sourceIP)) {
				return BadRequest(RESTAPI::Errors::InvalidIPRanges);
			}
			Existing.sourceIP = NewObject.sourceIP;
		}

		std::string MoveFromEntity, MoveToEntity;
		if (AssignIfPresent(RawObject, "entity", MoveToEntity)) {
			if (MoveToEntity.empty() || !StorageService()->EntityDB().Exists("id", MoveToEntity)) {
				return BadRequest(RESTAPI::Errors::EntityMustExist);
			}
			MoveFromEntity = Existing.entity;
			Existing.entity = MoveToEntity;
		}

		std::string MoveToVenue, MoveFromVenue;
		if (AssignIfPresent(RawObject, "venue", MoveToVenue)) {
			if (MoveToVenue.empty() || !StorageService()->VenueDB().Exists("id", MoveToVenue)) {
				return BadRequest(RESTAPI::Errors::VenueMustExist);
			}
			MoveFromVenue = Existing.parent;
			Existing.parent = MoveToVenue;
		}

		std::string MoveFromLocation, MoveToLocation;
		if (AssignIfPresent(RawObject, "location", MoveToLocation)) {
			if (MoveToLocation.empty() ||
				!StorageService()->LocationDB().Exists("id", MoveToLocation)) {
				return BadRequest(RESTAPI::Errors::LocationMustExist);
			}
			MoveFromLocation = Existing.location;
			Existing.location = MoveToLocation;
		}

		Types::UUIDvec_t MoveFromContacts, MoveToContacts;
		if (AssignIfPresent(RawObject, "contacts", MoveToContacts)) {
			for (const auto &contact : NewObject.contacts) {
				if (!StorageService()->ContactDB().Exists("id", contact)) {
					return BadRequest(RESTAPI::Errors::ContactMustExist);
				}
			}
			MoveFromContacts = Existing.contacts;
			Existing.contacts = MoveToContacts;
		}

		std::string MoveFromPolicy, MoveToPolicy;
		if (AssignIfPresent(RawObject, "managementPolicy", MoveToPolicy)) {
			if (MoveToPolicy.empty() || !StorageService()->PolicyDB().Exists("id", MoveToPolicy)) {
				return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
			}
			MoveFromPolicy = Existing.managementPolicy;
			Existing.managementPolicy = MoveToPolicy;
		}

		Types::UUIDvec_t MoveToConfigurations, MoveFromConfigurations;
		if (RawObject->has("deviceConfiguration")) {
			MoveToConfigurations = NewObject.deviceConfiguration;
			for (auto &configuration : MoveToConfigurations) {
				if (!StorageService()->ConfigurationDB().Exists("id", configuration)) {
					return BadRequest(RESTAPI::Errors::ConfigurationMustExist);
				}
			}
			MoveToConfigurations = NewObject.deviceConfiguration;
			MoveFromConfigurations = Existing.deviceConfiguration;
			Existing.deviceConfiguration = MoveToConfigurations;
		}

		std::string ErrorText;
		NewObject.parent = Existing.parent;
		NewObject.entity = Existing.entity;

		std::vector<std::string> Errors;
		auto ObjectsCreated = CreateObjects(NewObject, *this, Errors);
		if (!Errors.empty()) {
			return BadRequest(RESTAPI::Errors::ConfigBlockInvalid);
		}

		if (!ObjectsCreated.empty()) {
			if (!ObjectsCreated.empty()) {
				auto it = ObjectsCreated.find("location");
				if (it != ObjectsCreated.end()) {
					MoveFromLocation = "";
					MoveToLocation = it->second;
					Existing.location = MoveToLocation;
				}
			}
		}

		if (RawObject->has("boards")) {
			Existing.boards = NewObject.boards;
		}

		if (StorageService()->VenueDB().UpdateRecord("id", UUID, Existing)) {
			UpdateKafkaProvisioningObject(ProvisioningOperation::modification, Existing);
			MoveUsage(StorageService()->ContactDB(), DB_, MoveFromContacts, MoveToContacts,
					  Existing.info.id);
			MoveUsage(StorageService()->LocationDB(), DB_, MoveFromLocation, MoveToLocation,
					  Existing.info.id);
			MoveUsage(StorageService()->PolicyDB(), DB_, MoveFromPolicy, MoveToPolicy,
					  Existing.info.id);
			ManageMembership(StorageService()->EntityDB(), &ProvObjects::Entity::venues,
							 MoveFromEntity, MoveToEntity, Existing.info.id);
			ManageMembership(StorageService()->VenueDB(), &ProvObjects::Venue::children,
							 MoveFromVenue, MoveToVenue, Existing.info.id);
			MoveUsage(StorageService()->ConfigurationDB(), DB_, MoveFromConfigurations,
					  MoveToConfigurations, Existing.info.id);

			ProvObjects::Venue AddedRecord;
			DB_.GetRecord("id", UUID, AddedRecord);
			Poco::JSON::Object Answer;
			AddedRecord.to_json(Answer);
			return ReturnObject(Answer);
		}
		InternalError(RESTAPI::Errors::RecordNotUpdated);
	}
} // namespace OpenWifi