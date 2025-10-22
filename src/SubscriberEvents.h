/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
#pragma once
#ifdef CGW_INTEGRATION

#include "framework/SubSystemServer.h"
#include "framework/OpenWifiTypes.h"

#include "Poco/Notification.h"
#include "Poco/NotificationQueue.h"
#include "Poco/JSON/Object.h"

namespace OpenWifi {


    class SubscriberEventMessage : public Poco::Notification {
      public:
        explicit SubscriberEventMessage(const std::string &Key, const std::string &Payload)
            : Key_(Key), Payload_(Payload) {}
        const std::string &Key() { return Key_; }
        const std::string &Payload() { return Payload_; }

      private:
        std::string Key_;
        std::string Payload_;
    };

    /**
     * @brief Subsystem that listens for subscriber events and processes them asynchronously.
     *
     * Maintains a worker thread that dequeues incoming events and forwards them to
     * the provisioning backend.
     */
    class SubscriberEvents : public SubSystemServer, Poco::Runnable {
      public:
        static auto instance() {
            static auto instance_ = new SubscriberEvents;
            return instance_;
        }

        /**
         * @brief Start the subscriber event worker thread and register the watcher.
         *
         * @return 0 on success, negative value on failure.
         */
        int Start() override;
        /**
         * @brief Stop the worker thread and unregister the watcher.
         */
        void Stop() override;

        /**
         * @brief Queue an event for asynchronous processing.
         *
         * @param Key identifier for the event source or type.
         * @param Payload serialized event payload.
         */
        void EventReceived(const std::string &Key, const std::string &Payload) {
            std::lock_guard G(Mutex_);
            Queue_.enqueueNotification(new SubscriberEventMessage(Key, Payload));
        }

        /**
        * @brief Main worker loop that drains subscriber-event notifications.
        *
        * The loop blocks on the internal notification queue, parses each JSON payload,
        * and identifies the event `type` and `subscriberid`. For create events it
        * inserts a venue-to-group mapping in the GroupsMap database, attempts to
        * provision the group(call cgw-rest api) in CGW, and rolls back the DB entry if CGW provisioning
        * errors do not terminate the worker thread.
        * fails. For delete events it looks up the persisted mapping, triggers CGW
        * deletion, and removes the mapping from storage.
        */
        void run() override;

      private:
        uint64_t WatcherId_ = 0;
        Poco::NotificationQueue Queue_;
        Poco::Thread Worker_;
        std::atomic_bool Running_ = false;

        SubscriberEvents() noexcept
            : SubSystemServer("SubscriberEvents", "SUBSCRIBER-EVENTS", "subscriber-events") {}
    };

    /**
     * @brief Convenience accessor for the subscriber events subsystem singleton.
     *
     * @return pointer to the singleton instance.
     */
    inline auto SubscriberEventsProcessor() { return SubscriberEvents::instance(); }

} // namespace OpenWifi

#endif // CGW_INTEGRATION
