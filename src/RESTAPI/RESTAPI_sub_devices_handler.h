/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#pragma once
#include "StorageService.h"
#include "framework/RESTAPI_Handler.h"

namespace OpenWifi {
	class RESTAPI_sub_devices_handler : public RESTAPIHandler {
	  public:
		RESTAPI_sub_devices_handler(const RESTAPIHandler::BindingMap &bindings, Poco::Logger &L,
									RESTAPI_GenericServerAccounting &Server, uint64_t TransactionId,
									bool Internal)
			: RESTAPIHandler(bindings, L,
							 std::vector<std::string>{Poco::Net::HTTPRequest::HTTP_GET,
													  Poco::Net::HTTPRequest::HTTP_POST,
													  Poco::Net::HTTPRequest::HTTP_PUT,
													  Poco::Net::HTTPRequest::HTTP_DELETE,
													  Poco::Net::HTTPRequest::HTTP_OPTIONS},
							 Server, TransactionId, Internal) {}
		static auto PathName() {
			return std::list<std::string>{"/api/v1/subscriberDevice/{uuid}"};
		};
		bool ApplyConfiguration(const std::string &SerialNumber);

	  private:
		SubscriberDeviceDB &DB_ = StorageService()->SubscriberDeviceDB();
		void DoGet() final;
		void DoPost() final;
		void DoPut() final;
		void DoDelete() final;
	};
} // namespace OpenWifi
