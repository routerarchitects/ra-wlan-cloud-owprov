/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#include "SDK_analytics.h"

#include "framework/MicroServiceNames.h"
#include "framework/OpenAPIRequests.h"

namespace OpenWifi::SDK::Analytics {

	bool StartMonitoring(const Poco::JSON::Object &Body,
					 Poco::JSON::Object::Ptr &ResponseObject,
					 Poco::Net::HTTPServerResponse::HTTPStatus &Status) {
		auto API = OpenAPIRequestPost(uSERVICE_ANALYTICS, "/api/v1/board/0", {}, Body, 60000);
		ResponseObject = Poco::makeShared<Poco::JSON::Object>();
		Status = API.Do(ResponseObject);
		return Status == Poco::Net::HTTPResponse::HTTP_OK;
	}

	bool StopMonitoring(const std::string &BoardId,
						Poco::Net::HTTPServerResponse::HTTPStatus &Status) {
		auto API = OpenAPIRequestDelete(uSERVICE_ANALYTICS, "/api/v1/board/" + BoardId, {}, 60000);
		Status = API.Do();
		return Status == Poco::Net::HTTPResponse::HTTP_OK;
	}

} // namespace OpenWifi::SDK::Analytics
