//
// Created by stephane bourque on 2022-04-05.
//

#pragma once

#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "framework/orm.h"

namespace OpenWifi {

	typedef Poco::Tuple<std::string, std::string, std::string, std::string, uint64_t, uint64_t,
						std::string, std::string, std::string, std::string, bool, std::string,
						std::string, std::string>
		OperatorDBRecordType;

	class OperatorDB : public ORM::DB<OperatorDBRecordType, ProvObjects::Operator> {
	  public:
		OperatorDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L);
		virtual ~OperatorDB(){};
		bool GetByIP(const std::string &IP, std::string &uuid);
		bool Upgrade(uint32_t from, uint32_t &to) override;

	  private:
	};
} // namespace OpenWifi
