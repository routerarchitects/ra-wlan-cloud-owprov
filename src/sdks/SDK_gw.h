//
// Created by stephane bourque on 2022-01-11.
//

#pragma once

#include "RESTObjects/RESTAPI_GWobjects.h"
#include "framework/RESTAPI_Handler.h"

namespace OpenWifi::SDK::GW {
	namespace Device {
		void Reboot(RESTAPIHandler *client, const std::string &Mac, uint64_t When);
		bool Reboot(const std::string &Mac, [[maybe_unused]] uint64_t When);
		void LEDs(RESTAPIHandler *client, const std::string &Mac, uint64_t When, uint64_t Duration,
				  const std::string &Pattern);
		void Factory(RESTAPIHandler *client, const std::string &Mac, uint64_t When,
					 bool KeepRedirector);
		void Upgrade(RESTAPIHandler *client, const std::string &Mac, uint64_t When,
					 const std::string &ImageName, bool KeepRedirector);
		void Refresh(RESTAPIHandler *client, const std::string &Mac, uint64_t When);
		void PerformCommand(RESTAPIHandler *client, const std::string &Command,
							const std::string &EndPoint, Poco::JSON::Object &CommandRequest);
		bool Configure(RESTAPIHandler *client, const std::string &Mac,
					   Poco::JSON::Object::Ptr &Configuration, Poco::JSON::Object::Ptr &Response);
		bool Upgrade(RESTAPIHandler *client, const std::string &Mac, uint64_t When,
					 const std::string &ImageName, std::string &status);
		bool Delete(RESTAPIHandler *client, const std::string &SerialNumber);

		bool SetVenue(RESTAPIHandler *client, const std::string &SerialNumber,
					  const std::string &uuid);
		bool SetSubscriber(RESTAPIHandler *client, const std::string &SerialNumber,
						   const std::string &uuid);
		inline bool SetSubscriber(const std::string &SerialNumber, const std::string &uuid) {
			return SetSubscriber(nullptr, SerialNumber, uuid);
		}
		bool SetEntity(RESTAPIHandler *client, const std::string &SerialNumber,
					   const std::string &uuid);
		bool SetOwnerShip(RESTAPIHandler *client, const std::string &SerialNumber,
						  const std::string &entity, const std::string &venue,
						  const std::string &subscriber);
	} // namespace Device
    namespace RADIUS {
        bool GetConfiguration(RESTAPIHandler *client, GWObjects::RadiusProxyPoolList &Pools);
        bool SetConfiguration(RESTAPIHandler *client, const GWObjects::RadiusProxyPoolList &Pools,
                              GWObjects::RadiusProxyPoolList &NewPools, Poco::JSON::Object &ErrorObj);
        bool SetConfiguration(RESTAPIHandler *client, const Poco::JSON::Object &Configuration,
                              GWObjects::RadiusProxyPoolList &NewPools, Poco::JSON::Object &ErrorObj);
    }
} // namespace OpenWifi::SDK::GW
