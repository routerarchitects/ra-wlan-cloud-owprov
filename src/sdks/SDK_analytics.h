//
// Created by stephane bourque on 2025-01-01.
//

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
