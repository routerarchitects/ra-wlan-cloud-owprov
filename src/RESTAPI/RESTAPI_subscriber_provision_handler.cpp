/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#include "RESTAPI_subscriber_provision_handler.h"
#include "Poco/String.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "framework/RESTAPI_utils.h"
#include "framework/orm.h"
#include "framework/ow_constants.h"
#include "framework/utils.h"
#include "sdks/SDK_analytics.h"
#include "sdks/SDK_gw.h"

namespace OpenWifi {

	bool RESTAPI_subscriber_provision_handler::ParseRequest(const Poco::JSON::Object::Ptr &raw,
															ProvisionContext &ctx) {
		if (!raw) {
			BadRequest(RESTAPI::Errors::InvalidJSONDocument);
			return false;
		}

		std::string subscriberId;
		RESTAPI_utils::field_from_json(raw, "subscriberId", subscriberId);
		if (subscriberId.empty()) {
			BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
			return false;
		}

		RESTAPI_utils::field_from_json(raw, "enableMonitoring", ctx.enableMonitoring);
		if (ctx.enableMonitoring && raw->has("monitoring")) {
			auto monitoring = raw->get("monitoring").extract<Poco::JSON::Object::Ptr>();
			if (monitoring) {
				RESTAPI_utils::field_from_json(monitoring, "retention", ctx.retention);
				RESTAPI_utils::field_from_json(monitoring, "interval", ctx.interval);
				RESTAPI_utils::field_from_json(monitoring, "monitorSubVenues",
											   ctx.monitorSubVenues);
			}
		}

		ctx.signupRecord.userId = subscriberId;
		return true;
	}

	bool RESTAPI_subscriber_provision_handler::LoadSignupRecord(ProvisionContext &ctx) {
		if (!SignupDB_.GetRecord("userid", ctx.signupRecord.userId, ctx.signupRecord)) {
			poco_error(
				Logger(),
				fmt::format("[SUBSCRIBER_PROVISION]: Signup record for subscriber {} not Found",
							ctx.signupRecord.userId));
			NotFound();
			return false;
		}

		if (ctx.signupRecord.serialNumber.empty() || ctx.signupRecord.registrationId.empty() ||
			ctx.signupRecord.email.empty()) {
			poco_error(Logger(), fmt::format("[SUBSCRIBER_PROVISION]: subscriber {} is missing "
											 "required fields SerialNumber/RegistrationId/Email.",
											 ctx.signupRecord.userId));
			BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
			return false;
		}
		ctx.venueName = ctx.signupRecord.email;
		return true;
	}

	bool RESTAPI_subscriber_provision_handler::LoadOperatorRecord(ProvisionContext &ctx) {
		if (!OperatorDB_.GetRecord("registrationId", ctx.signupRecord.registrationId,
								   ctx.operatorRecord)) {
			BadRequest(RESTAPI::Errors::InvalidRegistrationOperatorName);
			return false;
		}

		if (ctx.operatorRecord.entityId.empty() ||
			!EntityDB_.Exists("id", ctx.operatorRecord.entityId)) {
			BadRequest(RESTAPI::Errors::EntityMustExist);
			return false;
		}

		return true;
	}

	bool RESTAPI_subscriber_provision_handler::LoadVenueRecord(ProvisionContext &ctx) {
		if (ctx.venueRecord.info.id.empty()) {
			poco_debug(
				Logger(),
				fmt::format(
					"[SUBSCRIBER_PROVISION]: Venue identification missing for subscriber {}.",
					ctx.signupRecord.userId));
			return false;
		}

		if (!VenueDB_.GetRecord("id", ctx.venueRecord.info.id, ctx.venueRecord)) {
			poco_debug(Logger(),
					   fmt::format("[SUBSCRIBER_PROVISION]: Venue {} not found for subscriber {}.",
								   ctx.venueRecord.info.id, ctx.signupRecord.userId));
			return false;
		}

		return true;
	}

	bool RESTAPI_subscriber_provision_handler::CreateVenueRecord(ProvisionContext &ctx) {
		if (VenueDB_.DoesVenueNameAlreadyExist(ctx.venueName, ctx.operatorRecord.entityId, "")) {
			BadRequest(RESTAPI::Errors::VenuesNameAlreadyExists);
			return false;
		}

		ProvObjects::CreateObjectInfo(UserInfo_.userinfo, ctx.venueRecord.info);
		ctx.venueRecord.info.name = ctx.venueName;
		ctx.venueRecord.entity = ctx.operatorRecord.entityId;

		if (!VenueDB_.CreateRecord(ctx.venueRecord)) {
			InternalError(RESTAPI::Errors::RecordNotCreated);
			return false;
		}

		ManageMembership(StorageService()->EntityDB(), &ProvObjects::Entity::venues, "",
						 ctx.venueRecord.entity, ctx.venueRecord.info.id);
		return true;
	}

	bool RESTAPI_subscriber_provision_handler::DeleteVenueRecord(ProvisionContext &ctx) {
		auto &venue = ctx.venueRecord;

		if (venue.info.id.empty()) {
			poco_error(
				Logger(),
				fmt::format("[SUBSCRIBER_PROVISION]: Venue record ID missing for deletion."));
			BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
			return false;
		}

		poco_debug(Logger(),
				   fmt::format("[SUBSCRIBER_PROVISION]: Deleting venue record {}.", venue.info.id));

		if (!VenueDB_.DeleteRecord("id", venue.info.id)) {
			return BadRequest(RESTAPI::Errors::NoRecordsDeleted), false;
		}
		ManageMembership(StorageService()->EntityDB(), &ProvObjects::Entity::venues,
						 ctx.operatorRecord.entityId, "", venue.info.id);
		return true;
	}

	bool RESTAPI_subscriber_provision_handler::LinkInventoryRecord(ProvisionContext &ctx) {
		if (!InventoryDB_.GetRecord("serialNumber", ctx.signupRecord.serialNumber,
									ctx.inventoryRecord)) {
			poco_error(
				Logger(),
				fmt::format("[SUBSCRIBER_PROVISION]: Device with serial number {} not found.",
							ctx.signupRecord.serialNumber));
			NotFound();
			return false;
		}

		ctx.inventoryRecord.venue = ctx.venueRecord.info.id;
		ctx.inventoryRecord.info.modified = Utils::Now();

		if (!InventoryDB_.UpdateRecord("id", ctx.inventoryRecord.info.id, ctx.inventoryRecord)) {
			InternalError(RESTAPI::Errors::RecordNotUpdated);
			return false;
		}
		AddMembership(StorageService()->VenueDB(), &ProvObjects::Venue::devices,
					  ctx.inventoryRecord.venue, ctx.inventoryRecord.info.id);

		poco_trace(Logger(),
				   fmt::format("[SUBSCRIBER_PROVISION]: Device with serial number {} "
							   "provisioned to subscriber {}.",
							   ctx.inventoryRecord.serialNumber, ctx.signupRecord.userId));
		return true;
	}

	bool RESTAPI_subscriber_provision_handler::UnlinkInventoryRecord(ProvisionContext &ctx) {
		if (!InventoryDB_.GetRecord("serialNumber", ctx.signupRecord.serialNumber,
									ctx.inventoryRecord)) {
			poco_trace(
				Logger(),
				fmt::format(
					"[SUBSCRIBER_PROVISION]: Inventory Device with serial number {} not found.",
					ctx.signupRecord.serialNumber));
			// Idempotent: nothing to unlink
			return true;
		}

		const auto previousVenueId = ctx.inventoryRecord.venue;
		if (previousVenueId.empty()) {
			poco_trace(Logger(), fmt::format("[SUBSCRIBER_PROVISION]: Inventory Device with serial "
											 "number {} not linked to any venue.",
											 ctx.signupRecord.serialNumber));
			// Idempotent: nothing to unlink
			return true;
		}
		ctx.venueRecord.info.id = previousVenueId;
		ctx.inventoryRecord.venue = "";
		ctx.inventoryRecord.info.modified = Utils::Now();

		if (!InventoryDB_.UpdateRecord("id", ctx.inventoryRecord.info.id, ctx.inventoryRecord)) {
			InternalError(RESTAPI::Errors::RecordNotUpdated);
			return false;
		}
		RemoveMembership(StorageService()->VenueDB(), &ProvObjects::Venue::devices, previousVenueId,
						 ctx.inventoryRecord.info.id);
		return true;
	}

	bool RESTAPI_subscriber_provision_handler::StartMonitoring(ProvisionContext &ctx) {
		if (!ctx.enableMonitoring) {
			return true;
		}

		Poco::JSON::Object boardBody;
		boardBody.set("name", ctx.venueName);
		Poco::JSON::Array venueList;
		Poco::JSON::Object venueEntry;
		venueEntry.set("id", ctx.venueRecord.info.id);
		venueEntry.set("name", ctx.venueName);
		venueEntry.set("retention", ctx.retention);
		venueEntry.set("interval", ctx.interval);
		venueEntry.set("monitorSubVenues", ctx.monitorSubVenues);
		venueList.add(venueEntry);
		boardBody.set("venueList", venueList);

		Poco::JSON::Object::Ptr boardResponse;
		Poco::Net::HTTPServerResponse::HTTPStatus boardStatus;
		if (!SDK::Analytics::StartMonitoring(boardBody, boardResponse, boardStatus)) {
			poco_error(Logger(),
					   fmt::format("[SUBSCRIBER_PROVISION]: Failed to start monitoring "
								   "for venue {} (status {}).",
								   ctx.venueRecord.info.id, static_cast<int>(boardStatus)));
			InternalError(RESTAPI::Errors::RecordNotCreated);
			return false;
		}

		if (!boardResponse || !boardResponse->has("id")) {
			poco_error(Logger(),
					   "[SUBSCRIBER_PROVISION]: Monitoring started but no boardId returned.");
			InternalError(RESTAPI::Errors::RecordNotCreated);
			return false;
		}

		ctx.boardId = boardResponse->get("id").toString();
		if (ctx.boardId.empty()) {
			poco_error(Logger(),
					   "[SUBSCRIBER_PROVISION]: Monitoring started but empty boardId returned.");
			InternalError(RESTAPI::Errors::RecordNotCreated);
			return false;
		}

		ProvObjects::Venue venueRecord;
		if (!VenueDB_.GetRecord("id", ctx.venueRecord.info.id, venueRecord)) {
			poco_error(
				Logger(),
				fmt::format("[SUBSCRIBER_PROVISION]: Venue {} not found after monitoring start.",
							ctx.venueRecord.info.id));
			InternalError(RESTAPI::Errors::MissingOrInvalidParameters);
			return false;
		}

		venueRecord.boards.push_back(ctx.boardId);
		venueRecord.info.modified = Utils::Now();
		if (!VenueDB_.UpdateRecord("id", venueRecord.info.id, venueRecord)) {
			poco_error(
				Logger(),
				fmt::format("[SUBSCRIBER_PROVISION]: Failed to update venue {} with boardId {}.",
							venueRecord.info.id, ctx.boardId));
			InternalError(RESTAPI::Errors::RecordNotUpdated);
			return false;
		}
		ctx.venueRecord = venueRecord;
		return true;
	}

	bool RESTAPI_subscriber_provision_handler::StopMonitoring(ProvisionContext &ctx) {
		auto &venue = ctx.venueRecord;
		if (venue.boards.empty()) {
			return true;
		}
		// Stop monitoring for all boards
		for (const auto &boardId : venue.boards) {
			Poco::Net::HTTPServerResponse::HTTPStatus callStatus =
				Poco::Net::HTTPServerResponse::HTTP_INTERNAL_SERVER_ERROR;
			if (!SDK::Analytics::StopMonitoring(boardId, callStatus)) {
				poco_warning(Logger(),
							 fmt::format("[SUBSCRIBER_PROVISION][DELETE]: Failed to stop "
										 "monitoring for board {} (status {}). Continuing.",
										 boardId, static_cast<int>(callStatus)));
			}
		}

		// Now clear the boards list
		auto updatedVenue = venue;
		updatedVenue.boards.clear();
		updatedVenue.info.modified = Utils::Now();

		if (!VenueDB_.UpdateRecord("id", updatedVenue.info.id, updatedVenue)) {
			return InternalError(RESTAPI::Errors::RecordNotUpdated), false;
		}

		// Only update the context reference on success
		venue = std::move(updatedVenue);
		return true;
	}

	void RESTAPI_subscriber_provision_handler::DoPost() {
		ProvisionContext ctx;

		if (!ParseRequest(ParsedBody_, ctx))
			return;
		if (!LoadSignupRecord(ctx))
			return;
		if (!LoadOperatorRecord(ctx))
			return;
		if (!CreateVenueRecord(ctx))
			return;
		if (!LinkInventoryRecord(ctx))
			return;
		if (!StartMonitoring(ctx))
			return;

		Poco::JSON::Object Answer;
		Answer.set("boardId", ctx.boardId);
		Answer.set("entityId", ctx.operatorRecord.entityId);
		Answer.set("serialNumber", ctx.inventoryRecord.serialNumber);
		Answer.set("subscriberId", ctx.signupRecord.userId);
		Answer.set("venueId", ctx.venueRecord.info.id);
		Answer.set("venueName", ctx.venueName);
		ReturnObject(Answer);
	}

	void RESTAPI_subscriber_provision_handler::DoDelete() {
		ProvisionContext ctx;

		auto subscriberId = GetBinding("uuid", "");
		if (subscriberId.empty()) {
			BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
			return;
		}
		ctx.signupRecord.userId = subscriberId;
		if (!LoadSignupRecord(ctx))
			return;
		if (!LoadOperatorRecord(ctx))
			return;
		if (!UnlinkInventoryRecord(ctx))
			return;
		if (!LoadVenueRecord(ctx)) {
			OK();
			return;
		}
		if (!StopMonitoring(ctx))
			return;
		if (!DeleteVenueRecord(ctx))
			return;
		OK();
	}

} // namespace OpenWifi
