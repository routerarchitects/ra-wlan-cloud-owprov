//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#pragma once

#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "framework/orm.h"

namespace OpenWifi {
	typedef Poco::Tuple<std::string, std::string, std::string, std::string, uint64_t, uint64_t,
						std::string, std::string, std::string, std::string, std::string,
						std::string, std::string, std::string, std::string, std::string,
						std::string, std::string, std::string, std::string, std::string,
						std::string, std::string, bool, uint64_t, uint64_t, std::string>
		InventoryDBRecordType;

	class InventoryDB : public ORM::DB<InventoryDBRecordType, ProvObjects::InventoryTag> {
	  public:
		InventoryDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L);
		virtual ~InventoryDB(){};
		bool CreateFromConnection(const std::string &SerialNumber,
								  const std::string &ConnectionInfo, const std::string &DeviceType,
								  const std::string &Locale,
								  const bool isConnection);

		void InitializeSerialCache();
		bool GetRRMDeviceList(Types::UUIDvec_t &DeviceList);

		bool EvaluateDeviceIDRules(const std::string &id, ProvObjects::DeviceRules &Rules);
		bool EvaluateDeviceSerialNumberRules(const std::string &serialNumber,
											 ProvObjects::DeviceRules &Rules);

		inline uint32_t Version() override { return 1; }

		bool Upgrade(uint32_t from, uint32_t &to) override;

        bool GetDevicesForVenue(const std::string &uuid, std::vector<std::string> &devices);
        bool GetDevicesUUIDForVenue(const std::string &uuid, std::vector<std::string> &devices);
        bool GetDevicesForVenue(const std::string &uuid, std::vector<ProvObjects::InventoryTag> &devices);

	  private:
		bool EnsureSerialUniqueIndex();
		bool EvaluateDeviceRules(const ProvObjects::InventoryTag &T,
								 ProvObjects::DeviceRules &Rules);
	};
} // namespace OpenWifi
