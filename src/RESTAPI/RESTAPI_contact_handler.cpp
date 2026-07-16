//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include "RESTAPI_contact_handler.h"

#include "RESTAPI_db_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "framework/ow_constants.h"

namespace OpenWifi {
	void RESTAPI_contact_handler::DoGet() {

		std::string UUID = GetBinding("uuid", "");
		ProvObjects::Contact Existing;
		if (UUID.empty() || !DB_.GetRecord("id", UUID, Existing)) {
			return NotFound();
		}
		RBAC::TargetScope scope;
		if (RBAC::ResolveContactScope(UUID, scope)) {
			if (!RBAC::RequireAccess(*this, "contact", "READ", scope)) {
				return;
			}
		} else if (!RBAC::IsRootUser(*this)) {
			return UnAuthorized(RESTAPI::Errors::ACCESS_DENIED);
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
		} else if (QB_.AdditionalInfo) {
			AddExtendedInfo(Existing, Answer);
		}

		Existing.to_json(Answer);
		ReturnObject(Answer);
	}

	void RESTAPI_contact_handler::DoDelete() {

		std::string UUID = GetBinding("uuid", "");
		ProvObjects::Contact Existing;
		if (UUID.empty() || !DB_.GetRecord("id", UUID, Existing)) {
			return NotFound();
		}
		RBAC::TargetScope scope;
		if (RBAC::ResolveContactScope(UUID, scope)) {
			if (!RBAC::RequireAccess(*this, "contact", "DELETE", scope)) {
				return;
			}
		} else if (!RBAC::IsRootUser(*this)) {
			return UnAuthorized(RESTAPI::Errors::ACCESS_DENIED);
		}

		bool Force = false;
		std::string Arg;
		if (HasParameter("force", Arg) && Arg == "true")
			Force = true;

		if (!Force && !Existing.inUse.empty()) {
			return BadRequest(RESTAPI::Errors::StillInUse);
		}

		DB_.DeleteRecord("id", UUID);
		RemoveMembership(StorageService()->EntityDB(), &ProvObjects::Entity::contacts,
						 Existing.entity, Existing.info.id);
		MoveUsage(StorageService()->PolicyDB(), DB_, Existing.info.id, "", Existing.info.id);
		return OK();
	}

	void RESTAPI_contact_handler::DoPost() {
		std::string UUID = GetBinding(RESTAPI::Protocol::UUID, "");

		if (UUID.empty()) {
			return BadRequest(RESTAPI::Errors::MissingUUID);
		}

		const auto &Obj = ParsedBody_;
		ProvObjects::Contact NewObject;
		if (!NewObject.from_json(Obj)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		RBAC::TargetScope scope;
		if (!NewObject.entity.empty()) {
			scope.entity = NewObject.entity;
		} else if (RBAC::ResolveManagementPolicyScope(NewObject.managementPolicy, scope)) {
			// scope resolved from the attached policy
		}
		if (!scope.entity.empty() || !scope.venue.empty()) {
			if (!RBAC::RequireAccess(*this, "contact", "CREATE", scope)) {
				return;
			}
		} else if (!RBAC::IsRootUser(*this)) {
			return UnAuthorized(RESTAPI::Errors::ACCESS_DENIED);
		}

		if (!ProvObjects::CreateObjectInfo(Obj, UserInfo_.userinfo, NewObject.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		if (NewObject.entity.empty() &&
			!StorageService()->EntityDB().Exists("id", NewObject.entity)) {
			return BadRequest(RESTAPI::Errors::EntityMustExist);
		}

		if (!NewObject.managementPolicy.empty() &&
			!StorageService()->PolicyDB().Exists("id", NewObject.managementPolicy)) {
			return BadRequest(RESTAPI::Errors::UnknownManagementPolicyUUID);
		}

		NewObject.inUse.clear();

		if (DB_.CreateRecord(NewObject)) {
			AddMembership(StorageService()->EntityDB(), &ProvObjects::Entity::contacts,
						  NewObject.entity, NewObject.info.id);
			MoveUsage(StorageService()->PolicyDB(), DB_, "", NewObject.managementPolicy,
					  NewObject.info.id);

			ProvObjects::Contact NewContact;
			StorageService()->ContactDB().GetRecord("id", NewObject.info.id, NewContact);

			Poco::JSON::Object Answer;
			NewContact.to_json(Answer);
			ReturnObject(Answer);
			return;
		}
		InternalError(RESTAPI::Errors::RecordNotCreated);
	}

	void RESTAPI_contact_handler::DoPut() {
		std::string UUID = GetBinding(RESTAPI::Protocol::UUID, "");
		ProvObjects::Contact Existing;
		if (UUID.empty() || !DB_.GetRecord("id", UUID, Existing)) {
			return NotFound();
		}
		RBAC::TargetScope scope;
		if (RBAC::ResolveContactScope(UUID, scope)) {
			if (!RBAC::RequireAccess(*this, "contact", "UPDATE", scope)) {
				return;
			}
		} else if (!RBAC::IsRootUser(*this)) {
			return UnAuthorized(RESTAPI::Errors::ACCESS_DENIED);
		}

		const auto &RawObject = ParsedBody_;
		ProvObjects::Contact NewObject;
		if (!NewObject.from_json(RawObject)) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		if (!UpdateObjectInfo(RawObject, UserInfo_.userinfo, Existing.info)) {
			return BadRequest(RESTAPI::Errors::NameMustBeSet);
		}

		std::string FromPolicy, ToPolicy;
		if (!CreateMove(RawObject, "managementPolicy", &ContactDB::RecordName::managementPolicy,
						Existing, FromPolicy, ToPolicy, StorageService()->PolicyDB()))
			return BadRequest(RESTAPI::Errors::EntityMustExist);

		std::string FromEntity, ToEntity;
		if (!CreateMove(RawObject, "entity", &ContactDB::RecordName::entity, Existing, FromEntity,
						ToEntity, StorageService()->EntityDB()))
			return BadRequest(RESTAPI::Errors::EntityMustExist);

		AssignIfPresent(RawObject, "title", Existing.title);
		AssignIfPresent(RawObject, "salutation", Existing.salutation);
		AssignIfPresent(RawObject, "firstname", Existing.firstname);
		AssignIfPresent(RawObject, "lastname", Existing.lastname);
		AssignIfPresent(RawObject, "initials", Existing.initials);
		AssignIfPresent(RawObject, "visual", Existing.visual);
		AssignIfPresent(RawObject, "primaryEmail", Existing.primaryEmail);
		AssignIfPresent(RawObject, "secondaryEmail", Existing.secondaryEmail);
		AssignIfPresent(RawObject, "accessPIN", Existing.accessPIN);
		if (RawObject->has("type"))
			Existing.type = NewObject.type;
		if (RawObject->has("mobiles"))
			Existing.mobiles = NewObject.mobiles;
		if (RawObject->has("phones"))
			Existing.phones = NewObject.phones;

		if (DB_.UpdateRecord("id", UUID, Existing)) {
			MoveUsage(StorageService()->PolicyDB(), DB_, FromPolicy, ToPolicy, Existing.info.id);
			ManageMembership(StorageService()->EntityDB(), &ProvObjects::Entity::contacts,
							 FromEntity, ToEntity, Existing.info.id);

			ProvObjects::Contact NewObjectAdded;
			DB_.GetRecord("id", UUID, NewObjectAdded);
			Poco::JSON::Object Answer;
			NewObjectAdded.to_json(Answer);
			return ReturnObject(Answer);
		}
		InternalError(RESTAPI::Errors::RecordNotUpdated);
	}
} // namespace OpenWifi
