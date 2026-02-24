/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#pragma once

#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "RESTObjects/RESTAPI_SecurityObjects.h"

namespace Poco {
class Logger;
}

namespace OpenWifi {
class RESTAPIHandler;

namespace SubscriberProvisioning {

	struct SyncOptions {
		bool createVenueIfMissing = false;
		bool enableMonitoringForOlg = true;
		RESTAPIHandler *client = nullptr;
		const SecurityObjects::UserInfo *actor = nullptr;
	};

	bool SyncInventoryForSubscriberDevice(const ProvObjects::SubscriberDevice &device,
										  Poco::Logger &logger,
										  const SyncOptions &options = {});

	bool SyncInventoryForSerialNumber(const std::string &serialNumber, Poco::Logger &logger,
									  const SyncOptions &options = {});

	bool StopMonitoringForSubscriberDevice(const ProvObjects::SubscriberDevice &device,
										   Poco::Logger &logger);

} // namespace SubscriberProvisioning
} // namespace OpenWifi
