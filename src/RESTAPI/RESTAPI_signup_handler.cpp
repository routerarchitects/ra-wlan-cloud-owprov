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

	void RESTAPI_signup_handler::DoPost() {
		auto UserName = GetParameter("email");
		Poco::toLowerInPlace(UserName);
		Poco::trimInPlace(UserName);

		auto macAddress = GetParameter("macAddress");
		Poco::toLowerInPlace(macAddress);
		Poco::trimInPlace(macAddress);

		auto deviceID = GetParameter("deviceID");
		Poco::toLowerInPlace(deviceID);
		Poco::trimInPlace(deviceID);

		auto registrationId = GetParameter("registrationId");
		Poco::toLowerInPlace(registrationId);
		Poco::trimInPlace(registrationId);

		if (UserName.empty() || macAddress.empty() || registrationId.empty()) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
		}

		if (!Utils::ValidEMailAddress(UserName)) {
			return BadRequest(RESTAPI::Errors::InvalidEmailAddress);
		}

		if (!Utils::ValidSerialNumber(macAddress)) {
			return BadRequest(RESTAPI::Errors::InvalidSerialNumber);
		}

		// find the operator id
		ProvObjects::Operator SignupOperator;
		if (!StorageService()->OperatorDB().GetRecord("registrationId", registrationId,
													  SignupOperator)) {
			return BadRequest(RESTAPI::Errors::InvalidRegistrationOperatorName);
		}

		//  if a signup already exists for this user, we should just return its value completion
		SignupDB::RecordVec SEs;
		if (StorageService()->SignupDB().GetRecords(
				0, 100, SEs, " email='" + UserName + "' and macAddress='" + macAddress + "' ")) {
			for (const auto &i : SEs) {

				if (!i.deviceID.empty() && i.deviceID != deviceID) {
					return BadRequest(RESTAPI::Errors::InvalidDeviceID);
				}

				if (i.statusCode == ProvObjects::SignupStatusCodes::SignupWaitingForEmail ||
					i.statusCode == ProvObjects::SignupStatusCodes::SignupWaitingForDevice ||
					i.statusCode == ProvObjects::SignupStatusCodes::SignupSuccess) {
					poco_debug(
						Logger(),
						fmt::format("SIGNUP: Returning existing signup record for '{}'", i.email));
					Poco::JSON::Object Answer;
					i.to_json(Answer);
					return ReturnObject(Answer);
				}
			}
		}

		// Return error if user already exists. We do not allow multiple users with same email
		SignupDB::RecordVec UserInv;
		if (StorageService()->SignupDB().GetRecords(0, 100, UserInv, " email='" + UserName + "' ")) {
			poco_error(Logger(), fmt::format("SIGNUP: Email {} already registered", UserName));
			return BadRequest(RESTAPI::Errors::UserAlreadyExists);
		}

		// Return error if device is already provisioned with another subscriber. We do not
		// allow multiple users with same device
		SignupDB::RecordVec inv;
		if (StorageService()->SignupDB().GetRecords(0, 100, inv, " macAddress='" + macAddress + "' ")) {
			poco_error(Logger(), fmt::format("SIGNUP: Device {} already provisioned with another subscriber", macAddress));
			return BadRequest(RESTAPI::Errors::SerialNumberAlreadyProvisioned);
		}

		//  So we do not have an outstanding signup...
		//  Can we actually claim this serial number??? if not, we need to return an error
		ProvObjects::InventoryTag IT;
		std::string SerialNumber;
		bool FoundIT = false;
		for (int Index = 0; Index < 4; Index++) {
			auto TrySerialNumber = Utils::SerialNumberToInt(macAddress);
			for (uint i = 0; i < 4; ++i) {
				SerialNumber = Utils::IntToSerialNumber(TrySerialNumber + i);
				if (StorageService()->InventoryDB().GetRecord("serialNumber", SerialNumber, IT)) {
					if (!IT.subscriber.empty()) {
						return BadRequest(RESTAPI::Errors::SerialNumberAlreadyProvisioned);
					}

					if (!(IT.devClass.empty() || IT.devClass == "subscriber" ||
						  IT.devClass == "any")) {
						return BadRequest(RESTAPI::Errors::SerialNumberNotTheProperClass);
					}
					FoundIT = true;
					break;
				}
			}
		}

		//  OK, we can claim this device, can we create a userid?
		//  Let's create one
		//  If sec.signup("email",uuid);
		auto SignupUUID = MicroServiceCreateUUID();
		poco_debug(Logger(), fmt::format("SIGNUP: Creating signup entry for '{}', uuid='{}'",
										 UserName, SignupUUID));

		Poco::JSON::Object Body;
		OpenAPIRequestPost CreateUser(uSERVICE_SECURITY, "/api/v1/signup",
									  {{"email", UserName},
									   {"signupUUID", SignupUUID},
									   {"owner", SignupOperator.info.id},
									   {"operatorName", SignupOperator.registrationId}},
									  Body, 30000);

		Poco::JSON::Object::Ptr Answer;
		if (CreateUser.Do(Answer) == Poco::Net::HTTPServerResponse::HTTP_OK) {
			SecurityObjects::UserInfo UI;

			UI.from_json(Answer);
			std::ostringstream os;
			Answer->stringify(os);
			poco_debug(Logger(), fmt::format("SIGNUP: email: '{}' signupID: '{}' userId: '{}'",
											 UserName, SignupUUID, UI.id));

			//  so create the Signup entry and modify the inventory
			ProvObjects::SignupEntry SE;
			SE.info.id = SignupUUID;
			SE.info.created = SE.info.modified = SE.submitted = Utils::Now();
			SE.completed = 0;
			SE.macAddress = macAddress;
			SE.error = 0;
			SE.userId = UI.id;
			SE.email = UserName;
			SE.deviceID = deviceID;
			SE.registrationId = registrationId;
			SE.status = "waiting-for-email-verification";
			SE.operatorId = SignupOperator.info.id;
			SE.statusCode = ProvObjects::SignupStatusCodes::SignupWaitingForEmail;
			StorageService()->SignupDB().CreateRecord(SE);
			Signup()->AddOutstandingSignup(SE);

			if (FoundIT) {
				Poco::JSON::Object StateDoc;
				StateDoc.set("method", "signup");
				StateDoc.set("claimer", UserName);
				StateDoc.set("claimerId", UI.id);
				StateDoc.set("signupUUID", SignupUUID);
				StateDoc.set("errorCode", 0);
				StateDoc.set("date", Utils::Now());
				StateDoc.set("status", "waiting for email-verification");
				std::ostringstream os2;
				StateDoc.stringify(os2);
				IT.realMacAddress = macAddress;
				IT.state = os2.str();
				IT.info.modified = Utils::Now();
				std::cout << "Updating inventory entry: " << SE.macAddress << std::endl;
				StorageService()->InventoryDB().UpdateRecord("id", IT.info.id, IT);
			}

			Poco::JSON::Object SEAnswer;
			SE.to_json(SEAnswer);
			return ReturnObject(SEAnswer);
		}
		return BadRequest(RESTAPI::Errors::UserAlreadyExists);
	}

	//  this will be called by the SEC backend once the password has been verified.
	void RESTAPI_signup_handler::DoPut() {
		auto SignupUUID = GetParameter("signupUUID");
		auto Operation = GetParameter("operation");

		poco_information(Logger(), fmt::format("signup-progress: {} - {} ", SignupUUID, Operation));
		if (SignupUUID.empty() || Operation.empty()) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
		}

		ProvObjects::SignupEntry SE;
		poco_information(Logger(), fmt::format("signup-progress: {} - {} fetching entry",
											   SignupUUID, Operation));
		if (!StorageService()->SignupDB().GetRecord("id", SignupUUID, SE)) {
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
														" serialNumber='" + macAddress + "' ")) {
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
