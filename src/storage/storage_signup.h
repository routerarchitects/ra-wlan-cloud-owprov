//
// Created by stephane bourque on 2022-02-21.
//

#pragma once

#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "framework/orm.h"

namespace OpenWifi {
	typedef Poco::Tuple<std::string, std::string, std::string, std::string, uint64_t, uint64_t,
						std::string, std::string, std::string, std::string, uint64_t, uint64_t,
						std::string, uint64_t, uint64_t, std::string, std::string, std::string,
						std::string>
		SignupDBRecordType;

	class SignupDB : public ORM::DB<SignupDBRecordType, ProvObjects::SignupEntry> {
	  public:
		explicit SignupDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L) noexcept;
		virtual ~SignupDB(){};
		bool GetIncompleteSignups(SignupDB::RecordVec &Signups);
		void RemoveIncompleteSignups();

	  private:
	};
} // namespace OpenWifi
