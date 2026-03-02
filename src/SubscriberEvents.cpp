/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
#ifdef CGW_INTEGRATION

#include "SubscriberEvents.h"

#include "Poco/JSON/Parser.h"
#include "Poco/Logger.h"
#include "fmt/format.h"

#include "StorageService.h"
#include "framework/KafkaManager.h"
#include "framework/KafkaTopics.h"
#include "InfraGroupEvents.h"

namespace OpenWifi {

	void SubscriberEvents::HandleSubscriberDelete(const std::string &subscriberId) {
		auto &db = StorageService()->GroupsMapDB();
		OpenWifi::GroupsMapRecord rec{};
		if (!db.GetRecord("venueid", subscriberId, rec)) {
			poco_error(Logger(), fmt::format("No groupsmap entry found for subscriber {} on delete event", subscriberId));
			return;
		}

		poco_information(Logger(), fmt::format("Deleting CGW group {} for subscriber {}", rec.groupid, subscriberId));
		PublishInfraGroupEvent("infrastructure_group_delete", rec.groupid);
	}

	void SubscriberEvents::HandleSubscriberCreate(const std::string &subscriberId) {
		auto &db = StorageService()->GroupsMapDB();
		if (db.Exists("venueid", subscriberId)) {
			poco_debug(Logger(), fmt::format("Subscriber {} already in groupsmap; skipping CGW create", subscriberId));
			return;
		}

		uint64_t groupId = 0;
		if (!db.AddVenue(subscriberId, groupId)) {
			poco_error(Logger(), fmt::format("Failed to create groupsmap entry for subscriber {}", subscriberId));
			return;
		}

		poco_information(Logger(), fmt::format("Created groupsmap entry for subscriber {} with group {}", subscriberId, groupId));
		PublishInfraGroupEvent("infrastructure_group_create", groupId);
	}

	static inline Poco::JSON::Object::Ptr ExtractPayloadOrSelf(const Poco::JSON::Object::Ptr &Obj) {
		if (Obj && Obj->has("payload")) {
			try {
				return Obj->getObject("payload");
			} catch (...) {
				return Obj;
			}
		}
		return Obj;
	}

	int SubscriberEvents::Start() {
		poco_information(Logger(), "Starting... SubscriberEvents");
		Running_ = true;

		Types::TopicNotifyFunction F = [this](const std::string &Key, const std::string &Payload) {
			this->EventReceived(Key, Payload);
		};
		WatcherId_ = KafkaManager()->RegisterTopicWatcher(KafkaTopics::SUBSCRIBER_EVENT, F);
		Worker_.start(*this);
		return 0;
	}

	void SubscriberEvents::Stop() {
		poco_information(Logger(), "Stopping...SubscriberEvents");
		Running_ = false;
		KafkaManager()->UnregisterTopicWatcher(KafkaTopics::SUBSCRIBER_EVENT, WatcherId_);
		Queue_.wakeUpAll();
		Worker_.join();
		poco_information(Logger(), "Stopped...");
	}

	void SubscriberEvents::run() {
		Utils::SetThreadName("subscriber-events");
		Poco::AutoPtr<Poco::Notification> Note(Queue_.waitDequeueNotification());
		while (Note && Running_) {
			auto *Msg = dynamic_cast<SubscriberEventMessage *>(Note.get());
			if (!Msg) {
				Note = Queue_.waitDequeueNotification();
				continue;
			}

			try {
				Poco::JSON::Parser parser;
				auto root = parser.parse(Msg->Payload()).extract<Poco::JSON::Object::Ptr>();
				auto obj = ExtractPayloadOrSelf(root);
				if (obj.isNull()) {
					Note = Queue_.waitDequeueNotification();
					continue;
				}

				const std::string type = obj->has("type") ? obj->get("type").toString() : "";
				const std::string subscriberId = obj->has("subscriberid") ? obj->get("subscriberid").toString() : "";
				if (subscriberId.empty()) {
					Note = Queue_.waitDequeueNotification();
					continue;
				}

				if (type == "infrastructure_subscriber_create") {
					HandleSubscriberCreate(subscriberId);
				} else if (type == "infrastructure_subscriber_delete") {
					HandleSubscriberDelete(subscriberId);
				}
			} catch (const std::exception &e) {
				poco_error(Logger(), fmt::format("Subscriber Event std::exception {}", e.what()));
			}
		
			Note = Queue_.waitDequeueNotification();
		}
	}

} // namespace OpenWifi

#endif // CGW_INTEGRATION
