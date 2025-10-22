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

#include "Poco/JSON/Object.h"
#include "Poco/Net/HTTPServerResponse.h"

#include "framework/OpenWifiTypes.h"

namespace OpenWifi {

	class OpenAPIRequestGet {
	  public:
		explicit OpenAPIRequestGet(const std::string &Type, const std::string &EndPoint,
								   const Types::StringPairVec &QueryData, uint64_t msTimeout,
								   const std::string &LoggingStr = "")
			: Type_(Type), EndPoint_(EndPoint), QueryData_(QueryData), msTimeout_(msTimeout),
			  LoggingStr_(LoggingStr){};
		Poco::Net::HTTPServerResponse::HTTPStatus Do(Poco::JSON::Object::Ptr &ResponseObject,
													 const std::string &BearerToken = "");

	  private:
		std::string Type_;
		std::string EndPoint_;
		Types::StringPairVec QueryData_;
		uint64_t msTimeout_;
		std::string LoggingStr_;
	};

	class OpenAPIRequestPut {
	  public:
		explicit OpenAPIRequestPut(const std::string &Type, const std::string &EndPoint,
								   const Types::StringPairVec &QueryData,
								   const Poco::JSON::Object &Body, uint64_t msTimeout,
								   const std::string &LoggingStr = "")
			: Type_(Type), EndPoint_(EndPoint), QueryData_(QueryData), msTimeout_(msTimeout),
			  Body_(Body), LoggingStr_(LoggingStr){};

		Poco::Net::HTTPServerResponse::HTTPStatus Do(Poco::JSON::Object::Ptr &ResponseObject,
													 const std::string &BearerToken = "");

	  private:
		std::string Type_;
		std::string EndPoint_;
		Types::StringPairVec QueryData_;
		uint64_t msTimeout_;
		Poco::JSON::Object Body_;
		std::string LoggingStr_;
	};

	class OpenAPIRequestPost {
	  public:
		explicit OpenAPIRequestPost(const std::string &Type, const std::string &EndPoint,
									const Types::StringPairVec &QueryData,
									const Poco::JSON::Object &Body, uint64_t msTimeout,
									const std::string &LoggingStr = "")
			: Type_(Type), EndPoint_(EndPoint), QueryData_(QueryData), msTimeout_(msTimeout),
			  Body_(Body), LoggingStr_(LoggingStr){};
		Poco::Net::HTTPServerResponse::HTTPStatus Do(Poco::JSON::Object::Ptr &ResponseObject,
													 const std::string &BearerToken = "");

	  private:
		std::string Type_;
		std::string EndPoint_;
		Types::StringPairVec QueryData_;
		uint64_t msTimeout_;
		Poco::JSON::Object Body_;
		std::string LoggingStr_;
	};

	class OpenAPIRequestDelete {
	  public:
#ifdef CGW_INTEGRATION
		// CGW build: support DELETE with or without JSON body
		explicit OpenAPIRequestDelete(const std::string &Type, const std::string &EndPoint,
									  const Types::StringPairVec &QueryData,
									  const Poco::JSON::Object &Body, uint64_t msTimeout,
									  const std::string &LoggingStr = "")
			: Type_(Type), EndPoint_(EndPoint), QueryData_(QueryData), msTimeout_(msTimeout), Body_(Body),
			  LoggingStr_(LoggingStr){};

		explicit OpenAPIRequestDelete(const std::string &Type, const std::string &EndPoint,
									  const Types::StringPairVec &QueryData, uint64_t msTimeout,
									  const std::string &LoggingStr = "")
			: Type_(Type), EndPoint_(EndPoint), QueryData_(QueryData), msTimeout_(msTimeout),
			  LoggingStr_(LoggingStr){};
#else
		// Non-CGW build: classic constructor, no body
		explicit OpenAPIRequestDelete(const std::string &Type, const std::string &EndPoint,
									  const Types::StringPairVec &QueryData, uint64_t msTimeout,
									  const std::string &LoggingStr = "")
			: Type_(Type), EndPoint_(EndPoint), QueryData_(QueryData), msTimeout_(msTimeout),
			  LoggingStr_(LoggingStr){};
#endif
		Poco::Net::HTTPServerResponse::HTTPStatus Do(const std::string &BearerToken = "");

	  private:
		std::string Type_;
		std::string EndPoint_;
		Types::StringPairVec QueryData_;
		uint64_t msTimeout_;
		Poco::JSON::Object Body_;
		std::string LoggingStr_;
	};

} // namespace OpenWifi
