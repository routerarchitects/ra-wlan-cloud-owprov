//
// Created by stephane bourque on 2022-10-26.
//

#include "framework/EventBusManager.h"
#include "framework/KafkaManager.h"
#include "framework/MicroServiceFuncs.h"
#include "framework/utils.h"

namespace OpenWifi {

	void EventBusManager::run() {
		Running_ = true;
		Utils::SetThreadName("fmwk:EventMgr");

		// Redis registration is refreshed before Kafka JOIN/KEEP_ALIVE, and deleted
		// before LEAVE, to keep the shared snapshot consistent when Redis is available.
		//
		// Redis failures are caught and logged, allowing Kafka event submission to
		// proceed afterward. These calls remain synchronous, so slow Redis responses
		// can delay Kafka events. openwifi.redis.timeout configures connection and
		// receive timeouts, not a strict end-to-end operation deadline.
		MicroServiceRefreshRedisRegistration();
		auto Msg = (MicroServiceMakeSystemEventMessage(KafkaTopics::ServiceEvents::EVENT_JOIN));
		KafkaManager()->PostMessage(KafkaTopics::SERVICE_EVENTS, MicroServicePrivateEndPoint(), Msg,
									false);
		while (Running_) {
			if(!Poco::Thread::trySleep((unsigned long)MicroServiceDaemonBusTimer())) {
                break;
            }
			MicroServiceRefreshRedisRegistration();
			Msg = (MicroServiceMakeSystemEventMessage(KafkaTopics::ServiceEvents::EVENT_KEEP_ALIVE));
			KafkaManager()->PostMessage(KafkaTopics::SERVICE_EVENTS, MicroServicePrivateEndPoint(),
										Msg, false);
		}
		MicroServiceDeleteRedisRegistration();
		Msg = (MicroServiceMakeSystemEventMessage(KafkaTopics::ServiceEvents::EVENT_LEAVE));
		KafkaManager()->PostMessage(KafkaTopics::SERVICE_EVENTS, MicroServicePrivateEndPoint(), Msg,
									false);
	};

	void EventBusManager::Start() {
		poco_information(Logger_, "Starting...");
		if (KafkaManager()->Enabled()) {
			Thread_.start(*this);
		}
	}

	void EventBusManager::Stop() {
		if (KafkaManager()->Enabled()) {
			poco_information(Logger_, "Stopping...");
			Running_ = false;
			Thread_.wakeUp();
			Thread_.join();
			poco_information(Logger_, "Stopped...");
		}
	}

} // namespace OpenWifi