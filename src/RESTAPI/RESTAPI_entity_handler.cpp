//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include "RESTAPI_entity_handler.h"

#include "RESTAPI_db_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "RESTObjects/RESTAPI_SecurityObjects.h"
#include "StorageService.h"

#include "framework/CIDR.h"

namespace OpenWifi {

	void RESTAPI_entity_handler::DoGet() {
		std::string UUID = GetBinding("uuid", "");
		ProvObjects::Entity Existing;
		if (UUID.empty() || !DB_.GetRecord("id", UUID, Existing)) {
			return NotFound();
		}

		Poco::JSON::Object Answer;
		Existing.to_json(Answer);
		if (NeedAdditionalInfo())
			AddExtendedInfo(Existing, Answer);
		ReturnObject(Answer);
	}

	void RESTAPI_entity_handler::DoDelete() {
		std::string UUID = GetBinding("uuid", "");
		ProvObjects::Entity Existing;
		if (UUID.empty() || !DB_.GetRecord("id", UUID, Existing)) {
			return NotFound();
		}

		if (UUID == EntityDB::RootUUID()) {
			return BadRequest(RESTAPI::Errors::CannotDeleteRoot);
		}

		if (!Existing.operatorId.empty()) {
			return BadRequest(RESTAPI::Errors::StillInUse);
		}

		if (!Existing.children.empty() || !Existing.devices.empty() || !Existing.venues.empty() ||
			!Existing.locations.empty() || !Existing.contacts.empty() ||
			!Existing.configurations.empty()) {
			return BadRequest(RESTAPI::Errors::StillInUse);
		}

		MoveUsage(StorageService()->PolicyDB(), DB_, Existing.managementPolicy, "",
				  Existing.info.id);
		DB_.DeleteRecord("id", UUID);
		DB_.DeleteChild("id", Existing.parent, UUID);
		return OK();
	}

	void RESTAPI_entity_handler::DoPost() {
		std::string UUID = GetBinding("uuid", "");
		if (UUID.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		if (UUID == EntityDB::RootUUID()) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
		}

		const auto &RawObject = ParsedBody_;
		ProvObjects::Entity NewEntity;
		if (!NewEntity.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if ((RawObject->has("deviceRules") && !ValidDeviceRules(NewEntity.deviceRules, *this))) {
			return;
		}

		if (!ProvObjects::CreateObjectInfo(RawObject, UserInfo_.userinfo, NewEntity.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		//  When creating an entity, it cannot have any relations other that parent, notes, name,
		//  description. Everything else must be conveyed through PUT.
		NewEntity.info.id = (UUID == EntityDB::RootUUID()) ? UUID : MicroServiceCreateUUID();

		if (UUID == EntityDB::RootUUID()) {
			NewEntity.parent = "";
		} else if (NewEntity.parent.empty() || !DB_.Exists("id", NewEntity.parent)) {
			return BadRequest(RESTAPI::Errors::ParentUUIDMustExist);
		}

		if (!NewEntity.managementPolicy.empty() &&
			!StorageService()->PolicyDB().Exists("id", NewEntity.managementPolicy)) {
			return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
		}

		if (!NewEntity.sourceIP.empty() && !CIDR::ValidateIpRanges(NewEntity.sourceIP)) {
			return BadRequest(RESTAPI::Errors::InvalidIPRanges);
		}

		NewEntity.venues.clear();
		NewEntity.children.clear();
		NewEntity.contacts.clear();
		NewEntity.locations.clear();
		NewEntity.deviceConfiguration.clear();
		NewEntity.managementRoles.clear();
		NewEntity.operatorId.clear();

		if (DB_.CreateRecord(NewEntity)) {
			MoveUsage(StorageService()->PolicyDB(), DB_, "", NewEntity.managementPolicy,
					  NewEntity.info.id);
			DB_.AddChild("id", NewEntity.parent, NewEntity.info.id);

			Poco::JSON::Object Answer;
			NewEntity.to_json(Answer);
			return ReturnObject(Answer);
		}
		InternalError(RESTAPI::Errors::RecordNotCreated);
	}

	/*
	 * Put is a complex operation, it contains commands only.
	 *      addContact=UUID, delContact=UUID,
	 *      addLocation=UUID, delLocation=UUID,
	 *      addVenue=UUID, delVenue=UUID,
	 *      addEntity=UUID, delEntity=UUID
	 *      addDevice=UUID, delDevice=UUID
	 */

	void RESTAPI_entity_handler::DoPut() {
		std::string UUID = GetBinding("uuid", "");
		ProvObjects::Entity Existing;
		if (UUID.empty() || !DB_.GetRecord("id", UUID, Existing)) {
			return NotFound();
		}

		const auto &RawObject = ParsedBody_;
		ProvObjects::Entity NewEntity;
		if (!NewEntity.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if ((RawObject->has("deviceRules") && !ValidDeviceRules(NewEntity.deviceRules, *this))) {
			return;
		}

		if (!UpdateObjectInfo(RawObject, UserInfo_.userinfo, Existing.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		std::string FromPolicy, ToPolicy;
		if (!CreateMove(RawObject, "managementPolicy", &EntityDB::RecordName::managementPolicy,
						Existing, FromPolicy, ToPolicy, StorageService()->PolicyDB()))
			return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);

		if (RawObject->has("sourceIP")) {
			if (!NewEntity.sourceIP.empty() && !CIDR::ValidateIpRanges(NewEntity.sourceIP)) {
				return BadRequest(RESTAPI::Errors::InvalidIPRanges);
			}
			Existing.sourceIP = NewEntity.sourceIP;
		}

		RESTAPI::Errors::msg Error;
		if (!StorageService()->Validate(Parameters_, Error)) {
			return BadRequest(Error);
		}

		if (RawObject->has("deviceRules"))
			Existing.deviceRules = NewEntity.deviceRules;

		if (DB_.UpdateRecord("id", UUID, Existing)) {
			MoveUsage(StorageService()->PolicyDB(), DB_, FromPolicy, ToPolicy, Existing.info.id);

			Poco::JSON::Object Answer;
			ProvObjects::Entity NewRecord;
			StorageService()->EntityDB().GetRecord("id", UUID, NewRecord);
			NewRecord.to_json(Answer);
			return ReturnObject(Answer);
		}
		InternalError(RESTAPI::Errors::RecordNotUpdated);
	}
} // namespace OpenWifi
