//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#pragma once
#include "StorageService.h"
#include "framework/RESTAPI_Handler.h"

namespace OpenWifi {
	class RESTAPI_venue_handler : public RESTAPIHandler {
	  public:
		RESTAPI_venue_handler(const RESTAPIHandler::BindingMap &bindings, Poco::Logger &L,
							  RESTAPI_GenericServerAccounting &Server, uint64_t TransactionId,
							  bool Internal)
			: RESTAPIHandler(bindings, L,
							 std::vector<std::string>{Poco::Net::HTTPRequest::HTTP_GET,
													  Poco::Net::HTTPRequest::HTTP_POST,
													  Poco::Net::HTTPRequest::HTTP_PUT,
													  Poco::Net::HTTPRequest::HTTP_DELETE,
													  Poco::Net::HTTPRequest::HTTP_OPTIONS},
							 Server, TransactionId, Internal) {}
		static auto PathName() { return std::list<std::string>{"/api/v1/venue/{uuid}"}; };

	  private:
		VenueDB &DB_ = StorageService()->VenueDB();
		void DoGet() final;
		void DoPost() final;
		void DoPut() final;
		void DoDelete() final;
		bool CanAssociateContact(const std::string &ContactId,
							 const ProvObjects::Venue &TargetVenue);
		bool CanAssociateLocation(const std::string &LocationId,
							  const ProvObjects::Venue &TargetVenue,
							  const std::string &AuthorizationVenue);

		template <typename T>
		bool IdExists(T &DB, const std::string &Field, const RESTAPI::Errors::msg &Error) {
			if (!Field.empty() && !DB.Exists("id", Field)) {
				BadRequest(Error);
				return false;
			}
			return true;
		}
	};
} // namespace OpenWifi
