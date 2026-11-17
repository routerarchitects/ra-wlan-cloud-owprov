//
// Created by stephane bourque on 2022-04-28.
//

#include "ProvWebSocketClient.h"

#include "SerialNumberCache.h"
#include "StorageService.h"
#include "framework/UI_WebSocketClientServer.h"
#include "sdks/SDK_sec.h"

namespace OpenWifi {

	ProvWebSocketClient::ProvWebSocketClient(Poco::Logger &Logger) : Logger_(Logger) {
		UI_WebSocketClientServer()->SetProcessor(this);
	}

	ProvWebSocketClient::~ProvWebSocketClient() {
		UI_WebSocketClientServer()->SetProcessor(nullptr);
	}

	void ProvWebSocketClient::ws_command_serial_number_search(const Poco::JSON::Object::Ptr &O,
															  bool &Done, std::string &Answer) {
		Done = false;
		auto Prefix = ORM::Escape(O->get("serial_prefix").toString());
		Poco::toLowerInPlace(Prefix);
		Logger().information(Poco::format("serial_number_search: %s", Prefix));
		if (!Prefix.empty()) {
			const std::size_t maxLen = (Prefix.front() == '*') ? 31 : 30;
			if (Prefix.length() <= maxLen) {
				std::vector<std::string> Numbers;
				SerialNumberCache()->FindNumbers(Prefix, 50, Numbers);
				Poco::JSON::Array Arr;
				for (const auto &i : Numbers)
					Arr.add(i);
				Poco::JSON::Object RetObj;
				RetObj.set("serialNumbers", Arr);
				std::ostringstream SS;
				Poco::JSON::Stringifier::stringify(RetObj, SS);
				Answer = SS.str();
			}
		}
	}

	void ProvWebSocketClient::ws_command_address_completion(const Poco::JSON::Object::Ptr &O,
															bool &Done, std::string &Answer) {
		Done = false;
		auto Address = O->get("address").toString();
		Answer = GoogleGeoCodeCall(Address);
	}

	void ProvWebSocketClient::ws_command_exit([[maybe_unused]] const Poco::JSON::Object::Ptr &O,
											  bool &Done, std::string &Answer) {
		Done = true;
		Answer = R"lit({ "closing" : "Goodbye! Aurevoir! Hasta la vista!" })lit";
	}

	void ProvWebSocketClient::ws_command_invalid([[maybe_unused]] const Poco::JSON::Object::Ptr &O,
												 bool &Done, std::string &Answer) {
		Done = false;
		Answer = std::string{R"lit({ "error" : "invalid command" })lit"};
	}

	void ProvWebSocketClient::ws_command_subuser_search(const Poco::JSON::Object::Ptr &O,
														bool &Done, std::string &Answer) {
		Done = false;
		auto operatorId = ORM::Escape(O->get("operatorId").toString());
		std::string nameSearch, emailSearch;
		OpenWifi::RESTAPIHandler::AssignIfPresent(O, "nameSearch", nameSearch);
		OpenWifi::RESTAPIHandler::AssignIfPresent(O, "emailSearch", emailSearch);
		SecurityObjects::UserInfoList Users;
		SDK::Sec::Subscriber::Search(nullptr, operatorId, nameSearch, emailSearch, Users);

		Poco::JSON::Array Arr;
		for (const auto &i : Users.users) {
			Poco::JSON::Object OO;
			OO.set("name", i.name);
			OO.set("email", i.email);
			OO.set("id", i.id);
			i.to_json(OO);
			Arr.add(OO);
		}
		Poco::JSON::Object ObjAnswer;
		ObjAnswer.set("users", Arr);
		std::ostringstream SS;
		Poco::JSON::Stringifier::stringify(ObjAnswer, SS);
		Answer = SS.str();
	}

	void ProvWebSocketClient::ws_command_subdevice_search(const Poco::JSON::Object::Ptr &O,
														  bool &Done, std::string &Answer) {
		Done = false;
		auto operatorId = O->get("operatorId").toString();
		auto Prefix = O->get("serial_prefix").toString();
		Poco::toLowerInPlace(Prefix);
		std::string Query;

		if (Prefix[0] == '*') {
			Query = fmt::format(" operatorId='{}' and (right(serialNumber,{})='{}' or "
								"right(realMacAddress,{})='{}' ) ",
								operatorId, Prefix.size() - 1, Prefix.substr(1), Prefix.size() - 1,
								Prefix.substr(1));
		} else {
			Query = fmt::format(" operatorId='{}' and (left(serialNumber,{})='{}'  or "
								"left(realMacAddress,{})='{}' ) ",
								operatorId, Prefix.size(), Prefix, Prefix.size(), Prefix);
		}

		std::vector<ProvObjects::SubscriberDevice> SubDevices;

		StorageService()->SubscriberDeviceDB().GetRecords(0, 200, SubDevices, Query);
		Poco::JSON::Array Arr;
		for (const auto &i : SubDevices) {
			Arr.add(i.serialNumber);
		}
		Poco::JSON::Object RetObj;
		RetObj.set("serialNumbers", Arr);
		std::ostringstream SS;
		Poco::JSON::Stringifier::stringify(RetObj, SS);
		Answer = SS.str();
	}

	void
	ProvWebSocketClient::Processor(const Poco::JSON::Object::Ptr &O, std::string &Result,
								   bool &Done,
								   [[maybe_unused]] const SecurityObjects::UserInfo &UserInfo) {
		try {
			if (O->has("command") && O->has("id")) {
				auto id = (uint64_t)O->get("id");
				std::string Answer;
				auto Command = O->get("command").toString();
				if (Command == "serial_number_search" && O->has("serial_prefix")) {
					ws_command_serial_number_search(O, Done, Answer);
				} else if (UI_WebSocketClientServer()->GeoCodeEnabled() &&
						   Command == "address_completion" && O->has("address")) {
					ws_command_address_completion(O, Done, Answer);
				} else if (UI_WebSocketClientServer()->GeoCodeEnabled() &&
						   Command == "subuser_search" && O->has("operatorId")) {
					ws_command_subuser_search(O, Done, Answer);
				} else if (UI_WebSocketClientServer()->GeoCodeEnabled() &&
						   Command == "subdevice_search" && O->has("operatorId") &&
						   O->has("serial_prefix")) {
					ws_command_subdevice_search(O, Done, Answer);
				} else if (Command == "exit") {
					ws_command_exit(O, Done, Answer);
				} else {
					ws_command_invalid(O, Done, Answer);
				}

				Result = fmt::format("{{ \"command_response_id\" : {} , \"response\" : {}  }}", id,
									 Answer);
			}
		} catch (const Poco::Exception &E) {
			Logger().log(E);
		}
	}

	std::string ProvWebSocketClient::GoogleGeoCodeCall(const std::string &A) {
		try {
			std::string URI = {"https://maps.googleapis.com/maps/api/geocode/json"};
			Poco::URI uri(URI);

			uri.addQueryParameter("address", A);
			uri.addQueryParameter("key", UI_WebSocketClientServer()->GoogleApiKey());

			Poco::Net::HTTPSClientSession session(uri.getHost(), uri.getPort());
			Poco::Net::HTTPRequest req(Poco::Net::HTTPRequest::HTTP_GET, uri.getPathAndQuery(),
									   Poco::Net::HTTPMessage::HTTP_1_1);
			session.sendRequest(req);
			Poco::Net::HTTPResponse res;
			std::istream &rs = session.receiveResponse(res);
			if (res.getStatus() == Poco::Net::HTTPResponse::HTTP_OK) {
				std::ostringstream os;
				Poco::StreamCopier::copyStream(rs, os);
				return os.str();
			} else {
				std::ostringstream os;
				Poco::StreamCopier::copyStream(rs, os);
				return R"lit({ "error: )lit" + os.str() + R"lit( })lit";
			}
		} catch (...) {
		}
		return "{ \"error\" : \"No call made\" }";
	}

} // namespace OpenWifi