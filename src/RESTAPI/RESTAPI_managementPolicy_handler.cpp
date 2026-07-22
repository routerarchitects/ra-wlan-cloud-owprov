//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include "RESTAPI_managementPolicy_handler.h"
#include "Daemon.h"
#include "Poco/JSON/Parser.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"

namespace OpenWifi {

	void RESTAPI_managementPolicy_handler::DoGet() {
		std::string UUID = GetBinding("uuid", "");
		ProvObjects::ManagementPolicy Existing;
		if (UUID.empty() || !DB_.GetRecord("id", UUID, Existing)) {
			return NotFound();
		}

		std::string Arg;
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
			Poco::JSON::Object Answer;
			Answer.set("entries", Inner);
			return ReturnObject(Answer);
		}

		Poco::JSON::Object Answer;
		if (QB_.AdditionalInfo)
			AddExtendedInfo(Existing, Answer);

		Existing.to_json(Answer);
		ReturnObject(Answer);
	}

	void RESTAPI_managementPolicy_handler::DoDelete() {
		if (UserInfo_.userinfo.userRole != SecurityObjects::ROOT) {
			return UnAuthorized(RESTAPI::Errors::ACCESS_DENIED);
		}

		std::string UUID = GetBinding("uuid", "");
		ProvObjects::ManagementPolicy Existing;
		if (UUID.empty() || !DB_.GetRecord("id", UUID, Existing)) {
			return NotFound();
		}

		if (!StorageService()->PolicyDB().DeleteRecord("id", UUID)) {
			return InternalError(RESTAPI::Errors::CouldNotBeDeleted);
		}

		AuthCache::GetInstance()->Clear();
		return OK();
	}

	void RESTAPI_managementPolicy_handler::DoPost() {
		if (UserInfo_.userinfo.userRole != SecurityObjects::ROOT) {
			return UnAuthorized(RESTAPI::Errors::ACCESS_DENIED);
		}

		std::string UUID = GetBinding("uuid", "");
		if (UUID.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		ProvObjects::ManagementPolicy NewObject;
		const auto &RawObject = ParsedBody_;
		if (!NewObject.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if (!CreateObjectInfo(RawObject, UserInfo_.userinfo, NewObject.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		if (!NewObject.entity.empty() &&
			!StorageService()->EntityDB().Exists("id", NewObject.entity)) {
			return BadRequest(RESTAPI::Errors::EntityMustExist);
		}

		if (!NewObject.venue.empty() && !StorageService()->VenueDB().Exists("id", NewObject.venue)) {
			return BadRequest(RESTAPI::Errors::VenueMustExist);
		}

		NewObject.inUse.clear();
		if (DB_.CreateRecord(NewObject)) {
			AuthCache::GetInstance()->Clear();
			PolicyDB::RecordName AddedObject;
			DB_.GetRecord("id", NewObject.info.id, AddedObject);
			Poco::JSON::Object Answer;
			AddedObject.to_json(Answer);
			return ReturnObject(Answer);
		}
		InternalError(RESTAPI::Errors::RecordNotCreated);
	}

	void RESTAPI_managementPolicy_handler::DoPut() {
		if (UserInfo_.userinfo.userRole != SecurityObjects::ROOT) {
			return UnAuthorized(RESTAPI::Errors::ACCESS_DENIED);
		}

		std::string UUID = GetBinding("uuid", "");
		ProvObjects::ManagementPolicy Existing;
		if (UUID.empty() || !DB_.GetRecord("id", UUID, Existing)) {
			return NotFound();
		}

		ProvObjects::ManagementPolicy NewPolicy;
		const auto &RawObject = ParsedBody_;
		if (!NewPolicy.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if (!UpdateObjectInfo(RawObject, UserInfo_.userinfo, Existing.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		if (RawObject->has("entity")) {
			std::string TargetEntity = RawObject->get("entity").toString();
			if (!TargetEntity.empty() && !StorageService()->EntityDB().Exists("id", TargetEntity)) {
				return BadRequest(RESTAPI::Errors::EntityMustExist);
			}
			Existing.entity = TargetEntity;
		}

		if (RawObject->has("venue")) {
			std::string TargetVenue = RawObject->get("venue").toString();
			if (!TargetVenue.empty() && !StorageService()->VenueDB().Exists("id", TargetVenue)) {
				return BadRequest(RESTAPI::Errors::VenueMustExist);
			}
			Existing.venue = TargetVenue;
		}

		if (!NewPolicy.entries.empty()) {
			Existing.entries = NewPolicy.entries;
		}

		if (DB_.UpdateRecord("id", Existing.info.id, Existing)) {
			AuthCache::GetInstance()->Clear();
			ProvObjects::ManagementPolicy P;
			DB_.GetRecord("id", Existing.info.id, P);
			Poco::JSON::Object Answer;
			P.to_json(Answer);
			return ReturnObject(Answer);
		}
		InternalError(RESTAPI::Errors::RecordNotUpdated);
	}
} // namespace OpenWifi