//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include "RESTAPI_location_handler.h"
#include "Daemon.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"
#include "framework/utils.h"

namespace OpenWifi {

	void RESTAPI_location_handler::DoGet() {
		std::string UUID = GetBinding("uuid", "");
		ProvObjects::Location Existing;
		if (UUID.empty() || !DB_.GetRecord("id", UUID, Existing)) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		std::string Arg;
		Poco::JSON::Object Answer;
		if (HasParameter("expandInUse", Arg) && Arg == "true") {
			Storage::ExpandedListMap M;
			std::vector<std::string> Errors;
			Poco::JSON::Object Inner;
			if (StorageService()->ExpandInUse(Existing.inUse, M, Errors)) {
				for (const auto &[type, list] : M) {
					Poco::JSON::Array ObjList;
					for (const auto &i : list.entries) {
						Poco::JSON::Object O;
						i.to_json(O);
						ObjList.add(O);
					}
					Inner.set(type, ObjList);
				}
			}
			Answer.set("entries", Inner);
			return ReturnObject(Answer);
		} else if (QB_.AdditionalInfo) {
			AddExtendedInfo(Existing, Answer);
		}
		Existing.to_json(Answer);
		ReturnObject(Answer);
	}

	void RESTAPI_location_handler::DoDelete() {
		std::string UUID = GetBinding("uuid", "");
		ProvObjects::Location Existing;
		if (UUID.empty() || !DB_.GetRecord("id", UUID, Existing)) {
			return NotFound();
		}

		bool Force = false;
		std::string Arg;
		if (HasParameter("force", Arg) && Arg == "true")
			Force = true;

		if (!Force && !Existing.inUse.empty()) {
			return BadRequest(RESTAPI::Errors::StillInUse);
		}

		DB_.DeleteRecord("id", UUID);
		RemoveMembership(StorageService()->EntityDB(), &ProvObjects::Entity::locations,
						 Existing.entity, Existing.info.id);
		MoveUsage(StorageService()->PolicyDB(), DB_, Existing.info.id, "", Existing.info.id);
		return OK();
	}

	void RESTAPI_location_handler::DoPost() {
		std::string UUID = GetBinding(RESTAPI::Protocol::UUID, "");
		if (UUID.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		const auto &Obj = ParsedBody_;
		if (Obj->has("timezone")) {
			if (!Obj->get("timezone").isString()) {
				return BadRequest(RESTAPI::Errors::InvalidTimezone);
			}
			auto Timezone = Poco::trim(Obj->get("timezone").toString());
			if (!Utils::ValidTimeZone(Timezone)) {
				return BadRequest(RESTAPI::Errors::InvalidTimezone);
			}
		}

		ProvObjects::Location NewObject;
		if (!NewObject.from_json(Obj)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if (Obj->has("timezone")) {
			NewObject.timezone = Poco::trim(Obj->get("timezone").toString());
		}

		if (!ProvObjects::CreateObjectInfo(Obj, UserInfo_.userinfo, NewObject.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		if (NewObject.entity.empty() ||
			!StorageService()->EntityDB().Exists("id", NewObject.entity)) {
			return BadRequest(RESTAPI::Errors::EntityMustExist);
		}

		if (!NewObject.managementPolicy.empty() &&
			!StorageService()->PolicyDB().Exists("id", NewObject.managementPolicy)) {
			return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
		}

		NewObject.inUse.clear();

		if (DB_.CreateRecord(NewObject)) {
			MoveUsage(StorageService()->PolicyDB(), DB_, "", NewObject.managementPolicy,
					  NewObject.info.id);
			AddMembership(StorageService()->EntityDB(), &ProvObjects::Entity::locations,
						  NewObject.entity, NewObject.info.id);

			LocationDB::RecordName AddedRecord;
			DB_.GetRecord("id", NewObject.info.id, AddedRecord);
			Poco::JSON::Object Answer;
			NewObject.to_json(Answer);
			return ReturnObject(Answer);
		}
		BadRequest(RESTAPI::Errors::RecordNotCreated);
	}

	void RESTAPI_location_handler::DoPut() {

		std::string UUID = GetBinding(RESTAPI::Protocol::UUID);
		ProvObjects::Location Existing;
		if (UUID.empty() || !DB_.GetRecord("id", UUID, Existing)) {
			return NotFound();
		}

		const auto &RawObject = ParsedBody_;
		if (RawObject->has("timezone")) {
			if (!RawObject->get("timezone").isString()) {
				return BadRequest(RESTAPI::Errors::InvalidTimezone);
			}
			auto Timezone = Poco::trim(RawObject->get("timezone").toString());
			if (!Utils::ValidTimeZone(Timezone)) {
				return BadRequest(RESTAPI::Errors::InvalidTimezone);
			}
		}

		ProvObjects::Location NewObject;
		if (!NewObject.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if (RawObject->has("timezone")) {
			Existing.timezone = Poco::trim(RawObject->get("timezone").toString());
		}

		if (!UpdateObjectInfo(RawObject, UserInfo_.userinfo, Existing.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		std::string FromPolicy, ToPolicy;
		if (!CreateMove(RawObject, "managementPolicy", &LocationDB::RecordName::managementPolicy,
						Existing, FromPolicy, ToPolicy, StorageService()->PolicyDB()))
			return BadRequest(RESTAPI::Errors::EntityMustExist);

		std::string FromEntity, ToEntity;
		if (!CreateMove(RawObject, "entity", &LocationDB::RecordName::entity, Existing, FromEntity,
						ToEntity, StorageService()->EntityDB()))
			return BadRequest(RESTAPI::Errors::EntityMustExist);

		AssignIfPresent(RawObject, "buildingName", Existing.buildingName);
		AssignIfPresent(RawObject, "city", Existing.city);
		AssignIfPresent(RawObject, "state", Existing.state);
		AssignIfPresent(RawObject, "postal", Existing.postal);
		AssignIfPresent(RawObject, "country", Existing.country);
		AssignIfPresent(RawObject, "geoCode", Existing.geoCode);
		if (RawObject->has("addressLines"))
			Existing.addressLines = NewObject.addressLines;
		if (RawObject->has("phones"))
			Existing.phones = NewObject.phones;
		if (RawObject->has("mobiles"))
			Existing.mobiles = NewObject.mobiles;
		Existing.info.modified = Utils::Now();
		if (RawObject->has("type"))
			Existing.type = NewObject.type;

		if (DB_.UpdateRecord("id", UUID, Existing)) {
			MoveUsage(StorageService()->PolicyDB(), DB_, FromPolicy, ToPolicy, Existing.info.id);
			ManageMembership(StorageService()->EntityDB(), &ProvObjects::Entity::locations,
							 FromEntity, ToEntity, Existing.info.id);

			ProvObjects::Location NewObjectAdded;
			DB_.GetRecord("id", UUID, NewObjectAdded);
			Poco::JSON::Object Answer;
			NewObjectAdded.to_json(Answer);
			return ReturnObject(Answer);
		}
		InternalError(RESTAPI::Errors::RecordNotUpdated);
	}
} // namespace OpenWifi