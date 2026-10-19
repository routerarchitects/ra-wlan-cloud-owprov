//
// Created by stephane bourque on 2022-10-25.
//

#pragma once

#include <condition_variable>
#include <mutex>
#include <set>
#include <string>

#include "Poco/Notification.h"
#include "Poco/NotificationQueue.h"
#include "Poco/JSON/Object.h"
#include "framework/KafkaTopics.h"
#include "framework/OpenWifiTypes.h"
#include "framework/SubSystemServer.h"
#include "framework/utils.h"

#include "cppkafka/cppkafka.h"

namespace OpenWifi {

	enum class KafkaConsumerType {
		Group,
		Broadcast
	};

	class KafkaMessage : public Poco::Notification {
	  public:
		KafkaMessage(const char * Topic, const std::string &Key, const std::string &Payload)
			: Topic_(Topic), Key_(Key), Payload_(Payload) {}

		inline const char * Topic() { return Topic_; }
		inline const std::string &Key() { return Key_; }
		inline const std::string &Payload() { return Payload_; }

	  private:
		const char *Topic_;
		std::string Key_;
		std::string Payload_;
	};

	class KafkaProducer : public Poco::Runnable {
	  public:
		void run() override;
		void Start();
		void Stop();
		void Produce(const char *Topic, const std::string &Key, const std::string & Payload);

		[[nodiscard]] std::string GetClientId() const;
		[[nodiscard]] static std::string GenerateClientId(const std::string &BaseClientId,
		                                                  const std::string &RuntimeInstanceId);
		[[nodiscard]] bool IsRunning() const { return Running_; }

	  private:
		std::mutex Mutex_;
		Poco::Thread Worker_;
		mutable std::atomic_bool Running_ = false;
		Poco::NotificationQueue Queue_;
	};

	class KafkaConsumer : public Poco::Runnable {
	  public:
		explicit KafkaConsumer(KafkaConsumerType Type = KafkaConsumerType::Group);
		void Start();
		void Stop();

		[[nodiscard]] KafkaConsumerType ConsumerType() const { return ConsumerType_; }
		[[nodiscard]] std::string GetGroupId() const;
		[[nodiscard]] std::string GetClientId() const;
		[[nodiscard]] std::string GetAutoOffsetReset() const;
		[[nodiscard]] static std::string GenerateGroupId(KafkaConsumerType Type,
		                                                 const std::string &BaseGroupId,
		                                                 const std::string &RuntimeInstanceId);
		[[nodiscard]] static std::string GenerateClientId(KafkaConsumerType Type,
		                                                  const std::string &BaseClientId,
		                                                  const std::string &RuntimeInstanceId);
		[[nodiscard]] static std::string GenerateAutoOffsetReset(KafkaConsumerType Type);

		[[nodiscard]] bool IsRunning() const { return Running_; }
		[[nodiscard]] std::set<std::string> Topics() const;

	  private:
		KafkaConsumerType 		ConsumerType_ = KafkaConsumerType::Group;
		mutable std::mutex 		ConsumerMutex_;
		std::condition_variable TopicCV_;
		Types::NotifyTable 		Notifiers_;
		Poco::Thread 			Worker_;
		mutable std::atomic_bool Running_ = false;
		uint64_t 				FunctionId_ = 1;
		std::unique_ptr<cppkafka::Consumer> 			Consumer_;
		std::unique_ptr<cppkafka::ConsumerDispatcher> 	Dispatcher_;
		std::set<std::string>	Topics_;

		void run() override;
		friend class KafkaManager;
		std::uint64_t RegisterTopicWatcher(const std::string &Topic, Types::TopicNotifyFunction &F);
		void UnregisterTopicWatcher(const std::string &Topic, uint64_t Id);
	};

	class KafkaManager : public SubSystemServer {
	  public:
		friend class KafkaConsumer;
		friend class KafkaProducer;

		inline void initialize(Poco::Util::Application &self) override;

		static auto instance() {
			static auto instance_ = new KafkaManager;
			return instance_;
		}

		int Start() override;
		void Stop() override;

		void PostMessage(const char *topic, const std::string &key,
						 const std::string &PayLoad, bool WrapMessage = true);
		void PostMessage(const char *topic, const std::string &key,
						 const Poco::JSON::Object &Object, bool WrapMessage = true);

		[[nodiscard]] std::string WrapSystemId(const std::string & PayLoad);
		[[nodiscard]] inline bool Enabled() const { return KafkaEnabled_; }

		inline std::uint64_t RegisterTopicWatcher(const std::string &Topic,
		                                          Types::TopicNotifyFunction &F,
		                                          KafkaConsumerType ConsumerType) {
			return (ConsumerType == KafkaConsumerType::Broadcast)
				? BroadcastConsumerThr_.RegisterTopicWatcher(Topic, F)
				: GroupConsumerThr_.RegisterTopicWatcher(Topic, F);
		}

		inline void UnregisterTopicWatcher(const std::string &Topic,
		                                  uint64_t Id,
		                                  KafkaConsumerType ConsumerType) {
			if (ConsumerType == KafkaConsumerType::Broadcast) {
				BroadcastConsumerThr_.UnregisterTopicWatcher(Topic, Id);
			} else {
				GroupConsumerThr_.UnregisterTopicWatcher(Topic, Id);
			}
		}

		[[nodiscard]] KafkaConsumer &GroupConsumer() { return GroupConsumerThr_; }
		[[nodiscard]] KafkaConsumer &BroadcastConsumer() { return BroadcastConsumerThr_; }
		[[nodiscard]] KafkaProducer &Producer() { return ProducerThr_; }

		std::uint64_t KafkaManagerMaximumPayloadSize() const { return MaxPayloadSize_; }

	  private:
		bool KafkaEnabled_ = false;
		std::string SystemInfoWrapper_;
		KafkaProducer ProducerThr_;
		KafkaConsumer GroupConsumerThr_{KafkaConsumerType::Group};
		KafkaConsumer BroadcastConsumerThr_{KafkaConsumerType::Broadcast};
		std::uint64_t MaxPayloadSize_ = 250000;

		void PartitionAssignment(const cppkafka::TopicPartitionList &partitions);
		void PartitionRevocation(const cppkafka::TopicPartitionList &partitions);

		KafkaManager() noexcept : SubSystemServer("KafkaManager", "KAFKA-SVR", "openwifi.kafka") {}
	};

	inline auto KafkaManager() { return KafkaManager::instance(); }

} // namespace OpenWifi
