/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#pragma once
#include "framework/RESTAPI_Handler.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"

namespace OpenWifi {
	class RESTAPI_subscriber_provision_handler : public RESTAPIHandler {
	  public:
		RESTAPI_subscriber_provision_handler(const RESTAPIHandler::BindingMap &bindings,
											 Poco::Logger &L,
											 RESTAPI_GenericServerAccounting &Server,
											 uint64_t TransactionId, bool Internal)
			: RESTAPIHandler(bindings, L,
							 std::vector<std::string>{Poco::Net::HTTPRequest::HTTP_POST,
													  Poco::Net::HTTPRequest::HTTP_DELETE,
													  Poco::Net::HTTPRequest::HTTP_OPTIONS},
							 Server, TransactionId, Internal) {}
		static auto PathName() {
			return std::list<std::string>{"/api/v1/subscriber/provision",
										  "/api/v1/subscriber/provision/{uuid}"};
		};

	  private:
		struct ProvisionContext {
			ProvObjects::SignupEntry signupRecord{};
			ProvObjects::Operator operatorRecord{};
			ProvObjects::Venue venueRecord{};
			ProvObjects::InventoryTag inventoryRecord{};
			std::string venueName;
			std::string boardId;
			bool enableMonitoring = false;
			uint64_t retention = 604800;
			uint64_t interval = 60;
			bool monitorSubVenues = true;
		};

		bool ParseRequest(const Poco::JSON::Object::Ptr &raw, ProvisionContext &ctx);
		bool LoadSignupRecord(ProvisionContext &ctx);
		bool LoadOperatorRecord(ProvisionContext &ctx);
		bool CreateVenueRecord(ProvisionContext &ctx);
		bool UpdateInventoryRecord(ProvisionContext &ctx);
		bool StartMonitoring(ProvisionContext &ctx);
		bool UnlinkInventoryRecord(ProvisionContext &ctx);
		bool StopMonitoring(ProvisionContext &ctx);
		bool DeleteVenueRecord(ProvisionContext &ctx);

		EntityDB &EntityDB_ = StorageService()->EntityDB();
		InventoryDB &InventoryDB_ = StorageService()->InventoryDB();
		OperatorDB &OperatorDB_ = StorageService()->OperatorDB();
		SignupDB &SignupDB_ = StorageService()->SignupDB();
		VenueDB &VenueDB_ = StorageService()->VenueDB();

		void DoDelete() final;
		void DoGet() final {};
		void DoPost() final;
		void DoPut() final {};
	};
} // namespace OpenWifi
