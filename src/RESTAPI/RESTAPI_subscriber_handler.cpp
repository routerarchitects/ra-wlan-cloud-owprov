/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
//
// Created by stephane bourque on 2022-02-20.
//

#include "RESTAPI_subscriber_handler.h"
#include "Poco/String.h"
#include "Signup.h"
#include "StorageService.h"
#include "framework/MicroServiceFuncs.h"
#include "framework/MicroServiceNames.h"
#include "framework/OpenAPIRequests.h"
#include "framework/orm.h"
#include "framework/utils.h"
#include "sdks/SDK_sec.h"

namespace OpenWifi {

	/*
		1) Record notExists + resend=true/false -> Create new UUID and call Security
		2) Record exists(waiting-for-email-verification) + resend=true -> Reuse existing UUID and call Security
		3) Record exists(waiting-for-email-verification) + resend=false -> Return existing waiting-for-email response
		4) Record exists(emailVerified) + resend=true/false -> UserAlreadyExists
	*/
	void RESTAPI_subscriber_handler::DoPost() {
		auto norm = [](std::string s) {
			Poco::toLowerInPlace(s);
			Poco::trimInPlace(s);
			return s;
		};

		const auto UserName       = norm(GetParameter("email"));
		const auto registrationId = norm(GetParameter("registrationId"));
		const bool resend         = GetBoolParameter("resend", false);

		if (UserName.empty() || registrationId.empty()) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
		}
		if (!Utils::ValidEMailAddress(UserName)) {
			return BadRequest(RESTAPI::Errors::InvalidEmailAddress);
		}

		Logger_.information(fmt::format(
			"SIGNUP: Signup request for '{}' reg='{}' resend={}",
			UserName, registrationId, resend));

		ProvObjects::Operator SignupOperator;
		if (!StorageService()->OperatorDB().GetRecord("registrationId", registrationId, SignupOperator)) {
			return BadRequest(RESTAPI::Errors::InvalidRegistrationOperatorName);
		}
		if (SignupOperator.entityId.empty() ||
			!StorageService()->EntityDB().Exists("id", SignupOperator.entityId)) {
			return BadRequest(RESTAPI::Errors::EntityMustExist);
		}

		// Lookup existing signup entry by email and enforce state-machine semantics.
		ProvObjects::SignupEntry existing{};
		const bool foundByEmail = StorageService()->SignupDB().GetRecord("email", UserName, existing);
		const bool waitingForEmailVerification =
			foundByEmail &&
			(existing.statusCode == ProvObjects::SignupStatusCodes::SignupWaitingForEmail);

		if (foundByEmail && !waitingForEmailVerification) {
			poco_information(Logger(), fmt::format(
										 "SIGNUP: rejecting duplicate signup for '{}' reg='{}' "
										 "resend={} status='{}' statusCode={}",
										 UserName, registrationId, resend, existing.status,
										 existing.statusCode));
			return BadRequest(RESTAPI::Errors::UserAlreadyExists);
		}

		// Idempotent behavior while waiting for email verification:
		// repeated POST without resend returns existing signup as-is.
		if (waitingForEmailVerification && !resend) {
			poco_information(Logger(), fmt::format(
										 "SIGNUP: idempotent pending signup for '{}' reg='{}' "
										 "returning existing signup '{}'",
										 UserName, registrationId, existing.info.id));
			Poco::JSON::Object ExistingAnswer;
			existing.to_json(ExistingAnswer);
			return ReturnObject(ExistingAnswer);
		}

		const bool reuseExisting = resend && waitingForEmailVerification;

		// Reuse existing signup UUID for resend; otherwise generate a new one.
		const auto SignupUUID = reuseExisting ? existing.info.id : MicroServiceCreateUUID();

		poco_debug(Logger(), fmt::format(
			"SIGNUP: {} request for '{}' reg='{}' uuid='{}'",
			reuseExisting ? "Resend(reuse)" : "Create(new)",
			UserName, registrationId, SignupUUID));

		// Forward request to security
		Poco::JSON::Object Body;
		OpenAPIRequestPost CreateUser(
			uSERVICE_SECURITY,
			"/api/v1/signup",
			{{"email", UserName},
			{"signupUUID", SignupUUID},
			{"owner", SignupOperator.info.id},
			{"operatorName", SignupOperator.registrationId},
			{"resend", resend ? "true" : "false"}},
			Body,
			30000);

		Poco::JSON::Object::Ptr Answer;
		auto status = CreateUser.Do(Answer);

		if (status != Poco::Net::HTTPServerResponse::HTTP_OK) {
			// Pass through OWSEC's HTTP status and JSON body
			Response->setStatus(status);

			std::stringstream ss;
			Poco::JSON::Stringifier::condense(Answer, ss);

			Response->setContentType("application/json");
			Response->setContentLength(ss.str().size());
			auto &os = Response->send();
			os << ss.str();
			return;
		}

		SecurityObjects::UserInfo UI;
		UI.from_json(Answer);

		// Defensive guard for legacy/non-normalized records: never create a second
		// signup row for the same subscriber userId.
		if (!reuseExisting) {
			ProvObjects::SignupEntry byUserId{};
			if (StorageService()->SignupDB().GetRecord("userid", UI.id, byUserId)) {
				if (byUserId.statusCode == ProvObjects::SignupStatusCodes::SignupWaitingForEmail) {
					poco_warning(Logger(), fmt::format(
										   "SIGNUP: found existing pending signup by userId='{}' "
										   "signup='{}', returning existing row",
										   UI.id, byUserId.info.id));
					Poco::JSON::Object ExistingByUserIdAnswer;
					byUserId.to_json(ExistingByUserIdAnswer);
					return ReturnObject(ExistingByUserIdAnswer);
				}
				poco_warning(Logger(), fmt::format(
									   "SIGNUP: preventing duplicate signup row for userId='{}' "
									   "existingSignup='{}' incomingSignup='{}'",
									   UI.id, byUserId.info.id, SignupUUID));
				return BadRequest(RESTAPI::Errors::UserAlreadyExists);
			}
		}

		// Build the SignupEntry to return, then persist it (update vs create)
		ProvObjects::SignupEntry se = reuseExisting ? existing : ProvObjects::SignupEntry{};

		const auto now = Utils::Now();
		if (!reuseExisting) {
			se.info.id = SignupUUID;
			se.info.created = se.info.modified = se.submitted = now;
			se.completed = 0;
			se.error = 0;
			se.userId = UI.id;
			se.email = UserName;
			se.registrationId = registrationId;
			se.status = "waiting-for-email-verification";
			se.operatorId = SignupOperator.info.id;
			se.statusCode = ProvObjects::SignupStatusCodes::SignupWaitingForEmail;
		}

		if (reuseExisting) {
			StorageService()->SignupDB().UpdateRecord("id", se.info.id, se);
		} else {
			StorageService()->SignupDB().CreateRecord(se);
			Signup()->AddOutstandingSignup(se);
		}

		// Ensure a subscriber-scoped venue exists under the operator's entity.
		if (!StorageService()->VenueDB().DoesVenueNameAlreadyExist(
				UserName, SignupOperator.entityId, "")) {
			ProvObjects::Venue SubscriberVenue;
			ProvObjects::CreateObjectInfo(UserInfo_.userinfo, SubscriberVenue.info);
			SubscriberVenue.info.name = UserName;
			SubscriberVenue.entity = SignupOperator.entityId;
			SubscriberVenue.subscriber = se.userId;
			if (!StorageService()->VenueDB().CreateRecord(SubscriberVenue)) {
				return InternalError(RESTAPI::Errors::RecordNotCreated);
			}
			StorageService()->EntityDB().AddVenue("id", SubscriberVenue.entity,
												  SubscriberVenue.info.id);
		}

		Poco::JSON::Object SEAnswer;
		se.to_json(SEAnswer);
		return ReturnObject(SEAnswer);
	}

	//  this will be called by the SEC backend once the password has been verified.
	void RESTAPI_subscriber_handler::DoPut() {
		auto SignupUUID = GetParameter("signupUUID");
		auto Operation = GetParameter("operation");
		auto UserId = GetParameter("userId");

		poco_information(Logger(), fmt::format("signup-progress: {} - {} ", SignupUUID, Operation));
		if ((SignupUUID.empty() && UserId.empty()) || Operation.empty()) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
		}

		ProvObjects::SignupEntry SE;
		poco_information(Logger(), fmt::format("signup-progress: {} - {} fetching entry",
											   SignupUUID, Operation));
		if (!StorageService()->SignupDB().GetRecord("id", SignupUUID, SE) &&
			!StorageService()->SignupDB().GetRecord("userid", UserId, SE)) {
			return NotFound();
		}

		poco_debug(Logger(),
				   fmt::format("signup-progress: {} - {} fetching entry", SignupUUID, Operation));
		if (Operation == "emailVerified" &&
			SE.statusCode == ProvObjects::SignupStatusCodes::SignupWaitingForEmail) {
			poco_information(Logger(), fmt::format("{}: email {} verified.", SE.info.id, SE.email));
			std::cout << "Verified email for : " << SE.email << std::endl;
			SE.info.modified = Utils::Now();
			SE.status = "emailVerified";
			SE.statusCode = ProvObjects::SignupStatusCodes::SignupWaitingForDevice;
			StorageService()->SignupDB().UpdateRecord("id", SE.info.id, SE);
			Signup()->AddOutstandingSignup(SE);
			Poco::JSON::Object Answer;
			SE.to_json(Answer);
			return ReturnObject(Answer);
		}
		/*
		 - For updateMac operation, the mac parameter must be present.
		 - An empty value (mac="") is allowed and denotes deletion of macAddress from table.
		*/
		if (Operation == "updateMac") {
			std::string macAddress;
			if (!HasParameter("mac", macAddress)) {
				return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
			}
			poco_information(Logger(),fmt::format("Updating Signup device to [{}] for subscriber: [{}].", macAddress, SE.email));
			SE.macAddress = macAddress;
			SE.serialNumber = macAddress;
			SE.info.modified = Utils::Now();
			StorageService()->SignupDB().UpdateRecord("id", SE.info.id, SE);
			Poco::JSON::Object Answer;
			SE.to_json(Answer);
			return ReturnObject(Answer);
		}
		poco_information(Logger(), fmt::format("signup-progress: {} - {} something is bad",
											   SignupUUID, Operation));

		return BadRequest(RESTAPI::Errors::UnknownId);
	}

	void RESTAPI_subscriber_handler::DoGet() {
		auto EMail = ORM::Escape(GetParameter("email"));
		auto SignupUUID = GetParameter("signupUUID");
		auto macAddress = ORM::Escape(GetParameter("macAddress"));
		auto List = GetBoolParameter("listOnly", false);

		poco_information(Logger(), fmt::format("Looking for signup for {}", EMail));
		Poco::JSON::Object Answer;
		ProvObjects::SignupEntry SE;
		if (!SignupUUID.empty()) {
			poco_information(
				Logger(), fmt::format("Looking for signup for {}: Signup {}", EMail, SignupUUID));
			if (StorageService()->SignupDB().GetRecord("id", SignupUUID, SE)) {
				SE.to_json(Answer);
				return ReturnObject(Answer);
			}
			return NotFound();
		} else if (!EMail.empty()) {
			SignupDB::RecordVec SEs;
			poco_information(
				Logger(), fmt::format("Looking for signup for {}: Signup {}", EMail, SignupUUID));
			if (StorageService()->SignupDB().GetRecords(0, 100, SEs, " email='" + EMail + "' ")) {
				return ReturnObject("signups", SEs);
			}
			return NotFound();
		} else if (!macAddress.empty()) {
			SignupDB::RecordVec SEs;
			poco_information(Logger(),
							 fmt::format("Looking for signup for {}: Mac {}", EMail, macAddress));
			if (StorageService()->SignupDB().GetRecords(0, 100, SEs,
														" macAddress='" + macAddress + "' ")) {
				return ReturnObject("signups", SEs);
			}
			return NotFound();
		} else if (List) {
			poco_information(Logger(),
							 fmt::format("Returning list of signups...", EMail, macAddress));
			SignupDB::RecordVec SEs;
			StorageService()->SignupDB().GetRecords(0, 100, SEs);
			return ReturnObject("signups", SEs);
		}
		poco_information(Logger(), fmt::format("Bad signup get", EMail, macAddress));
		return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
	}

	void RESTAPI_subscriber_handler::DoDelete() {
		auto subscriberId = GetBinding("id", "");
		if (subscriberId.empty()) {
			subscriberId = GetParameter("subscriberId", "");
		}
		if (subscriberId.empty()) {
			poco_warning(Logger(),
						 "[SUBSCRIBER_DELETE]: Missing subscriberId in request.");
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
		}

		ProvObjects::SignupEntry signupRecord;
		if (!StorageService()->SignupDB().GetRecord("userid", subscriberId, signupRecord)) {
			poco_warning(Logger(), fmt::format(
									 "[SUBSCRIBER_DELETE]: Signup record not found for subscriber [{}].",
									 subscriberId));
			return NotFound();
		}

		auto inventoryCount = StorageService()->InventoryDB().Count(
			StorageService()->InventoryDB().OP("subscriber", ORM::EQ, subscriberId));
		auto subscriberDeviceCount = StorageService()->SubscriberDeviceDB().Count(
			StorageService()->SubscriberDeviceDB().OP("subscriberId", ORM::EQ, subscriberId));
		if (inventoryCount > 0 || subscriberDeviceCount > 0) {
			poco_warning(Logger(),
						 fmt::format("[SUBSCRIBER_DELETE]: subscriber [{}] still has associated "
									 "devices. Deletion rejected.",
									 subscriberId));
			return BadRequest(RESTAPI::Errors::StillInUse);
		}

		VenueDB::RecordVec subscriberVenues;
		if (StorageService()->VenueDB().GetRecords(
				0, 100, subscriberVenues,
				fmt::format(" subscriber='{}' ", ORM::Escape(subscriberId)))) {
			for (const auto &subscriberVenue : subscriberVenues) {
				if (!subscriberVenue.devices.empty()) {
					poco_warning(Logger(), fmt::format(
											 "[SUBSCRIBER_DELETE]: Venue [{}] still has {} devices "
											 "for subscriber [{}].",
											 subscriberVenue.info.id,
											 subscriberVenue.devices.size(), subscriberId));
					return BadRequest(RESTAPI::Errors::StillInUse);
				}
				if (!subscriberVenue.entity.empty()) {
					StorageService()->EntityDB().DeleteVenue("id", subscriberVenue.entity,
															 subscriberVenue.info.id);
				}
				if (!StorageService()->VenueDB().DeleteRecord("id", subscriberVenue.info.id)) {
					return BadRequest(RESTAPI::Errors::NoRecordsDeleted);
				}
			}
		}

		if (!SDK::Sec::Subscriber::Delete(this, subscriberId)) {
			return BadRequest(RESTAPI::Errors::CouldNotBeDeleted);
		}

		if (!StorageService()->SignupDB().DeleteRecord("id", signupRecord.info.id)) {
			return BadRequest(RESTAPI::Errors::NoRecordsDeleted);
		}
		return OK();
	}

} // namespace OpenWifi
