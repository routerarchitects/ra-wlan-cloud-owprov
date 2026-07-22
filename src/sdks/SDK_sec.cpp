//
// Created by stephane bourque on 2022-01-11.
//

#include "SDK_sec.h"
#include "framework/MicroServiceNames.h"
#include "framework/OpenAPIRequests.h"

namespace OpenWifi::SDK::Sec {

	namespace User {
		bool Exists([[maybe_unused]] RESTAPIHandler *client, const Types::UUID_t &Id) {
			OpenAPIRequestGet Req(uSERVICE_SECURITY, "/api/v1/user/" + Id, {}, 5000);

			auto CallResponse = Poco::makeShared<Poco::JSON::Object>();
			auto StatusCode = Req.Do(CallResponse);
			if (StatusCode == Poco::Net::HTTPResponse::HTTP_OK) {
				return true;
			}
			return false;
		}

		bool Get([[maybe_unused]] RESTAPIHandler *client, const Types::UUID_t &Id,
				 SecurityObjects::UserInfo &UserInfo) {
			OpenAPIRequestGet Req(uSERVICE_SECURITY, "/api/v1/user/" + Id, {}, 5000);

			auto CallResponse = Poco::makeShared<Poco::JSON::Object>();
			auto StatusCode = Req.Do(CallResponse);
			if (StatusCode == Poco::Net::HTTPResponse::HTTP_OK) {
				return UserInfo.from_json(CallResponse);
			}
			return false;
		}
	} // namespace User

	namespace Subscriber {
		bool Exists([[maybe_unused]] RESTAPIHandler *client, const Types::UUID_t &Id) {
			OpenAPIRequestGet Req(uSERVICE_SECURITY, "/api/v1/subuser/" + Id, {}, 5000);

			auto CallResponse = Poco::makeShared<Poco::JSON::Object>();
			auto StatusCode = Req.Do(CallResponse);
			if (StatusCode == Poco::Net::HTTPResponse::HTTP_OK) {
				return true;
			}
			return false;
		}

		bool Get([[maybe_unused]] RESTAPIHandler *client, const Types::UUID_t &Id,
				 SecurityObjects::UserInfo &UserInfo) {
			OpenAPIRequestGet Req(uSERVICE_SECURITY, "/api/v1/subuser/" + Id, {}, 5000);

			auto CallResponse = Poco::makeShared<Poco::JSON::Object>();
			auto StatusCode = Req.Do(CallResponse);
			if (StatusCode == Poco::Net::HTTPResponse::HTTP_OK) {
				return UserInfo.from_json(CallResponse);
			}
			return false;
		}

		bool Delete([[maybe_unused]] RESTAPIHandler *client, const Types::UUID_t &Id) {
			OpenAPIRequestDelete Req(uSERVICE_SECURITY, "/api/v1/subuser/" + Id, {}, 5000);

			auto StatusCode = Req.Do();
			if (StatusCode == Poco::Net::HTTPResponse::HTTP_OK ||
				StatusCode == Poco::Net::HTTPResponse::HTTP_NO_CONTENT ||
				StatusCode == Poco::Net::HTTPResponse::HTTP_ACCEPTED) {
				return true;
			}
			return false;
		}

		bool Search([[maybe_unused]] RESTAPIHandler *client, const std::string &OperatorId,
					const std::string &Name, const std::string &EMail,
					SecurityObjects::UserInfoList &Users) {
			OpenAPIRequestGet Req(
				uSERVICE_SECURITY, "/api/v1/subusers",
				{{"operatorId", OperatorId}, {"nameSearch", Name}, {"emailSearch", EMail}}, 5000);
			auto CallResponse = Poco::makeShared<Poco::JSON::Object>();
			auto StatusCode = Req.Do(CallResponse);
			if (StatusCode == Poco::Net::HTTPResponse::HTTP_OK) {
				return Users.from_json(CallResponse);
			}
			return false;
		}

	} // namespace Subscriber

} // namespace OpenWifi::SDK::Sec
