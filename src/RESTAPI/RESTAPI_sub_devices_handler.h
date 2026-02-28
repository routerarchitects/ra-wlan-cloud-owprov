//
// Created by stephane bourque on 2022-04-06.
//

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

	  private:
		bool PushConfigurationToDevice(const std::string &SerialNumber);
		bool LoadDeviceFromBinding(ProvObjects::SubscriberDevice &device, bool allowSerialLookup);
		bool ValidateDeleteRequest(const std::string &deviceIdOrSerial,
								   ProvObjects::SubscriberDevice &device);
		bool StartMonitoring(const ProvObjects::SubscriberDevice &newObject);
		bool StopMonitoring(const ProvObjects::SubscriberDevice &existingObject);
		bool ParsePutRequest(const std::string &uuid,
							 SubscriberDeviceDB::RecordName &existingObject,
							 SubscriberDeviceDB::RecordName &updateObject);
		bool ParsePostRequest(SubscriberDeviceDB::RecordName &newObject);
		bool ValidateRequestFields(const SubscriberDeviceDB::RecordName &deviceObject);
		bool CreateConfigurationRecord(SubscriberDeviceDB::RecordName &newObject);
		bool UpdateConfiguration(const std::string &uuid,
								 SubscriberDeviceDB::RecordName &existingObject,
								 SubscriberDeviceDB::RecordName &updateObject,
								 const Poco::JSON::Object::Ptr &rawObject,
								 bool &hasUpdatedConfiguration);
		void CleanupSubscriberConfigurationRecord(const std::string &deviceConfiguration,
													  const std::string &inventoryId);
		bool ValidateDeviceGroupForUpdate(SubscriberDeviceDB::RecordName &existingObject,
										  const SubscriberDeviceDB::RecordName &updateObject);
		bool ValidateDeviceGroup(SubscriberDeviceDB::RecordName &newObject);
		bool ApplyPutRequest(const SubscriberDeviceDB::RecordName &updateObject,
							 SubscriberDeviceDB::RecordName &existingObject,
							 const Poco::JSON::Object::Ptr &rawObject);
		bool CreateInventoryRecord(const SubscriberDeviceDB::RecordName &newObject);
		void CleanupInventoryAssociations(const ProvObjects::InventoryTag &inventoryRecord);
		void
		DeleteInventoryForSubscriberDevice(const ProvObjects::SubscriberDevice &existingObject);
		bool CreateSubscriberDeviceRecord(const SubscriberDeviceDB::RecordName &newObject);
		void ReturnSubscriberDeviceObject(const ProvObjects::SubscriberDevice &device);

		SubscriberDeviceDB &DB_ = StorageService()->SubscriberDeviceDB();
		void DoGet() final;
		void DoPost() final;
		void DoPut() final;
		void DoDelete() final;
	};
} // namespace OpenWifi
