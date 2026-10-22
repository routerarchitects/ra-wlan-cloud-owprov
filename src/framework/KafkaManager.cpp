//
// Created by stephane bourque on 2022-10-25.
//

#include "KafkaManager.h"

#include "fmt/format.h"
#include "framework/MicroServiceFuncs.h"
#include "cppkafka/utils/consumer_dispatcher.h"

namespace OpenWifi {

	void KafkaLoggerFun([[maybe_unused]] cppkafka::KafkaHandleBase &handle, int level,
						const std::string &facility, const std::string &message) {
		switch ((cppkafka::LogLevel)level) {
		case cppkafka::LogLevel::LogNotice: {
			poco_notice(KafkaManager()->Logger(),
						fmt::format("kafka-log: facility: {} message: {}", facility, message));
		} break;
		case cppkafka::LogLevel::LogDebug: {
			poco_debug(KafkaManager()->Logger(),
					   fmt::format("kafka-log: facility: {} message: {}", facility, message));
		} break;
		case cppkafka::LogLevel::LogInfo: {
			poco_information(KafkaManager()->Logger(),
							 fmt::format("kafka-log: facility: {} message: {}", facility, message));
		} break;
		case cppkafka::LogLevel::LogWarning: {
			poco_warning(KafkaManager()->Logger(),
						 fmt::format("kafka-log: facility: {} message: {}", facility, message));
		} break;
		case cppkafka::LogLevel::LogAlert:
		case cppkafka::LogLevel::LogCrit: {
			poco_critical(KafkaManager()->Logger(),
						  fmt::format("kafka-log: facility: {} message: {}", facility, message));
		} break;
		case cppkafka::LogLevel::LogErr:
		case cppkafka::LogLevel::LogEmerg:
		default: {
			poco_error(KafkaManager()->Logger(),
					   fmt::format("kafka-log: facility: {} message: {}", facility, message));
		} break;
		}
	}

	inline void KafkaErrorFun([[maybe_unused]] cppkafka::KafkaHandleBase &handle, int error,
							  const std::string &reason) {
		poco_error(KafkaManager()->Logger(),
				   fmt::format("kafka-error: {}, reason: {}", error, reason));
	}

	inline void AddKafkaSecurity(cppkafka::Configuration &Config) {
		auto CA = MicroServiceConfigGetString("openwifi.kafka.ssl.ca.location", "");
		auto Certificate =
			MicroServiceConfigGetString("openwifi.kafka.ssl.certificate.location", "");
		auto Key = MicroServiceConfigGetString("openwifi.kafka.ssl.key.location", "");
		auto Password = MicroServiceConfigGetString("openwifi.kafka.ssl.key.password", "");

		if (CA.empty() || Certificate.empty() || Key.empty())
			return;

		Config.set("ssl.ca.location", CA);
		Config.set("ssl.certificate.location", Certificate);
		Config.set("ssl.key.location", Key);
		if (!Password.empty())
			Config.set("ssl.key.password", Password);
	}

	void KafkaManager::initialize(Poco::Util::Application &self) {
		SubSystemServer::initialize(self);
		KafkaEnabled_ = MicroServiceConfigGetBool("openwifi.kafka.enable", false);
	}

	KafkaConsumer::KafkaConsumer(KafkaConsumerType Type) : ConsumerType_(Type) {}

	std::set<std::string> KafkaConsumer::Topics() const {
		std::lock_guard G(ConsumerMutex_);
		return Topics_;
	}

	std::vector<std::string> KafkaConsumer::Subscriptions() const {
		std::lock_guard G(ConsumerMutex_);
		if (Consumer_) {
			return Consumer_->get_subscription();
		}
		return {};
	}

	std::string KafkaConsumer::GenerateGroupId(KafkaConsumerType Type,
	                                           const std::string &BaseGroupId,
	                                           const std::string &RuntimeInstanceId) {
		std::string Base = BaseGroupId.empty() ? "prov" : BaseGroupId;
		return (Type == KafkaConsumerType::Broadcast) ? fmt::format("{}-{}-broadcast", Base, RuntimeInstanceId) : Base;
	}

	std::string KafkaConsumer::GenerateClientId(KafkaConsumerType Type,
	                                            const std::string &BaseClientId,
	                                            const std::string &RuntimeInstanceId) {
		std::string Sfx = (Type == KafkaConsumerType::Broadcast) ? "broadcast-consumer" : "work-consumer";
		return BaseClientId.empty() ? fmt::format("{}-{}", RuntimeInstanceId, Sfx)
		                            : fmt::format("{}-{}-{}", BaseClientId, RuntimeInstanceId, Sfx);
	}

	std::string KafkaConsumer::GenerateAutoOffsetReset(KafkaConsumerType Type) {
		return (Type == KafkaConsumerType::Broadcast) ? "latest" : "smallest";
	}

	std::string KafkaConsumer::GetGroupId() const {
		return GenerateGroupId(ConsumerType_, MicroServiceConfigGetString("openwifi.kafka.group.id", "prov"), MicroServiceRuntimeInstanceId());
	}

	std::string KafkaConsumer::GetClientId() const {
		return GenerateClientId(ConsumerType_, MicroServiceConfigGetString("openwifi.kafka.client.id", "prov"), MicroServiceRuntimeInstanceId());
	}

	std::string KafkaConsumer::GetAutoOffsetReset() const {
		return GenerateAutoOffsetReset(ConsumerType_);
	}

	std::string KafkaProducer::GenerateClientId(const std::string &BaseClientId,
	                                            const std::string &RuntimeInstanceId) {
		return BaseClientId.empty() ? fmt::format("{}-producer", RuntimeInstanceId)
		                            : fmt::format("{}-{}-producer", BaseClientId, RuntimeInstanceId);
	}

	std::string KafkaProducer::GetClientId() const {
		return GenerateClientId(MicroServiceConfigGetString("openwifi.kafka.client.id", "prov"), MicroServiceRuntimeInstanceId());
	}

	void KafkaProducer::Start() {
		std::lock_guard G(Mutex_);
		if (!Running_ && !Worker_.isRunning()) {
			Running_ = true;
			Worker_.start(*this);
		}
	}

	void KafkaProducer::Stop() {
		{
			std::lock_guard G(Mutex_);
			if (!Running_ && !Worker_.isRunning()) return;
			Running_ = false;
			Queue_.wakeUpAll();
		}
		if (Worker_.isRunning()) Worker_.join();
	}

	void KafkaProducer::Produce(const char *Topic, const std::string &Key, const std::string &Payload) {
		std::lock_guard G(Mutex_);
		Queue_.enqueueNotification(new KafkaMessage(Topic, Key, Payload));
	}

	inline void KafkaProducer::run() {
		Poco::Logger &Logger_ = Poco::Logger::has("KAFKA-PRODUCER") ? Poco::Logger::get("KAFKA-PRODUCER") : Poco::Logger::create("KAFKA-PRODUCER", KafkaManager()->Logger().getChannel());
		poco_information(Logger_, fmt::format("Starting producer with client.id '{}'...", GetClientId()));
		Utils::SetThreadName("Kafka:Prod");

		struct Cleanup {
			std::atomic_bool &Running;
			std::mutex &Mtx;
			~Cleanup() { std::lock_guard G(Mtx); Running = false; }
		} Guard{Running_, Mutex_};

		cppkafka::Configuration Config({
			{"client.id", GetClientId()},
			{"metadata.broker.list", MicroServiceConfigGetString("openwifi.kafka.brokerlist", "")}
		});
		AddKafkaSecurity(Config);
		Config.set_log_callback(KafkaLoggerFun);
		Config.set_error_callback(KafkaErrorFun);

		KafkaManager()->SystemInfoWrapper_ = fmt::format(
			R"lit({{ "system" : {{ "id" : {}, "host" : "{}" }}, "payload" : )lit",
			MicroServiceID(), MicroServicePrivateEndPoint());

		try {
			cppkafka::Producer Producer(Config);
			while (Running_) {
				Poco::AutoPtr<Poco::Notification> Note(Queue_.waitDequeueNotification(100));
				if (!Note) {
					continue;
				}
				try {
					if (auto Msg = dynamic_cast<KafkaMessage *>(Note.get())) {
						auto NewMsg = cppkafka::MessageBuilder(Msg->Topic());
						NewMsg.key(Msg->Key());
						NewMsg.partition(0);
						NewMsg.payload(Msg->Payload());
						Producer.produce(NewMsg);
						if (Queue_.size() < 100) {
							Producer.flush();
						} else {
							Producer.poll(std::chrono::milliseconds(0));
						}
					}
					if (Queue_.size() == 0) {
						Producer.flush();
					}
				} catch (const cppkafka::HandleException &E) {
					poco_warning(Logger_, fmt::format("Kafka exception (producer): {}", E.what()));
				} catch (const Poco::Exception &E) {
					Logger_.log(E);
				} catch (...) {
					poco_error(Logger_, "Exception in producer loop");
				}
			}
			Producer.flush();
		} catch (const std::exception &E) {
			poco_warning(Logger_, fmt::format("Kafka producer error: {}", E.what()));
		} catch (...) {
			poco_error(Logger_, "Unknown exception in Kafka producer");
		}
		poco_information(Logger_, "Stopped...");
	}

	void KafkaConsumer::Start() {
		std::lock_guard Lock(ConsumerMutex_);
		if (!Running_ && !Worker_.isRunning()) {
			Running_ = true;
			Worker_.start(*this);
		}
	}

	void KafkaConsumer::Stop() {
		{
			std::lock_guard Lock(ConsumerMutex_);
			if (!Running_ && !Worker_.isRunning()) return;
			Running_ = false;
			TopicCV_.notify_all();
		}
		if (Worker_.isRunning()) Worker_.join();
	}

	inline void KafkaConsumer::run() {
		std::string RoleName = (ConsumerType_ == KafkaConsumerType::Broadcast) ? "BroadcastConsumer" : "GroupConsumer";
		Utils::SetThreadName((ConsumerType_ == KafkaConsumerType::Broadcast) ? "Kafka:Bcast" : "Kafka:GrpCons");
		std::string LoggerName = (ConsumerType_ == KafkaConsumerType::Broadcast) ? "KAFKA-BROADCAST-CONSUMER" : "KAFKA-GROUP-CONSUMER";
		Poco::Logger &Logger_ = Poco::Logger::has(LoggerName) ? Poco::Logger::get(LoggerName) : Poco::Logger::create(LoggerName, KafkaManager()->Logger().getChannel());

		poco_information(Logger_, fmt::format("Starting {} (group.id='{}', client.id='{}', auto.offset.reset='{}')...",
			RoleName, GetGroupId(), GetClientId(), GetAutoOffsetReset()));

		struct Cleanup {
			KafkaConsumer &Self;
			~Cleanup() {
				std::lock_guard Lock(Self.ConsumerMutex_);
				Self.Consumer_.reset();
				Self.Running_ = false;
				Self.TopicCV_.notify_all();
			}
		} Guard{*this};

		{
			std::unique_lock Lock(ConsumerMutex_);
			while (Running_ && Topics_.empty()) {
				TopicCV_.wait_for(Lock, std::chrono::milliseconds(500), [this]() {
					return !Running_ || !Topics_.empty();
				});
			}
			if (!Running_ || Topics_.empty()) {
				poco_information(Logger_, fmt::format("{} stopped cleanly (group.id='{}').", RoleName, GetGroupId()));
				return;
			}
		}

		bool CleanShutdown = false;
		try {
			cppkafka::Configuration Config({
				{"client.id", GetClientId()},
				{"metadata.broker.list", MicroServiceConfigGetString("openwifi.kafka.brokerlist", "")},
				{"group.id", GetGroupId()},
				{"enable.auto.commit", MicroServiceConfigGetBool("openwifi.kafka.auto.commit", false)},
				{"auto.offset.reset", GetAutoOffsetReset()},
				{"enable.partition.eof", false}
			});
			AddKafkaSecurity(Config);
			Config.set_log_callback(KafkaLoggerFun);
			Config.set_error_callback(KafkaErrorFun);
			Config.set_default_topic_configuration({{"auto.offset.reset", GetAutoOffsetReset()}});

			auto LocalConsumer = std::make_unique<cppkafka::Consumer>(Config);
			LocalConsumer->set_assignment_callback([&](cppkafka::TopicPartitionList &parts) {
				if (!parts.empty()) {
					poco_information(Logger_, fmt::format("Partition assigned: {}...", parts.front().get_partition()));
				}
			});
			LocalConsumer->set_revocation_callback([&](const cppkafka::TopicPartitionList &parts) {
				if (!parts.empty()) {
					poco_information(Logger_, fmt::format("Partition revocation: {}...", parts.front().get_partition()));
				}
			});

			{
				std::lock_guard Lock(ConsumerMutex_);
				if (!Running_) return;
				Consumer_ = std::move(LocalConsumer);
				Consumer_->subscribe(Types::StringVec(Topics_.begin(), Topics_.end()));
			}

			while (Running_) {
				cppkafka::Message msg = Consumer_->poll(std::chrono::milliseconds(100));
				if (!msg) {
					continue;
				}
				if (msg.get_error()) {
					if (msg.is_eof()) {
						poco_debug(Logger_, fmt::format("Partition {} EOF", msg.get_partition()));
					} else {
						poco_warning(Logger_, fmt::format("Error: {}", msg.get_error().to_string()));
					}
					continue;
				}
				{
					std::lock_guard G(ConsumerMutex_);
					auto It = Notifiers_.find(msg.get_topic());
					if (It != Notifiers_.end()) {
						for (const auto &[CallbackFunc, _] : It->second) {
							try { CallbackFunc(msg.get_key(), msg.get_payload()); }
							catch (const std::exception &E) { poco_warning(Logger_, fmt::format("Callback exception: {}", E.what())); }
							catch (...) { poco_error(Logger_, "Unknown callback exception"); }
						}
					}
				}
				if (Consumer_) Consumer_->commit(msg);
			}

			std::lock_guard Lock(ConsumerMutex_);
			if (Consumer_) { try { Consumer_->unsubscribe(); } catch (...) {} }
			CleanShutdown = true;
		} catch (const std::exception &E) {
			if (!Running_) {
				CleanShutdown = true;
			} else {
				poco_error(Logger_, fmt::format("Unexpected terminal failure in {} (group.id='{}'): {}", RoleName, GetGroupId(), E.what()));
			}
		} catch (...) {
			if (!Running_) {
				CleanShutdown = true;
			} else {
				poco_error(Logger_, fmt::format("Unexpected terminal failure in {} (group.id='{}'): unknown exception", RoleName, GetGroupId()));
			}
		}

		if (CleanShutdown) {
			poco_information(Logger_, fmt::format("{} stopped cleanly (group.id='{}').", RoleName, GetGroupId()));
		}
	}

	std::uint64_t KafkaConsumer::RegisterTopicWatcher(const std::string &Topic, Types::TopicNotifyFunction &F) {
		std::lock_guard G(ConsumerMutex_);
		Notifiers_[Topic].emplace_back(F, FunctionId_);
		auto [_, inserted] = Topics_.insert(Topic);
		TopicCV_.notify_all();
		if (inserted && Consumer_ && Running_) {
			try {
				Consumer_->subscribe(Types::StringVec(Topics_.begin(), Topics_.end()));
			} catch (const std::exception &E) {
				Topics_.erase(Topic);
				Notifiers_[Topic].pop_back();
				if (Notifiers_[Topic].empty()) {
					Notifiers_.erase(Topic);
				}
				std::string Role = (ConsumerType_ == KafkaConsumerType::Broadcast) ? "BroadcastConsumer" : "GroupConsumer";
				poco_error(KafkaManager()->Logger(),
					fmt::format("Failed to subscribe to topic '{}' on {}: {}", Topic, Role, E.what()));
				throw;
			} catch (...) {
				Topics_.erase(Topic);
				Notifiers_[Topic].pop_back();
				if (Notifiers_[Topic].empty()) {
					Notifiers_.erase(Topic);
				}
				std::string Role = (ConsumerType_ == KafkaConsumerType::Broadcast) ? "BroadcastConsumer" : "GroupConsumer";
				poco_error(KafkaManager()->Logger(),
					fmt::format("Failed to subscribe to topic '{}' on {}: unknown exception", Topic, Role));
				throw;
			}
		}
		return FunctionId_++;
	}

	void KafkaConsumer::UnregisterTopicWatcher(const std::string &Topic, uint64_t Id) {
		std::lock_guard G(ConsumerMutex_);
		auto It = Notifiers_.find(Topic);
		if (It != Notifiers_.end()) {
			auto &L = It->second;
			for (auto it = L.begin(); it != L.end(); ++it) {
				if (it->second == static_cast<int>(Id)) {
					auto RemovedWatcher = *it;
					L.erase(it);
					if (L.empty()) {
						Notifiers_.erase(It);
						Topics_.erase(Topic);
						if (Consumer_ && Running_) {
							try {
								if (Topics_.empty()) Consumer_->unsubscribe();
								else Consumer_->subscribe(Types::StringVec(Topics_.begin(), Topics_.end()));
							} catch (const std::exception &E) {
								Topics_.insert(Topic);
								Notifiers_[Topic].push_back(RemovedWatcher);
								std::string Role = (ConsumerType_ == KafkaConsumerType::Broadcast) ? "BroadcastConsumer" : "GroupConsumer";
								poco_error(KafkaManager()->Logger(),
									fmt::format("Failed to update subscription after unregistering topic '{}' on {}: {}", Topic, Role, E.what()));
								throw;
							} catch (...) {
								Topics_.insert(Topic);
								Notifiers_[Topic].push_back(RemovedWatcher);
								std::string Role = (ConsumerType_ == KafkaConsumerType::Broadcast) ? "BroadcastConsumer" : "GroupConsumer";
								poco_error(KafkaManager()->Logger(),
									fmt::format("Failed to update subscription after unregistering topic '{}' on {}: unknown exception", Topic, Role));
								throw;
							}
						}
					}
					break;
				}
			}
		}
	}

	int KafkaManager::Start() {
		if (!KafkaEnabled_)
			return 0;
		MaxPayloadSize_ = MicroServiceConfigGetInt("openwifi.kafka.max.payload", 250000);
		GroupConsumerThr_.Start();
		BroadcastConsumerThr_.Start();
		ProducerThr_.Start();
		return 0;
	}

	void KafkaManager::Stop() {
		if (KafkaEnabled_) {
			poco_information(Logger(), "Stopping...");
			ProducerThr_.Stop();
			BroadcastConsumerThr_.Stop();
			GroupConsumerThr_.Stop();
			poco_information(Logger(), "Stopped...");
			return;
		}
	}

	void KafkaManager::PostMessage(const char *topic, const std::string &key,
								   const std::string & PayLoad, bool WrapMessage) {
		if (KafkaEnabled_) {
			ProducerThr_.Produce(topic, key, WrapMessage ? WrapSystemId(PayLoad) : PayLoad);
		}
	}

	void KafkaManager::PostMessage(const char *topic, const std::string &key,
					 const Poco::JSON::Object &Object, bool WrapMessage) {
		if (KafkaEnabled_) {
			std::ostringstream ObjectStr;
			Object.stringify(ObjectStr);
			ProducerThr_.Produce(topic, key, WrapMessage ? WrapSystemId(ObjectStr.str()) : ObjectStr.str());
		}
	}

	[[nodiscard]] std::string KafkaManager::WrapSystemId(const std::string & PayLoad) {
		return fmt::format(	R"lit({{ "system" : {{ "id" : {}, "host" : "{}" }}, "payload" : {} }})lit",
						   MicroServiceID(), MicroServicePrivateEndPoint(), PayLoad ) ;
	}

	void KafkaManager::PartitionAssignment(const cppkafka::TopicPartitionList &partitions) {
		poco_information(
			Logger(), fmt::format("Partition assigned: {}...", partitions.front().get_partition()));
	}

	void KafkaManager::PartitionRevocation(const cppkafka::TopicPartitionList &partitions) {
		poco_information(Logger(), fmt::format("Partition revocation: {}...",
											   partitions.front().get_partition()));
	}

} // namespace OpenWifi
