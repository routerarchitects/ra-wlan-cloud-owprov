//
// Created by stephane bourque on 2021-09-29.
//

#include "AutoDiscovery.h"
#include "Poco/JSON/Parser.h"
#include "StorageService.h"
#include "Tasks/VenueConfigUpdater.h"
#include "framework/KafkaManager.h"
#include "framework/KafkaTopics.h"
#include "framework/ow_constants.h"

namespace OpenWifi {

	int AutoDiscovery::Start() {
		poco_information(Logger(), "Starting...");
		Running_ = true;
		Types::TopicNotifyFunction F = [this](const std::string &Key, const std::string &Payload) {
			this->ConnectionReceived(Key, Payload);
		};
		ConnectionWatcherId_ = KafkaManager()->RegisterTopicWatcher(KafkaTopics::CONNECTION, F);
		Worker_.start(*this);
		return 0;
	};

	void AutoDiscovery::Stop() {
		poco_information(Logger(), "Stopping...");
		Running_ = false;
		KafkaManager()->UnregisterTopicWatcher(KafkaTopics::CONNECTION, ConnectionWatcherId_);
		Queue_.wakeUpAll();
		Worker_.join();
		poco_information(Logger(), "Stopped...");
	};

    void AutoDiscovery::ProcessPing(const Poco::JSON::Object::Ptr & P, std::string &FW, std::string &SN,
                                    std::string &Compat, std::string &Conn, std::string &locale) {
        if (P->has(uCentralProtocol::CONNECTIONIP))
            Conn = P->get(uCentralProtocol::CONNECTIONIP).toString();
        if (P->has(uCentralProtocol::FIRMWARE))
            FW = P->get(uCentralProtocol::FIRMWARE).toString();
        if (P->has(uCentralProtocol::SERIALNUMBER))
            SN = P->get(uCentralProtocol::SERIALNUMBER).toString();
        if (P->has(uCentralProtocol::COMPATIBLE))
            Compat = P->get(uCentralProtocol::COMPATIBLE).toString();
        if (P->has("locale")) {
            locale = P->get("locale").toString();
        }
    }

    void AutoDiscovery::ProcessConnect(const Poco::JSON::Object::Ptr &P, std::string &FW, std::string &SN,
                                       std::string &Compat, std::string &Conn, std::string &locale) {
        if (P->has(uCentralProtocol::CONNECTIONIP))
            Conn = P->get(uCentralProtocol::CONNECTIONIP).toString();
        if (P->has(uCentralProtocol::FIRMWARE))
            FW = P->get(uCentralProtocol::FIRMWARE).toString();
        if (P->has(uCentralProtocol::SERIALNUMBER))
            SN = P->get(uCentralProtocol::SERIALNUMBER).toString();
        else if (P->has(uCentralProtocol::SERIAL))
            SN = P->get(uCentralProtocol::SERIAL).toString();
        if (P->has("locale")) {
            locale = P->get("locale").toString();
        }
        if(P->has(uCentralProtocol::CAPABILITIES)) {
            auto CapObj = P->getObject(uCentralProtocol::CAPABILITIES);
            if (CapObj->has(uCentralProtocol::COMPATIBLE))
                Compat = CapObj->get(uCentralProtocol::COMPATIBLE).toString();
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

    void AutoDiscovery::run() {
		Poco::AutoPtr<Poco::Notification> Note(Queue_.waitDequeueNotification());
		Utils::SetThreadName("auto-discovery");
		while (Note && Running_) {
			auto Msg = dynamic_cast<DiscoveryMessage *>(Note.get());
			if (Msg != nullptr) {
				try {
					Poco::JSON::Parser Parser;
					auto Object = Parser.parse(Msg->Payload()).extract<Poco::JSON::Object::Ptr>();
                    bool    Connected=true;
                    bool isConnection=false;

					if (Object->has(uCentralProtocol::PAYLOAD)) {
                        auto PayloadObj = Object->getObject(uCentralProtocol::PAYLOAD);
                        std::string ConnectedIP, SerialNumber, Compatible, Firmware, Locale ;
                        if (PayloadObj->has(uCentralProtocol::PING)) {
                            auto PingObj = PayloadObj->getObject("ping");
                            ProcessPing(PingObj, Firmware, SerialNumber, Compatible, ConnectedIP, Locale);
                        } else if(PayloadObj->has("capabilities")) {
                            isConnection=true;
                            ProcessConnect(PayloadObj, Firmware, SerialNumber, Compatible, ConnectedIP, Locale);
                        } else if(PayloadObj->has("disconnection")) {
                            //  we ignore disconnection in provisioning
                            Connected=false;
                            ProcessConnect(PayloadObj, Firmware, SerialNumber, Compatible, ConnectedIP, Locale);
                        } else {
                            poco_debug(Logger(),fmt::format("Unknown message on 'connection' topic: {}",Msg->Payload()));
                        }

                        if (!SerialNumber.empty() && Connected) {
                            StorageService()->InventoryDB().CreateFromConnection(
                                    SerialNumber, ConnectedIP, Compatible, Locale, isConnection);
                            // Now that the entry has been created, we can try to push a config if
                            // the connection was a capabilities message.
                            if (isConnection){
                                ComputeAndPushConfig(SerialNumber, Compatible, Logger());
                            }
                        }
                    }
				} catch (const Poco::Exception &E) {
                    std::cout << "EX:" << Msg->Payload() << std::endl;
					Logger().log(E);
				} catch (...) {
				}
			} else {
			}
			Note = Queue_.waitDequeueNotification();
		}
	}

} // namespace OpenWifi