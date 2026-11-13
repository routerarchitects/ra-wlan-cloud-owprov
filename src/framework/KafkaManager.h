//
// Created by stephane bourque on 2022-10-25.
//

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

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

	inline constexpr int32_t KAFKA_PARTITION_DEFAULT = 0;
	inline constexpr int32_t KAFKA_PARTITION_UNASSIGNED = -1;

	class KafkaMessage : public Poco::Notification {
	  public:
		KafkaMessage(const char * Topic, const std::string &Key, const std::string &Payload, int32_t Partition = KAFKA_PARTITION_DEFAULT)
			: Topic_(Topic), Key_(Key), Payload_(Payload), Partition_(Partition) {}

		inline const char * Topic() const { return Topic_; }
		inline const std::string &Key() const { return Key_; }
		inline const std::string &Payload() const { return Payload_; }
		inline int32_t Partition() const { return Partition_; }

	  private:
		const char *Topic_;
		std::string Key_;
		std::string Payload_;
		int32_t Partition_ = KAFKA_PARTITION_DEFAULT;
	};

	class KafkaProducer : public Poco::Runnable {
	  public:
		void run() override;
		void Start();
		void Stop();
		void Produce(const char *Topic, const std::string &Key, const std::string & Payload, int32_t Partition = KAFKA_PARTITION_DEFAULT);

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
		[[nodiscard]] bool HasFailed() const { return Failed_; }
		[[nodiscard]] std::string FailureReason() const;
		[[nodiscard]] std::string StatusString() const;
		[[nodiscard]] std::set<std::string> Topics() const;
		[[nodiscard]] std::vector<std::string> Subscriptions() const;
		[[nodiscard]] bool HasPartitionAssignment() const { return Assigned_; }
		bool WaitForPartitionAssignment(uint64_t TimeoutMs = 5000) const;
		[[nodiscard]] int64_t GetCommittedOffset(const std::string &Topic, int Partition = 0) const;

		using AckCallback = std::function<Types::KafkaHandlerOutcome(const std::string &, const std::string &, std::string &)>;

	  private:
		KafkaConsumerType 		ConsumerType_ = KafkaConsumerType::Group;
		mutable std::mutex 		ConsumerMutex_;
		std::condition_variable TopicCV_;
		std::map<std::string, std::list<std::pair<AckCallback, uint64_t>>> Notifiers_;
		Poco::Thread 			Worker_;
		mutable std::atomic_bool Running_ = false;
		mutable std::atomic_bool Assigned_ = false;
		mutable std::atomic_bool Failed_ = false;
		uint64_t 				FunctionId_ = 1;
		std::unique_ptr<cppkafka::Consumer> 			Consumer_;
		std::set<std::string>	Topics_;
		std::thread::id			DispatchThread_;
		std::string				FailureReason_;

		void run() override;
		void MarkFailed(const std::string &Reason);
		friend class KafkaManager;
		std::uint64_t RegisterTopicWatcher(const std::string &Topic, AckCallback F);
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
						 const std::string &PayLoad, bool WrapMessage = true, int32_t Partition = KAFKA_PARTITION_DEFAULT);
		void PostMessage(const char *topic, const std::string &key,
						 const Poco::JSON::Object &Object, bool WrapMessage = true, int32_t Partition = KAFKA_PARTITION_DEFAULT);

		[[nodiscard]] std::string WrapSystemId(const std::string & PayLoad);
		[[nodiscard]] inline bool Enabled() const { return KafkaEnabled_; }

		inline std::uint64_t RegisterTopicWatcher(const std::string &Topic,
		                                          Types::TopicNotifyFunction &F,
		                                          KafkaConsumerType ConsumerType) {
			if (ConsumerType == KafkaConsumerType::Group) {
				poco_error(Logger(), Poco::format("Rejecting legacy void callback for GroupConsumer topic '%s'; explicit outcome required.", Topic));
				throw std::invalid_argument("GroupConsumer requires an explicit KafkaHandlerOutcome callback");
			}
			return BroadcastConsumerThr_.RegisterTopicWatcher(Topic,
				[F](const std::string &Key, const std::string &Payload, std::string &) {
					F(Key, Payload);
					return Types::KafkaHandlerOutcome::Success;
				});
		}

		inline std::uint64_t RegisterTopicWatcher(const std::string &Topic,
		                                          const std::function<Types::KafkaHandlerOutcome(const std::string &, const std::string &)> &F,
		                                          KafkaConsumerType ConsumerType) {
			return RegisterTopicWatcher(Topic,
				KafkaConsumer::AckCallback([F](const std::string &Key, const std::string &Payload, std::string &) {
					return F(Key, Payload);
				}),
				ConsumerType);
		}

		inline std::uint64_t RegisterTopicWatcher(const std::string &Topic, const KafkaConsumer::AckCallback &F, KafkaConsumerType ConsumerType) {
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
		[[nodiscard]] const KafkaConsumer &GroupConsumer() const { return GroupConsumerThr_; }
		[[nodiscard]] bool HasGroupConsumerFailed() const { return GroupConsumerThr_.HasFailed(); }
		[[nodiscard]] std::string GroupConsumerFailureReason() const { return GroupConsumerThr_.FailureReason(); }
		[[nodiscard]] std::string GroupConsumerStatus() const { return GroupConsumerThr_.StatusString(); }
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
