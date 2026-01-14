/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
//
// Created by stephane bourque on 2022-02-20.
//

#include "RESTAPI_signup_handler.h"
#include "Signup.h"
#include "StorageService.h"
#include "framework/MicroServiceFuncs.h"
#include "framework/MicroServiceNames.h"
#include "framework/OpenAPIRequests.h"
#include "framework/utils.h"

namespace OpenWifi {

	/*
		1) Record notExists + resend=true/false -> Create new UUID and call Security
		2) Record exists(waiting-for-email-verification) + resend=true -> Reuse existing UUID and call Security
		3) Record exists(waiting-for-email-verification) + resend=false -> UserAlreadyExists
		4) Record exists(emailVerified) + resend=true/false -> UserAlreadyExists
		5) Record exists(email) with different mac -> UserAlreadyExists
		6) Record exists(mac) with different email -> SerialNumberAlreadyProvisioned
	*/
	void RESTAPI_signup_handler::DoPost() {
		auto norm = [](std::string s) {
			Poco::toLowerInPlace(s);
			Poco::trimInPlace(s);
			return s;
		};

		const auto UserName       = norm(GetParameter("email"));
		const auto macAddress     = norm(GetParameter("macAddress"));
		const auto deviceID       = norm(GetParameter("deviceID"));
		const auto registrationId = norm(GetParameter("registrationId"));
		const bool resend         = GetBoolParameter("resend", false);

		if (UserName.empty() || macAddress.empty() || registrationId.empty()) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
		}
		if (!Utils::ValidEMailAddress(UserName)) {
			return BadRequest(RESTAPI::Errors::InvalidEmailAddress);
		}
		if (!Utils::ValidSerialNumber(macAddress)) {
			return BadRequest(RESTAPI::Errors::InvalidSerialNumber);
		}

		Logger_.information(fmt::format(
			"SIGNUP: Signup request for '{}' mac='{}' reg='{}' resend={}",
			UserName, macAddress, registrationId, resend));

		ProvObjects::Operator SignupOperator;
		if (!StorageService()->OperatorDB().GetRecord("registrationId", registrationId, SignupOperator)) {
			return BadRequest(RESTAPI::Errors::InvalidRegistrationOperatorName);
		}

		// Lookup existing signup entries by email and device
		ProvObjects::SignupEntry byEmail{};
		const bool foundByEmail = StorageService()->SignupDB().GetRecord("email", UserName, byEmail);

		ProvObjects::SignupEntry byMac{};
		const bool foundByMac = StorageService()->SignupDB().GetRecord("macAddress", macAddress, byMac);

		// 1) Email exists but tied to another device => reject
		if (foundByEmail && Poco::icompare(byEmail.macAddress, macAddress) != 0) {
			poco_error(Logger(), fmt::format("SIGNUP: Email {} already registered (mac={})",
											UserName, byEmail.macAddress));
			return BadRequest(RESTAPI::Errors::UserAlreadyExists);
		}

		// 2) Device exists but tied to another email => reject
		if (foundByMac && Poco::icompare(byMac.email, UserName) != 0) {
			poco_error(Logger(), fmt::format(
				"SIGNUP: Device {} already provisioned with another subscriber (email={})",
				macAddress, byMac.email));
			return BadRequest(RESTAPI::Errors::SerialNumberAlreadyProvisioned);
		}

		ProvObjects::SignupEntry existing{};
		if (foundByEmail) { // If you are here and email exists, mac matches too
			existing = byEmail;
		}

		const bool reuseExisting = resend && foundByEmail;

		// Reuse existing signup UUID for resend; otherwise generate a new one.
		const auto SignupUUID = reuseExisting ? existing.info.id : MicroServiceCreateUUID();

		poco_debug(Logger(), fmt::format(
			"SIGNUP: {} request for '{}' mac='{}' reg='{}' uuid='{}'",
			reuseExisting ? "Resend(reuse)" : "Create(new)",
			UserName, macAddress, registrationId, SignupUUID));

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

		// Build the SignupEntry to return, then persist it (update vs create)
		ProvObjects::SignupEntry se = reuseExisting ? existing : ProvObjects::SignupEntry{};

		const auto now = Utils::Now();
		if (!reuseExisting) {
			se.info.id = SignupUUID;
			se.info.created = se.info.modified = se.submitted = now;
			se.completed = 0;
			se.serialNumber = macAddress;
			se.macAddress = macAddress;
			se.error = 0;
			se.userId = UI.id;
			se.email = UserName;
			se.deviceID = deviceID;
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

		Poco::JSON::Object SEAnswer;
		se.to_json(SEAnswer);
		return ReturnObject(SEAnswer);
	}

	//  this will be called by the SEC backend once the password has been verified.
	void RESTAPI_signup_handler::DoPut() {
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

	void RESTAPI_signup_handler::DoGet() {
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

	void RESTAPI_signup_handler::DoDelete() {
		auto EMail = GetParameter("email", "");
		auto SignupUUID = GetParameter("signupUUID", "");
		auto macAddress = GetParameter("macAddress", "");
		auto deviceID = GetParameter("deviceID", "");

		if (!SignupUUID.empty()) {
			if (StorageService()->SignupDB().DeleteRecord("id", SignupUUID)) {
				return OK();
			}
			return NotFound();
		} else if (!EMail.empty()) {
			if (StorageService()->SignupDB().DeleteRecord("email", EMail)) {
				return OK();
			}
			return NotFound();
		} else if (!macAddress.empty()) {
			if (StorageService()->SignupDB().DeleteRecord("serialNumber", macAddress)) {
				return OK();
			}
			return NotFound();
		}
		return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
	}

} // namespace OpenWifi
