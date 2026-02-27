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
			bool PushConfigurationToDevice(const std::string &SerialNumber);
			bool PushConfigurationIfInInventory(const std::string &SerialNumber);

	  private:
		bool LoadDeviceFromBinding(ProvObjects::SubscriberDevice &device, bool allowSerialLookup);
		bool ValidateDeleteRequest(const std::string &deviceIdOrSerial,
								   ProvObjects::SubscriberDevice &device);
		bool ParseAndValidatePutRequest(const std::string &uuid,
										SubscriberDeviceDB::RecordName &existingObject,
										SubscriberDeviceDB::RecordName &updateObject);
		bool ParseAndValidatePostRequest(SubscriberDeviceDB::RecordName &newObject);
		bool ValidateAndNormalizeDeviceGroupForUpdate(
			SubscriberDeviceDB::RecordName &existingObject,
			const SubscriberDeviceDB::RecordName &updateObject,
			const Poco::JSON::Object::Ptr &rawObject);
		bool ValidateAndNormalizeDeviceGroup(SubscriberDeviceDB::RecordName &newObject);
		bool HasPutRequestChanges(const SubscriberDeviceDB::RecordName &existingObject,
								  const SubscriberDeviceDB::RecordName &updateObject,
								  const Poco::JSON::Object::Ptr &rawObject) const;
		bool HasPutConfigurationChange(const SubscriberDeviceDB::RecordName &existingObject,
									  const SubscriberDeviceDB::RecordName &updateObject,
									  const Poco::JSON::Object::Ptr &rawObject) const;
		bool ApplyPutRequest(const SubscriberDeviceDB::RecordName &updateObject,
							 SubscriberDeviceDB::RecordName &existingObject,
							 const Poco::JSON::Object::Ptr &rawObject);
		bool EnsureInventoryRecordForSubscriberDevice(
			const SubscriberDeviceDB::RecordName &newObject);
		void ApplyPostCreateSync(const SubscriberDeviceDB::RecordName &newObject);
		void ApplyPostUpdateSync(const SubscriberDeviceDB::RecordName &beforeUpdate,
								 const SubscriberDeviceDB::RecordName &afterUpdate);
		bool StopMonitoringForDeleteIfNeeded(const ProvObjects::SubscriberDevice &existingObject,
											 bool &monitoringStopped);
		void RestoreMonitoringAfterDeleteFailure(
			const ProvObjects::SubscriberDevice &existingObject);
		void CleanupInventoryAssociations(const ProvObjects::InventoryTag &inventoryRecord);
		void DeleteInventoryForSubscriberDevice(
			const ProvObjects::SubscriberDevice &existingObject);
		bool DeleteSubscriberDevice(const ProvObjects::SubscriberDevice &existingObject);
		bool CreateSubscriberDeviceRecord(const SubscriberDeviceDB::RecordName &newObject);
		void ReturnSubscriberDeviceObject(const ProvObjects::SubscriberDevice &device);
		void ReturnSubscriberDeviceRecord(const std::string &uuid);

		SubscriberDeviceDB &DB_ = StorageService()->SubscriberDeviceDB();
		void DoGet() final;
		void DoPost() final;
		void DoPut() final;
		void DoDelete() final;
	};
} // namespace OpenWifi
