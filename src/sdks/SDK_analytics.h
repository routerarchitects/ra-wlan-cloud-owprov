/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#pragma once

#include "Poco/JSON/Object.h"
#include "Poco/Net/HTTPServerResponse.h"
#include "framework/RESTAPI_Handler.h"

namespace OpenWifi::SDK::Analytics {
	bool StartMonitoring(const Poco::JSON::Object &Body,
					 Poco::JSON::Object::Ptr &ResponseObject,
					 Poco::Net::HTTPServerResponse::HTTPStatus &Status);
	bool StopMonitoring(const std::string &BoardId,
						Poco::Net::HTTPServerResponse::HTTPStatus &Status);
} // namespace OpenWifi::SDK::Analytics
