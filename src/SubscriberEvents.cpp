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
#include "sdks/SDK_cgw.h"

namespace OpenWifi {

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
			auto Msg = dynamic_cast<SubscriberEventMessage *>(Note.get());
			if (Msg != nullptr) {
				try {
					Poco::JSON::Parser Parser;
					auto Root = Parser.parse(Msg->Payload()).extract<Poco::JSON::Object::Ptr>();
					auto Obj = ExtractPayloadOrSelf(Root);
					if (!Obj.isNull()) {
						std::string Type{};
						std::string SubscriberId{};
						if (Obj->has("type"))
							Type = Obj->get("type").toString();
						if (Obj->has("subscriberid"))
							SubscriberId = Obj->get("subscriberid").toString();

						if (!SubscriberId.empty() && Type == "infrastructure_subscriber_create") {
							bool exists =StorageService()->GroupsMapDB().Exists("venueid", SubscriberId);
							if (!exists) {
								uint64_t groupId = 0;
								if (StorageService()->GroupsMapDB().AddVenue(SubscriberId,groupId)) {
									poco_information(Logger(),fmt::format("Created groupsmap entry for subscriber {} with group {}", SubscriberId, groupId));
									if (!SDK::CGW::CreateGroup(groupId)) {
										poco_warning(Logger(),fmt::format("CGW CreateGroup failed for subscriber {}(group {}), rolling back groupsmap entry",SubscriberId, groupId));
										if (!StorageService()->GroupsMapDB().DeleteVenue(SubscriberId)) {
											poco_warning(Logger(),fmt::format("Rollback failed: could not delete groupsmap entry for subscriber {}",SubscriberId));
										}
									}
								} else {
									poco_warning(Logger(),fmt::format("Failed to create groupsmap entry for subscriber {}",SubscriberId));
								}
							} else {
								poco_debug(Logger(),fmt::format("Subscriber {} already in groupsmap; skipping CGW create",SubscriberId));
							}
						} else if (!SubscriberId.empty() && Type == "infrastructure_subscriber_delete") {
							OpenWifi::GroupsMapRecord rec{};
							if (StorageService()->GroupsMapDB().GetRecord("venueid", SubscriberId,rec)) {
								poco_information(Logger(), fmt::format("Deleting CGW group {} for subscriber {}",rec.groupid, SubscriberId));
								if (SDK::CGW::DeleteGroup(rec.groupid)) {
									if (StorageService()->GroupsMapDB().DeleteVenue(SubscriberId)) {
										poco_information(Logger(),fmt::format("Deleted groupsmap entry for subscriber {} group {}",SubscriberId, rec.groupid));
									} else {
										poco_warning(Logger(),fmt::format("Failed to delete groupsmap entry for subscriber {} after CGW deletion",SubscriberId));
									}
								} else {
									poco_warning(Logger(),fmt::format("CGW DeleteGroup failed for subscriber {} (group {})",SubscriberId, rec.groupid));
								}
							} else {
								poco_warning(Logger(), fmt::format("No groupsmap entry found for subscriber {} on delete event",SubscriberId));
							}
						}
					}
				} catch (const Poco::Exception &E) {
					Logger().log(E);
				}
			}
			Note = Queue_.waitDequeueNotification();
		}
	}

} // namespace OpenWifi

#endif // CGW_INTEGRATION
