//
// Created for OpenWifi Provisioning.
//

#pragma once

#include "framework/RESTAPI_Handler.h"

namespace OpenWifi {

	class RESTAPI_authorization_deviceCommand_handler : public RESTAPIHandler {
	  public:
		RESTAPI_authorization_deviceCommand_handler(const RESTAPIHandler::BindingMap &bindings,
													Poco::Logger &L,
													RESTAPI_GenericServerAccounting &Server,
													uint64_t TransactionId, bool Internal)
			: RESTAPIHandler(bindings, L,
							 std::vector<std::string>{Poco::Net::HTTPRequest::HTTP_POST,
													  Poco::Net::HTTPRequest::HTTP_OPTIONS},
							 Server, TransactionId, Internal) {}

		static auto PathName() {
			return std::list<std::string>{"/api/v1/authorization/deviceCommand"};
		};

	  private:
		void DoPost() final;
		void DoGet() final {}
		void DoPut() final {}
		void DoDelete() final {}
	};

} // namespace OpenWifi
