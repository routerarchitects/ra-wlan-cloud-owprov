//
// Created by stephane bourque on 2021-09-29.
//

#include "AutoDiscovery.h"
#include "Poco/JSON/Parser.h"
#include "StorageService.h"
#include "framework/KafkaManager.h"
#include "framework/KafkaTopics.h"
#include "framework/ow_constants.h"
#include "framework/utils.h"
#include "fmt/format.h"

namespace OpenWifi {

	int AutoDiscovery::Start() {
		poco_information(Logger(), "Starting...");
		Running_ = true;
		ConnectionWatcherId_ = KafkaManager()->RegisterTopicWatcher(
			KafkaTopics::CONNECTION,
			[this](const std::string &Key, const std::string &Payload, std::string &FailureReason) {
				return this->ConnectionReceived(Key, Payload, &FailureReason);
			},
			KafkaConsumerType::Group);
		return 0;
	}

	void AutoDiscovery::Stop() {
		poco_information(Logger(), "Stopping...");
		Running_ = false;
		if (ConnectionWatcherId_ != 0) {
			KafkaManager()->UnregisterTopicWatcher(KafkaTopics::CONNECTION, ConnectionWatcherId_, KafkaConsumerType::Group);
			ConnectionWatcherId_ = 0;
		}
		poco_information(Logger(), "Stopped...");
	}

	namespace {
		void ReadStringField(const Poco::JSON::Object::Ptr &Obj, const char *Field, std::string &Out) {
			if (!Obj->has(Field)) return;
			const auto &Val = Obj->get(Field);
			if (!Val.isString()) {
				throw Poco::BadCastException(fmt::format("JSON field '{}' must be a string", Field));
			}
			Out = Val.convert<std::string>();
		}
	}

    void AutoDiscovery::ProcessPing(const Poco::JSON::Object::Ptr & P, std::string &FW, std::string &SN,
                                    std::string &Compat, std::string &Conn, std::string &locale) {
        ReadStringField(P, uCentralProtocol::CONNECTIONIP, Conn);
        ReadStringField(P, uCentralProtocol::FIRMWARE, FW);
        ReadStringField(P, uCentralProtocol::SERIALNUMBER, SN);
        ReadStringField(P, uCentralProtocol::COMPATIBLE, Compat);
        ReadStringField(P, "locale", locale);
    }

    void AutoDiscovery::ProcessConnect(const Poco::JSON::Object::Ptr &P, std::string &FW, std::string &SN,
                                       std::string &Compat, std::string &Conn, std::string &locale) {
        ReadStringField(P, uCentralProtocol::CONNECTIONIP, Conn);
        ReadStringField(P, uCentralProtocol::FIRMWARE, FW);
        if (P->has(uCentralProtocol::SERIALNUMBER))
            ReadStringField(P, uCentralProtocol::SERIALNUMBER, SN);
        else
            ReadStringField(P, uCentralProtocol::SERIAL, SN);
        ReadStringField(P, "locale", locale);
        if (P->has(uCentralProtocol::CAPABILITIES)) {
            if (!P->isObject(uCentralProtocol::CAPABILITIES)) {
                throw Poco::BadCastException("JSON field 'capabilities' must be an object");
            }
            auto CapObj = P->getObject(uCentralProtocol::CAPABILITIES);
            if (!CapObj) {
                throw Poco::BadCastException("JSON field 'capabilities' is null");
            }
            ReadStringField(CapObj, uCentralProtocol::COMPATIBLE, Compat);
        }
    }

    void AutoDiscovery::ProcessDisconnect(const Poco::JSON::Object::Ptr &P, [[maybe_unused]] std::string &FW,
                                            std::string &SN,
                                          [[maybe_unused]] std::string &Compat,
                                          [[maybe_unused]] std::string &Conn,
                                          [[maybe_unused]] std::string &locale) {
        if (P->has(uCentralProtocol::SERIALNUMBER))
            SN = P->get(uCentralProtocol::SERIALNUMBER).toString();
    }

	Types::KafkaHandlerOutcome AutoDiscovery::ConnectionReceived(const std::string &Key, const std::string &Payload, std::string *FailureReason) {
		poco_trace(Logger(), Poco::format("Device(%s): Connection/Ping message.", Key));

		auto fail = [&](Types::KafkaHandlerOutcome Outcome, std::string Msg, bool Warn = false) {
			if (Warn) {
				poco_warning(Logger(), Msg);
			} else {
				poco_error(Logger(), Msg);
			}
			if (FailureReason) {
				*FailureReason = std::move(Msg);
			}
			return Outcome;
		};

		bool isConnection = false;
		std::string ConnectedIP, SerialNumber, Compatible, Firmware, Locale;
		try {
			Poco::JSON::Parser Parser;
			auto Parsed = Parser.parse(Payload);
			if (Parsed.type() != typeid(Poco::JSON::Object::Ptr)) {
				return fail(Types::KafkaHandlerOutcome::NonRetryableFailure,
					fmt::format("Invalid non-object JSON payload on 'connection' topic, Key: {}", Key));
			}
			auto Object = Parsed.extract<Poco::JSON::Object::Ptr>();
			if (!Object || !Object->has(uCentralProtocol::PAYLOAD) || !Object->isObject(uCentralProtocol::PAYLOAD)) {
				return fail(Types::KafkaHandlerOutcome::NonRetryableFailure,
					fmt::format("Missing/invalid payload on 'connection' topic, Key: {}", Key));
			}
			auto PayloadObj = Object->getObject(uCentralProtocol::PAYLOAD);
			if (!PayloadObj) {
				return fail(Types::KafkaHandlerOutcome::NonRetryableFailure,
					fmt::format("Null payload on 'connection' topic, Key: {}", Key));
			}

			if (PayloadObj->has(uCentralProtocol::PING)) {
				if (!PayloadObj->isObject(uCentralProtocol::PING)) {
					return fail(Types::KafkaHandlerOutcome::NonRetryableFailure,
						fmt::format("Invalid non-object 'ping' field on 'connection' topic, Key: {}", Key));
				}
				auto PingObj = PayloadObj->getObject("ping");
				if (!PingObj) {
					return fail(Types::KafkaHandlerOutcome::NonRetryableFailure,
						fmt::format("Null 'ping' object on 'connection' topic, Key: {}", Key));
				}
				ProcessPing(PingObj, Firmware, SerialNumber, Compatible, ConnectedIP, Locale);
			} else if (PayloadObj->has("capabilities")) {
				isConnection = true;
				ProcessConnect(PayloadObj, Firmware, SerialNumber, Compatible, ConnectedIP, Locale);
			} else if (PayloadObj->has("disconnection")) {
				return Types::KafkaHandlerOutcome::Success;
			} else {
				poco_information(Logger(), fmt::format("Ignored unknown event on 'connection' topic: {}", Payload));
				return Types::KafkaHandlerOutcome::Success;
			}

			const std::string LowerSerial = Poco::toLower(SerialNumber);
			if (LowerSerial.empty() || LowerSerial.size() > 16 || !Utils::ValidSerialNumber(LowerSerial)) {
				return fail(Types::KafkaHandlerOutcome::NonRetryableFailure,
					fmt::format("Invalid or malformed serial number '{}' on 'connection' topic, Key: {}", SerialNumber, Key));
			}
			try {
				(void)std::stoull(LowerSerial, nullptr, 16);
				std::string rev(LowerSerial.rbegin(), LowerSerial.rend());
				(void)std::stoull(rev, nullptr, 16);
			} catch (...) {
				return fail(Types::KafkaHandlerOutcome::NonRetryableFailure,
					fmt::format("Out of range hex serial number '{}' on 'connection' topic, Key: {}", SerialNumber, Key));
			}
		} catch (const Poco::Exception &E) {
			return fail(Types::KafkaHandlerOutcome::NonRetryableFailure,
				fmt::format("Invalid 'connection' JSON payload, Key: {}: {}", Key, E.displayText()));
		} catch (const std::exception &E) {
			return fail(Types::KafkaHandlerOutcome::RetryableFailure,
				fmt::format("ConnectionReceived parse error: {}", E.what()));
		} catch (...) {
			return fail(Types::KafkaHandlerOutcome::RetryableFailure,
				"ConnectionReceived parse unknown error");
		}

		try {
			if (!StorageService()->InventoryDB().CreateFromConnection(SerialNumber, ConnectedIP, Compatible, Locale, isConnection)) {
				return fail(Types::KafkaHandlerOutcome::RetryableFailure,
					fmt::format("CreateFromConnection failed for serial: {}", SerialNumber), true);
			}

			return Types::KafkaHandlerOutcome::Success;
		} catch (const Poco::Exception &E) {
			return fail(Types::KafkaHandlerOutcome::RetryableFailure,
				fmt::format("ConnectionReceived error: {}", E.displayText()));
		} catch (const std::exception &E) {
			return fail(Types::KafkaHandlerOutcome::RetryableFailure,
				fmt::format("ConnectionReceived error: {}", E.what()));
		} catch (...) {
			return fail(Types::KafkaHandlerOutcome::RetryableFailure,
				"ConnectionReceived unknown error");
		}
	}

} // namespace OpenWifi
