//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include "RESTAPI_configurations_handler.h"
#include "DeviceTypeCache.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"
#include "framework/ConfigurationValidator.h"

namespace OpenWifi {
	namespace {
		inline ConfigurationValidator::ConfigurationType ResolveValidationType(
			const Poco::JSON::Object::Ptr &rawObject, const std::string &fallbackType) {
			std::string validationHint = fallbackType;
			if (rawObject && rawObject->has("deviceGroup")) {
				validationHint = rawObject->getValue<std::string>("deviceGroup");
			}
			return ConfigurationValidator::GetType(validationHint);
		}
	} // namespace

	void RESTAPI_configurations_handler::DoGet() {
		std::string UUID = GetBinding("uuid", "");
		ProvObjects::DeviceConfiguration Existing;
		if (UUID.empty() || !DB_.GetRecord("id", UUID, Existing)) {
			return NotFound();
		}

		Poco::JSON::Object Answer;
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
			Answer.set("entries", Inner);
			return ReturnObject(Answer);
		} else if (HasParameter("computedAffected", Arg) && Arg == "true") {
			Types::UUIDvec_t DeviceSerialNumbers;
			DB_.GetListOfAffectedDevices(UUID, DeviceSerialNumbers);
			return ReturnObject("affectedDevices", DeviceSerialNumbers);
		} else if (QB_.AdditionalInfo) {
			AddExtendedInfo(Existing, Answer);
		}
		Existing.to_json(Answer);
		ReturnObject(Answer);
	}

	void RESTAPI_configurations_handler::DoDelete() {
		std::string UUID = GetBinding("uuid", "");
		ProvObjects::DeviceConfiguration Existing;
		if (UUID.empty() || !DB_.GetRecord("id", UUID, Existing)) {
			return NotFound();
		}

		if (!Existing.inUse.empty()) {
			return BadRequest(RESTAPI::Errors::StillInUse);
		}

		DB_.DeleteRecord("id", UUID);
		MoveUsage(StorageService()->PolicyDB(), DB_, Existing.managementPolicy, "",
				  Existing.info.id);
		RemoveMembership(StorageService()->VenueDB(), &ProvObjects::Venue::configurations,
						 Existing.venue, Existing.info.id);
		RemoveMembership(StorageService()->EntityDB(), &ProvObjects::Entity::configurations,
						 Existing.entity, Existing.info.id);
		for (const auto &i : Existing.variables)
			RemoveMembership(StorageService()->VariablesDB(),
							 &ProvObjects::VariableBlock::configurations, i, Existing.info.id);

		return OK();
	}

	void RESTAPI_configurations_handler::DoPost() {
		auto UUID = GetBinding("uuid", "");
		if (UUID.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		const auto &RawObject = ParsedBody_;
		std::string Arg;
		if (HasParameter("validateOnly", Arg) && Arg == "true") {
			if (!RawObject->has("configuration")) {
				return BadRequest(RESTAPI::Errors::MustHaveConfigElement);
			}
			auto Config = RawObject->get("configuration").toString();
			Poco::JSON::Object Answer;
            auto deviceType = GetParameter("deviceType", "AP");
            std::string Error;
			auto Res =
				ValidateUCentralConfiguration(ConfigurationValidator::GetType(deviceType),Config, Error, GetBoolParameter("strict", true));
			Answer.set("valid", Res);
			Answer.set("error", Error);
			return ReturnObject(Answer);
		}

		ProvObjects::DeviceConfiguration NewObject;
		if (!NewObject.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if ((RawObject->has("deviceRules") && !ValidDeviceRules(NewObject.deviceRules, *this))) {
			return;
		}

		if (!ProvObjects::CreateObjectInfo(RawObject, UserInfo_.userinfo, NewObject.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		if (!NewObject.entity.empty() &&
			!StorageService()->EntityDB().Exists("id", NewObject.entity)) {
			return BadRequest(RESTAPI::Errors::EntityMustExist);
		}

		if (!NewObject.venue.empty() &&
			!StorageService()->VenueDB().Exists("id", NewObject.venue)) {
			return BadRequest(RESTAPI::Errors::VenueMustExist);
		}

		if (!NewObject.managementPolicy.empty() &&
			!StorageService()->PolicyDB().Exists("id", NewObject.managementPolicy)) {
			return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
		}

		NewObject.inUse.clear();
		if (NewObject.deviceTypes.empty() ||
			!DeviceTypeCache()->AreAcceptableDeviceTypes(NewObject.deviceTypes, true)) {
			return BadRequest(RESTAPI::Errors::InvalidDeviceTypes);
		}

		std::vector<std::string> Errors;
		auto validationType = ResolveValidationType(RawObject, GetParameter("deviceType", "AP"));
		if (!ValidateConfigBlock(validationType, NewObject, Errors)) {
			for (const auto &Error : Errors) {
				poco_warning(Logger(), "Configuration validation failed: " + Error);
			}
			return BadRequest(RESTAPI::Errors::ConfigBlockInvalid);
		}

		Types::UUIDvec_t ToVariables;
		if (RawObject->has("variables")) {
			for (const auto &i : NewObject.variables) {
				if (!i.empty() && !StorageService()->VariablesDB().Exists("id", i)) {
					return BadRequest(RESTAPI::Errors::VariableMustExist);
				}
			}
			for (const auto &i : NewObject.variables)
				ToVariables.emplace_back(i);
			
			ToVariables = NewObject.variables;
		}

		if (DB_.CreateRecord(NewObject)) {
			AddMembership(StorageService()->VariablesDB(),
							 &ProvObjects::VariableBlock::configurations, ToVariables, NewObject.info.id);
			MoveUsage(StorageService()->PolicyDB(), DB_, "", NewObject.managementPolicy,
					  NewObject.info.id);
			AddMembership(StorageService()->VenueDB(), &ProvObjects::Venue::configurations,
						  NewObject.venue, NewObject.info.id);
			AddMembership(StorageService()->EntityDB(), &ProvObjects::Entity::configurations,
						  NewObject.entity, NewObject.info.id);

			ConfigurationDB::RecordName AddedRecord;
			DB_.GetRecord("id", NewObject.info.id, AddedRecord);
			Poco::JSON::Object Answer;
			AddedRecord.to_json(Answer);
			return ReturnObject(Answer);
		}
		InternalError(RESTAPI::Errors::RecordNotCreated);
	}

	void RESTAPI_configurations_handler::DoPut() {
		auto UUID = GetBinding("uuid", "");
		ProvObjects::DeviceConfiguration Existing;
		if (UUID.empty() || !DB_.GetRecord("id", UUID, Existing)) {
			return NotFound();
		}

		ProvObjects::DeviceConfiguration NewObject;
		const auto &RawObject = ParsedBody_;
		if (!NewObject.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if ((RawObject->has("deviceRules") && !ValidDeviceRules(NewObject.deviceRules, *this))) {
			return;
		}

		if (!UpdateObjectInfo(RawObject, UserInfo_.userinfo, Existing.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		if (!NewObject.deviceTypes.empty() &&
			!DeviceTypeCache()->AreAcceptableDeviceTypes(NewObject.deviceTypes, true)) {
			return BadRequest(RESTAPI::Errors::InvalidDeviceTypes);
		}

		if (!NewObject.deviceTypes.empty())
			Existing.deviceTypes = NewObject.deviceTypes;

		std::vector<std::string> Errors;
		auto validationType = ResolveValidationType(RawObject, GetParameter("deviceType", "AP"));
		if (!ValidateConfigBlock(validationType, NewObject, Errors)) {
			for (const auto &Error : Errors) {
				poco_warning(Logger(), "Configuration validation failed: " + Error);
			}
			return BadRequest(RESTAPI::Errors::ConfigBlockInvalid);
		}

		if (RawObject->has("configuration")) {
			Existing.configuration = NewObject.configuration;
		}

		std::string FromPolicy, ToPolicy;
		if (!CreateMove(RawObject, "managementPolicy",
						&ConfigurationDB::RecordName::managementPolicy, Existing, FromPolicy,
						ToPolicy, StorageService()->PolicyDB()))
			return BadRequest(RESTAPI::Errors::EntityMustExist);

		std::string FromEntity, ToEntity;
		if (!CreateMove(RawObject, "entity", &ConfigurationDB::RecordName::entity, Existing,
						FromEntity, ToEntity, StorageService()->EntityDB()))
			return BadRequest(RESTAPI::Errors::EntityMustExist);

		std::string FromVenue, ToVenue;
		if (!CreateMove(RawObject, "venue", &ConfigurationDB::RecordName::venue, Existing,
						FromVenue, ToVenue, StorageService()->VenueDB()))
			return BadRequest(RESTAPI::Errors::VenueMustExist);

		Types::UUIDvec_t FromVariables, ToVariables;
		if (RawObject->has("variables")) {
			for (const auto &i : NewObject.variables) {
				if (!i.empty() && !StorageService()->VariablesDB().Exists("id", i)) {
					return BadRequest(RESTAPI::Errors::VariableMustExist);
				}
			}
			for (const auto &i : Existing.variables)
				FromVariables.emplace_back(i);
			for (const auto &i : NewObject.variables)
				ToVariables.emplace_back(i);
			FromVariables = Existing.variables;
			ToVariables = NewObject.variables;
			Existing.variables = ToVariables;
		}

		if (RawObject->has("deviceRules"))
			Existing.deviceRules = NewObject.deviceRules;

		if (DB_.UpdateRecord("id", UUID, Existing)) {
			ManageMembership(StorageService()->VariablesDB(),
							 &ProvObjects::VariableBlock::configurations, FromVariables,
							 ToVariables, Existing.info.id);
			ManageMembership(StorageService()->VenueDB(), &ProvObjects::Venue::configurations,
							 FromVenue, ToVenue, Existing.info.id);
			ManageMembership(StorageService()->EntityDB(), &ProvObjects::Entity::configurations,
							 FromEntity, ToEntity, Existing.info.id);
			MoveUsage(StorageService()->PolicyDB(), DB_, FromPolicy, ToPolicy, Existing.info.id);

			ProvObjects::DeviceConfiguration D;
			DB_.GetRecord("id", UUID, D);
			Poco::JSON::Object Answer;
			D.to_json(Answer);
			return ReturnObject(Answer);
		}
		InternalError(RESTAPI::Errors::RecordNotUpdated);
	}
} // namespace OpenWifi
