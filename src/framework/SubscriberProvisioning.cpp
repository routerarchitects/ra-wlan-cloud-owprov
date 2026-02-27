/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#include "framework/SubscriberProvisioning.h"

#include <algorithm>

#include "Poco/Net/HTTPServerResponse.h"
#include "Poco/String.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "StorageService.h"
#include "framework/orm.h"
#include "framework/utils.h"
#include "fmt/format.h"
#include "sdks/SDK_analytics.h"
#include "sdks/SDK_gw.h"

namespace OpenWifi::SubscriberProvisioning {
namespace {

	struct SubscriberContext {
		ProvObjects::SignupEntry signupRecord{};
		ProvObjects::Operator operatorRecord{};
		ProvObjects::Venue venueRecord{};
		std::string venueName;
	};

	bool ResolveSubscriberContext(const std::string &subscriberId, bool createVenueIfMissing,
								  const SecurityObjects::UserInfo *actor,
								  SubscriberContext &ctx, Poco::Logger &logger) {
		auto &signupDB = StorageService()->SignupDB();
		if (!signupDB.GetRecord("userid", subscriberId, ctx.signupRecord)) {
			poco_debug(logger, fmt::format("[SUBSCRIBER_PROVISIONING]: Signup record not found for "
										   "subscriber [{}].",
										   subscriberId));
			return false;
		}

		if (ctx.signupRecord.email.empty()) {
			poco_warning(logger, fmt::format("[SUBSCRIBER_PROVISIONING]: Signup email missing for "
											"subscriber [{}].",
											subscriberId));
			return false;
		}

		auto &operatorDB = StorageService()->OperatorDB();
		bool haveOperator = false;
		if (!ctx.signupRecord.operatorId.empty()) {
			haveOperator = operatorDB.GetRecord("id", ctx.signupRecord.operatorId, ctx.operatorRecord);
		}
		if (!haveOperator && !ctx.signupRecord.registrationId.empty()) {
			haveOperator = operatorDB.GetRecord("registrationId", ctx.signupRecord.registrationId,
												ctx.operatorRecord);
		}
		if (!haveOperator) {
			poco_warning(logger, fmt::format("[SUBSCRIBER_PROVISIONING]: Operator record not found "
											"for subscriber [{}].",
											subscriberId));
			return false;
		}

		if (ctx.operatorRecord.entityId.empty() ||
			!StorageService()->EntityDB().Exists("id", ctx.operatorRecord.entityId)) {
			poco_warning(logger, fmt::format("[SUBSCRIBER_PROVISIONING]: Operator entity invalid "
											"for subscriber [{}].",
											subscriberId));
			return false;
		}

		ctx.venueName = ctx.signupRecord.email;
		const auto where =
			fmt::format("entity='{}' and upper(name)='{}'",
						ORM::Escape(ctx.operatorRecord.entityId),
						ORM::Escape(Poco::toUpper(ctx.venueName)));

		auto &venueDB = StorageService()->VenueDB();
		if (venueDB.GetRecord(ctx.venueRecord, where)) {
			return true;
		}

		if (!createVenueIfMissing || actor == nullptr) {
			poco_debug(logger, fmt::format("[SUBSCRIBER_PROVISIONING]: Venue not found for "
										   "subscriber [{}].",
										   subscriberId));
			return false;
		}

		ProvObjects::CreateObjectInfo(*actor, ctx.venueRecord.info);
		ctx.venueRecord.info.name = ctx.venueName;
		ctx.venueRecord.entity = ctx.operatorRecord.entityId;
		ctx.venueRecord.subscriber = ctx.signupRecord.userId;

		if (!venueDB.CreateRecord(ctx.venueRecord)) {
			poco_error(logger, fmt::format("[SUBSCRIBER_PROVISIONING]: Failed to create venue "
										   "for subscriber [{}].",
										   subscriberId));
			return false;
		}

		ManageMembership(StorageService()->EntityDB(), &ProvObjects::Entity::venues, "",
						 ctx.venueRecord.entity, ctx.venueRecord.info.id);
		return true;
	}

	bool EnsureMonitoringStartedForVenue(const std::string &venueId, const std::string &venueNameHint,
										 Poco::Logger &logger) {
		if (venueId.empty()) {
			return true;
		}

		auto &venueDB = StorageService()->VenueDB();
		ProvObjects::Venue venueRecord;
		if (!venueDB.GetRecord("id", venueId, venueRecord)) {
			poco_warning(logger, fmt::format("[SUBSCRIBER_PROVISIONING]: Venue [{}] not found for "
											"monitoring start.",
											venueId));
			return false;
		}
		if (!venueRecord.boards.empty()) {
			return true;
		}

		Poco::JSON::Object boardBody;
		boardBody.set("name", venueRecord.info.name.empty() ? venueNameHint : venueRecord.info.name);

		Poco::JSON::Array venueList;
		Poco::JSON::Object venueEntry;
		venueEntry.set("id", venueRecord.info.id);
		venueEntry.set("name",
					   venueRecord.info.name.empty() ? venueNameHint : venueRecord.info.name);
		venueEntry.set("retention", static_cast<uint64_t>(604800));
		venueEntry.set("interval", static_cast<uint64_t>(60));
		venueEntry.set("monitorSubVenues", true);
		venueList.add(venueEntry);
		boardBody.set("venueList", venueList);

		Poco::JSON::Object::Ptr boardResponse;
		Poco::Net::HTTPServerResponse::HTTPStatus boardStatus =
			Poco::Net::HTTPServerResponse::HTTP_INTERNAL_SERVER_ERROR;
		if (!SDK::Analytics::StartMonitoring(boardBody, boardResponse, boardStatus)) {
			poco_error(logger, fmt::format("[SUBSCRIBER_PROVISIONING]: Failed to start monitoring "
										   "for venue [{}], status [{}].",
										   venueId, static_cast<int>(boardStatus)));
			return false;
		}

		if (!boardResponse || !boardResponse->has("id")) {
			poco_error(logger, fmt::format("[SUBSCRIBER_PROVISIONING]: Monitoring started for "
										   "venue [{}] but no board id returned.",
										   venueId));
			return false;
		}

		auto boardId = boardResponse->get("id").toString();
		if (boardId.empty()) {
			poco_error(logger, fmt::format("[SUBSCRIBER_PROVISIONING]: Monitoring started for "
										   "venue [{}] but board id is empty.",
										   venueId));
			return false;
		}

		if (!venueDB.GetRecord("id", venueId, venueRecord)) {
			poco_error(logger, fmt::format("[SUBSCRIBER_PROVISIONING]: Venue [{}] not found after "
										   "monitoring start.",
										   venueId));
			return false;
		}
		if (std::find(venueRecord.boards.begin(), venueRecord.boards.end(), boardId) !=
			venueRecord.boards.end()) {
			return true;
		}

		venueRecord.boards.push_back(boardId);
		venueRecord.info.modified = Utils::Now();
		if (!venueDB.UpdateRecord("id", venueRecord.info.id, venueRecord)) {
			poco_error(logger, fmt::format("[SUBSCRIBER_PROVISIONING]: Failed to persist board "
										   "[{}] for venue [{}].",
										   boardId, venueId));
			return false;
		}

		return true;
	}

	bool StopMonitoringForVenue(const std::string &venueId, Poco::Logger &logger) {
		if (venueId.empty()) {
			return true;
		}

		auto &venueDB = StorageService()->VenueDB();
		ProvObjects::Venue venueRecord;
		if (!venueDB.GetRecord("id", venueId, venueRecord)) {
			return true;
		}
		if (venueRecord.boards.empty()) {
			return true;
		}

		for (const auto &boardId : venueRecord.boards) {
			Poco::Net::HTTPServerResponse::HTTPStatus callStatus =
				Poco::Net::HTTPServerResponse::HTTP_INTERNAL_SERVER_ERROR;
			if (!SDK::Analytics::StopMonitoring(boardId, callStatus)) {
				poco_warning(logger, fmt::format("[SUBSCRIBER_PROVISIONING]: Failed to stop board "
												 "[{}], status [{}]. Continuing.",
												 boardId, static_cast<int>(callStatus)));
			}
		}

		venueRecord.boards.clear();
		venueRecord.info.modified = Utils::Now();
		if (!venueDB.UpdateRecord("id", venueRecord.info.id, venueRecord)) {
			poco_error(logger, fmt::format("[SUBSCRIBER_PROVISIONING]: Failed to clear boards for "
										   "venue [{}].",
										   venueId));
			return false;
		}
		return true;
	}

} // namespace

bool SyncInventoryForSubscriberDevice(const ProvObjects::SubscriberDevice &device,
									  Poco::Logger &logger, const SyncOptions &options) {
	auto &inventoryDB = StorageService()->InventoryDB();
	ProvObjects::InventoryTag inventoryRecord;
	if (!inventoryDB.GetRecord("serialNumber", device.serialNumber, inventoryRecord)) {
		poco_debug(logger, fmt::format("[SUBSCRIBER_PROVISIONING]: Inventory not found for serial "
									   "[{}].",
									   device.serialNumber));
		return true;
	}

	SubscriberContext subscriberContext;
	const auto contextLoaded =
		ResolveSubscriberContext(device.subscriberId, options.createVenueIfMissing, options.actor,
								 subscriberContext, logger);

	const auto previousVenue = inventoryRecord.venue;
	const auto previousEntity = inventoryRecord.entity;
	const auto previousConfiguration = inventoryRecord.deviceConfiguration;
	bool updated = false;

	if (inventoryRecord.subscriber != device.subscriberId) {
		inventoryRecord.subscriber = device.subscriberId;
		updated = true;
	}

	if (contextLoaded && !subscriberContext.venueRecord.info.id.empty() &&
		inventoryRecord.venue != subscriberContext.venueRecord.info.id) {
		inventoryRecord.venue = subscriberContext.venueRecord.info.id;
		updated = true;
	}

	if (!inventoryRecord.venue.empty() && !inventoryRecord.entity.empty()) {
		inventoryRecord.entity.clear();
		updated = true;
	}

	if (inventoryRecord.deviceConfiguration != device.configurationId) {
		inventoryRecord.deviceConfiguration = device.configurationId;
		updated = true;
	}

	if (updated) {
		inventoryRecord.info.modified = Utils::Now();
		if (!inventoryDB.UpdateRecord("id", inventoryRecord.info.id, inventoryRecord)) {
			poco_error(logger, fmt::format("[SUBSCRIBER_PROVISIONING]: Failed to update inventory "
										   "[{}] for serial [{}].",
										   inventoryRecord.info.id, device.serialNumber));
			return false;
		}

		ManageMembership(StorageService()->VenueDB(), &ProvObjects::Venue::devices, previousVenue,
						 inventoryRecord.venue, inventoryRecord.info.id);
		ManageMembership(StorageService()->EntityDB(), &ProvObjects::Entity::devices, previousEntity,
						 inventoryRecord.entity, inventoryRecord.info.id);
		MoveUsage(StorageService()->ConfigurationDB(), StorageService()->InventoryDB(),
				  previousConfiguration, inventoryRecord.deviceConfiguration,
				  inventoryRecord.info.id);
		SDK::GW::Device::SetOwnerShip(options.client, inventoryRecord.serialNumber,
									  inventoryRecord.entity, inventoryRecord.venue,
									  inventoryRecord.subscriber);
	}

	std::string deviceGroup = device.deviceGroup;
	Poco::toLowerInPlace(deviceGroup);
	if (options.enableMonitoringForOlg && deviceGroup == "olg") {
		const auto venueId = !subscriberContext.venueRecord.info.id.empty()
								 ? subscriberContext.venueRecord.info.id
								 : inventoryRecord.venue;
		const auto venueNameHint = contextLoaded ? subscriberContext.venueName : std::string{};
		if (!EnsureMonitoringStartedForVenue(venueId, venueNameHint, logger)) {
			return false;
		}
	}

	return true;
}

bool SyncInventoryForSerialNumber(const std::string &serialNumber, Poco::Logger &logger,
								  const SyncOptions &options) {
	ProvObjects::SubscriberDevice subscriberDevice;
	if (!StorageService()->SubscriberDeviceDB().GetRecord("serialNumber", serialNumber,
														  subscriberDevice)) {
		return true;
	}
	return SyncInventoryForSubscriberDevice(subscriberDevice, logger, options);
}

bool StopMonitoringForSubscriberDevice(const ProvObjects::SubscriberDevice &device,
									   Poco::Logger &logger) {
	SubscriberContext subscriberContext;
	std::string venueId;

	if (ResolveSubscriberContext(device.subscriberId, false, nullptr, subscriberContext, logger)) {
		venueId = subscriberContext.venueRecord.info.id;
	}

	if (venueId.empty()) {
		ProvObjects::InventoryTag inventoryRecord;
		if (StorageService()->InventoryDB().GetRecord("serialNumber", device.serialNumber,
													  inventoryRecord)) {
			venueId = inventoryRecord.venue;
		}
	}

	return StopMonitoringForVenue(venueId, logger);
}

} // namespace OpenWifi::SubscriberProvisioning
