//
// Created by stephane bourque on 2022-01-11.
//

#include "SDK_gw.h"

#include "framework/MicroServiceNames.h"
#include "framework/OpenAPIRequests.h"
#include "framework/utils.h"

namespace OpenWifi::SDK::GW {
	namespace Device {
		void Reboot(RESTAPIHandler *client, const std::string &Mac,
					[[maybe_unused]] uint64_t When) {
			std::string EndPoint = "/api/v1/device/" + Mac + "/reboot";
			Poco::JSON::Object ObjRequest;

			ObjRequest.set("serialNumber", Mac);
			ObjRequest.set("when", 0);
			PerformCommand(client, "reboot", EndPoint, ObjRequest);
		}

		bool Reboot(const std::string &Mac, [[maybe_unused]] uint64_t When) {
			std::string EndPoint = "/api/v1/device/" + Mac + "/reboot";
			Poco::JSON::Object ObjRequest;

			ObjRequest.set("serialNumber", Mac);
			ObjRequest.set("when", When);

			OpenAPIRequestPost RebootCommand(uSERVICE_GATEWAY, EndPoint, {}, ObjRequest, 30000);

			Poco::JSON::Object::Ptr Response;
			return RebootCommand.Do(Response) == Poco::Net::HTTPResponse::HTTP_OK;
		}

		void LEDs(RESTAPIHandler *client, const std::string &Mac, uint64_t When, uint64_t Duration,
				  const std::string &Pattern) {
			std::string EndPoint = "/api/v1/device/" + Mac + "/leds";
			Poco::JSON::Object ObjRequest;

			std::cout << "M:" << Mac << " P:" << Pattern << std::endl;

			ObjRequest.set("serialNumber", Mac);
			ObjRequest.set("when", When);
			ObjRequest.set("duration", Duration);
			ObjRequest.set("pattern", Pattern);
			PerformCommand(client, "leds", EndPoint, ObjRequest);
		}

		void Factory(RESTAPIHandler *client, const std::string &Mac, uint64_t When,
					 bool KeepRedirector) {
			std::string EndPoint = "/api/v1/device/" + Mac + "/factory";
			Poco::JSON::Object ObjRequest;

			ObjRequest.set("serialNumber", Mac);
			ObjRequest.set("when", When);
			ObjRequest.set("keepRedirector", KeepRedirector);
			PerformCommand(client, "factory", EndPoint, ObjRequest);
		}

		void Upgrade(RESTAPIHandler *client, const std::string &Mac, uint64_t When,
					 const std::string &ImageName, [[maybe_unused]] bool KeepRedirector) {
			std::string EndPoint = "/api/v1/device/" + Mac + "/upgrade";
			Poco::JSON::Object ObjRequest;

			ObjRequest.set("serialNumber", Mac);
			ObjRequest.set("when", When);
			ObjRequest.set("uri", ImageName);
			PerformCommand(client, "upgrade", EndPoint, ObjRequest);
		}

		void Refresh(RESTAPIHandler *client, const std::string &Mac, uint64_t When) {
			std::string EndPoint = "/api/v1/device/" + Mac + "/refresh";
			Poco::JSON::Object ObjRequest;

			ObjRequest.set("serialNumber", Mac);
			ObjRequest.set("when", When);
			PerformCommand(client, "refresh", EndPoint, ObjRequest);
		}

		bool Upgrade(RESTAPIHandler *client, const std::string &SerialNumber, uint64_t When,
					 const std::string &ImageName, std::string &status) {
			Poco::JSON::Object Body;

			Body.set(RESTAPI::Protocol::SERIALNUMBER, SerialNumber);
			Body.set(RESTAPI::Protocol::URI, ImageName);
			Body.set(uCentralProtocol::WHEN, When);
			OpenWifi::OpenAPIRequestPost API(OpenWifi::uSERVICE_GATEWAY,
											 "/api/v1/device/" + SerialNumber + "/upgrade", {},
											 Body, 10000);
			auto CallResponse = Poco::makeShared<Poco::JSON::Object>();
			auto ResponseStatus =
				API.Do(CallResponse, client ? client->UserInfo_.webtoken.access_token_ : "");
			if (ResponseStatus == Poco::Net::HTTPResponse::HTTP_OK) {
                status = CallResponse->get("status").toString();
				return true;
			}
			return false;
		}

		void PerformCommand(RESTAPIHandler *client, const std::string &Command,
							const std::string &EndPoint, Poco::JSON::Object &CommandRequest) {
			auto API = OpenAPIRequestPost(uSERVICE_GATEWAY, EndPoint, {}, CommandRequest, 60000);
			auto CallResponse = Poco::makeShared<Poco::JSON::Object>();

			auto ResponseStatus = API.Do(CallResponse, client->UserInfo_.webtoken.access_token_);
			if (ResponseStatus == Poco::Net::HTTPServerResponse::HTTP_GATEWAY_TIMEOUT) {
				Poco::JSON::Object ResponseObject;
				ResponseObject.set("Code", Poco::Net::HTTPServerResponse::HTTP_GATEWAY_TIMEOUT);
				ResponseObject.set(
					"Details",
					"Command could not complete, you may want to retry this operation later.");
				ResponseObject.set("Operation", Command);
				client->Response->setStatus(ResponseStatus);
				std::stringstream SS;
				Poco::JSON::Stringifier::condense(ResponseObject, SS);
				client->Response->setContentLength(SS.str().size());
				client->Response->setContentType("application/json");
				auto &os = client->Response->send();
				os << SS.str();
			} else {
				client->Response->setStatus(ResponseStatus);
				std::stringstream SS;
				Poco::JSON::Stringifier::condense(CallResponse, SS);
				Poco::JSON::Parser P;
				auto Raw = P.parse(SS.str()).extract<Poco::JSON::Object::Ptr>();
				if (Raw->has("command") && Raw->has("errorCode") && Raw->has("errorText")) {
					Poco::JSON::Object ReturnResponse;
					ReturnResponse.set("Operation", Raw->get("command").toString());
					ReturnResponse.set("Details", Raw->get("errorText").toString());
					ReturnResponse.set("Code", Raw->get("errorCode"));

					std::stringstream Ret;
					Poco::JSON::Stringifier::condense(ReturnResponse, Ret);
					client->Response->setContentLength(Ret.str().size());
					client->Response->setContentType("application/json");
					auto &os = client->Response->send();
					os << Ret.str();
				}
			}
		}

		bool SetVenue(RESTAPIHandler *client, const std::string &SerialNumber,
					  const std::string &uuid) {
			Poco::JSON::Object Body;

			Body.set("serialNumber", SerialNumber);
			Body.set("venue", uuid);
			OpenWifi::OpenAPIRequestPut R(OpenWifi::uSERVICE_GATEWAY,
										  "/api/v1/device/" + SerialNumber, {}, Body, 10000);
			auto CallResponse = Poco::makeShared<Poco::JSON::Object>();
			auto ResponseStatus =
				R.Do(CallResponse, client ? client->UserInfo_.webtoken.access_token_ : "");
			if (ResponseStatus == Poco::Net::HTTPResponse::HTTP_OK) {
				return true;
			}
			return false;
		}

		bool SetEntity(RESTAPIHandler *client, const std::string &SerialNumber,
					   const std::string &uuid) {
			Poco::JSON::Object Body;

			Body.set("serialNumber", SerialNumber);
			Body.set("entity", uuid);
			OpenWifi::OpenAPIRequestPut R(OpenWifi::uSERVICE_GATEWAY,
										  "/api/v1/device/" + SerialNumber, {}, Body, 10000);
			auto CallResponse = Poco::makeShared<Poco::JSON::Object>();
			auto ResponseStatus =
				R.Do(CallResponse, client ? client->UserInfo_.webtoken.access_token_ : "");
			if (ResponseStatus == Poco::Net::HTTPResponse::HTTP_OK) {
				return true;
			}
			return false;
		}

		bool SetSubscriber(RESTAPIHandler *client, const std::string &SerialNumber,
						   const std::string &uuid) {
			Poco::JSON::Object Body;

			Body.set("serialNumber", SerialNumber);
			Body.set("subscriber", uuid);
			OpenWifi::OpenAPIRequestPut R(OpenWifi::uSERVICE_GATEWAY,
										  "/api/v1/device/" + SerialNumber, {}, Body, 10000);
			auto CallResponse = Poco::makeShared<Poco::JSON::Object>();
			auto ResponseStatus =
				R.Do(CallResponse, client ? client->UserInfo_.webtoken.access_token_ : "");
			if (ResponseStatus == Poco::Net::HTTPResponse::HTTP_OK) {
				return true;
			}
			return false;
		}

		bool SetOwnerShip(RESTAPIHandler *client, const std::string &SerialNumber,
						  const std::string &entity, const std::string &venue,
						  const std::string &subscriber) {

			Poco::JSON::Object Body;
			Body.set("serialNumber", SerialNumber);
			Body.set("subscriber", subscriber);
			Body.set("venue", venue);
			Body.set("entity", entity);
			OpenWifi::OpenAPIRequestPut R(OpenWifi::uSERVICE_GATEWAY,
										  "/api/v1/device/" + SerialNumber, {}, Body, 10000);
			auto CallResponse = Poco::makeShared<Poco::JSON::Object>();
			auto ResponseStatus =
				R.Do(CallResponse, client ? client->UserInfo_.webtoken.access_token_ : "");
			if (ResponseStatus == Poco::Net::HTTPResponse::HTTP_OK) {
				return true;
			}
			return false;
		}

		bool Delete(RESTAPIHandler *client, const std::string &SerialNumber) {
			OpenWifi::OpenAPIRequestDelete R(OpenWifi::uSERVICE_GATEWAY,
											 "/api/v1/device/" + SerialNumber, {}, 10000);
			const auto ResponseStatus =
				R.Do(client ? client->UserInfo_.webtoken.access_token_ : "");
			return ResponseStatus == Poco::Net::HTTPResponse::HTTP_OK ||
				   ResponseStatus == Poco::Net::HTTPResponse::HTTP_NO_CONTENT ||
				   ResponseStatus == Poco::Net::HTTPResponse::HTTP_ACCEPTED ||
				   ResponseStatus == Poco::Net::HTTPResponse::HTTP_NOT_FOUND;
		}

		bool Configure(RESTAPIHandler *client, const std::string &SerialNumber,
					   Poco::JSON::Object::Ptr &Configuration, Poco::JSON::Object::Ptr &Response) {

			Poco::JSON::Object Body;

			Poco::JSON::Parser P;
			uint64_t now = Utils::Now();

			Configuration->set("uuid", now);
			Body.set("serialNumber", SerialNumber);
			Body.set("UUID", now);
			Body.set("when", 0);
			Body.set("configuration", Configuration);

			OpenWifi::OpenAPIRequestPost R(OpenWifi::uSERVICE_GATEWAY,
										   "/api/v1/device/" + SerialNumber + "/configure", {},
										   Body, 60000);

			auto ResponseStatus =
				R.Do(Response, client ? client->UserInfo_.webtoken.access_token_ : "");
			if (ResponseStatus == Poco::Net::HTTPResponse::HTTP_OK) {
				std::ostringstream os;
				Poco::JSON::Stringifier::stringify(Response, os);
				return true;
			}
			return false;
		}
	} // namespace Device

    namespace RADIUS {

        bool GetConfiguration(RESTAPIHandler *client, GWObjects::RadiusProxyPoolList &Pools) {
            OpenWifi::OpenAPIRequestGet R(OpenWifi::uSERVICE_GATEWAY,
                                           "/api/v1/radiusProxyConfig", {},
                                           60000);
            auto CallResponse = Poco::makeShared<Poco::JSON::Object>();
            auto ResponseStatus =
                    R.Do(CallResponse, client ? client->UserInfo_.webtoken.access_token_ : "");
            if(ResponseStatus == Poco::Net::HTTPResponse::HTTP_OK) {
                return Pools.from_json(CallResponse);
            }
            return false;
        }

        bool SetConfiguration(RESTAPIHandler *client, const Poco::JSON::Object &Configuration,
                              GWObjects::RadiusProxyPoolList &NewPools, Poco::JSON::Object &ErrorObj) {
            OpenWifi::OpenAPIRequestPut R(OpenWifi::uSERVICE_GATEWAY,
                                          "/api/v1/radiusProxyConfig", {}, Configuration,
                                          60000);
            auto CallResponse = Poco::makeShared<Poco::JSON::Object>();
            auto ResponseStatus =
                    R.Do(CallResponse, client ? client->UserInfo_.webtoken.access_token_ : "");
            ErrorObj = *CallResponse;
            if(ResponseStatus == Poco::Net::HTTPResponse::HTTP_OK) {
                return NewPools.from_json(CallResponse);
            }
            return false;
        }

        bool SetConfiguration(RESTAPIHandler *client, const GWObjects::RadiusProxyPoolList &Pools,
                              GWObjects::RadiusProxyPoolList &NewPools, Poco::JSON::Object &ErrorObj) {
            Poco::JSON::Object  Body;
            Pools.to_json(Body);
            return SetConfiguration(client,Body,NewPools, ErrorObj);
        }

    }

} // namespace OpenWifi::SDK::GW
