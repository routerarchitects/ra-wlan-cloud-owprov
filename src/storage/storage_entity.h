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
						std::string,
						std::string>
		EntityDBRecordType;

	class EntityDB : public ORM::DB<EntityDBRecordType, ProvObjects::Entity> {
	  public:
		EntityDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L);
		virtual ~EntityDB(){};
		static inline bool IsRoot(const std::string &UUID) { return (UUID == RootUUID_); }
		static inline const std::string RootUUID() { return RootUUID_; }
		void BuildTree(Poco::JSON::Object &Tree, const std::string &Node = RootUUID_);
		void AddVenues(Poco::JSON::Object &Tree, const std::string &Venue);
		void ImportTree(const Poco::JSON::Object::Ptr &Ptr, const std::string &Node = RootUUID_);
		void ImportVenues(const Poco::JSON::Object::Ptr &Ptr, const std::string &Node = RootUUID_);
		bool CreateShortCut(ProvObjects::Entity &E);
		bool GetByIP(const std::string &IP, std::string &uuid);
		bool Upgrade(uint32_t from, uint32_t &to) override;
		bool EvaluateDeviceRules(const std::string &id, ProvObjects::DeviceRules &Rules);

	  private:
		inline static const std::string RootUUID_{"0000-0000-0000"};
	};
} // namespace OpenWifi
