//
// Created by stephane bourque on 2021-09-29.
//

#pragma once

#include "framework/OpenWifiTypes.h"
#include "framework/SubSystemServer.h"

#include "Poco/JSON/Object.h"

namespace OpenWifi {

	class AutoDiscovery : public SubSystemServer {
	  public:
		static auto instance() {
			static auto instance_ = new AutoDiscovery;
			return instance_;
		}

		int Start() override;
		void Stop() override;
		Types::KafkaHandlerOutcome ConnectionReceived(const std::string &Key, const std::string &Payload, std::string *FailureReason = nullptr);

	  private:
		uint64_t ConnectionWatcherId_ = 0;
		std::atomic_bool Running_ = false;

        void ProcessPing(const Poco::JSON::Object::Ptr & P, std::string &FW, std::string &SN,
                                        std::string &Compat, std::string &Conn, std::string &locale) ;
        void ProcessConnect(const Poco::JSON::Object::Ptr & P, std::string &FW, std::string &SN,
                         std::string &Compat, std::string &Conn, std::string &locale) ;
        void ProcessDisconnect(const Poco::JSON::Object::Ptr & P, std::string &FW, std::string &SN,
                            std::string &Compat, std::string &Conn, std::string &locale) ;

        AutoDiscovery() noexcept
			: SubSystemServer("AutoDiscovery", "AUTO-DISCOVERY", "discovery") {}
	};

	inline auto AutoDiscovery() { return AutoDiscovery::instance(); }

} // namespace OpenWifi
