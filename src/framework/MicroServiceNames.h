/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
//
// Created by stephane bourque on 2022-10-25.
//

#pragma once

#include <string>

namespace OpenWifi {

	static const std::string uSERVICE_SECURITY{"owsec"};
	static const std::string uSERVICE_GATEWAY{"owgw"};
	static const std::string uSERVICE_FIRMWARE{"owfms"};
	static const std::string uSERVICE_TOPOLOGY{"owtopo"};
	static const std::string uSERVICE_PROVISIONING{"owprov"};
	static const std::string uSERVICE_OWLS{"owls"};
	static const std::string uSERVICE_SUBCRIBER{"owsub"};
	static const std::string uSERVICE_INSTALLER{"owinst"};
	static const std::string uSERVICE_ANALYTICS{"owanalytics"};
	static const std::string uSERVICE_OWRRM{"owrrm"};

	// Microservice that exposes REST-APIs and get response from openlan-cgw
#ifdef CGW_INTEGRATION
    static const std::string uSERVICE_CGW{"cgw-rest"};
#endif
} // namespace OpenWifi