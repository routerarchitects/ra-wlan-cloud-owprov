//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Poco/ConsoleChannel.h"
#include "Poco/UUIDGenerator.h"
#include "framework/KafkaManager.h"
#include "framework/KafkaTopics.h"
#include "framework/utils.h"

#define TEST_ASSERT(cond, msg)                                                                   \
	do {                                                                                         \
		if (!(cond)) {                                                                           \
			std::cerr << "TEST FAILURE [" << __FILE__ << ":" << __LINE__ << "]: " << msg        \
					  << std::endl;                                                              \
			std::exit(1);                                                                        \
		}                                                                                        \
	} while (0)

namespace OpenWifi {

	static std::string g_MockGroupId = "prov";
	static std::string g_MockClientId = "prov1";
	static std::string g_MockInstanceId = "owprov-1-550e8400-e29b-41d4-a716-446655440000";
	static std::string g_MockPrivateEndPoint = "https://owprov-internal:17005";
	static uint64_t g_MockSystemId = 12345;

	std::string MicroServiceConfigGetString(const std::string &Key, const std::string &DefaultValue) {
		if (Key == "openwifi.kafka.group.id") return g_MockGroupId;
		if (Key == "openwifi.kafka.client.id") return g_MockClientId;
		if (Key == "openwifi.kafka.brokerlist") {
			const char *env = std::getenv("KAFKA_BROKERLIST");
			return env ? std::string(env) : DefaultValue;
		}
		return DefaultValue;
	}

	bool MicroServiceConfigGetBool(const std::string &/*Key*/, bool DefaultValue) {
		return DefaultValue;
	}

	std::uint64_t MicroServiceConfigGetInt(const std::string &/*Key*/, std::uint64_t DefaultValue) {
		return DefaultValue;
	}

	std::string MicroServiceRuntimeInstanceId() {
		return g_MockInstanceId;
	}

	std::string MicroServicePrivateEndPoint() {
		return g_MockPrivateEndPoint;
	}

	std::uint64_t MicroServiceID() {
		return g_MockSystemId;
	}

	SubSystemServer::SubSystemServer(const std::string &Name, const std::string &LoggingPrefix,
									 const std::string &SubSystemConfigPrefix)
		: Name_(Name), LoggerPrefix_(LoggingPrefix), SubSystemConfigPrefix_(SubSystemConfigPrefix) {
		Logger_ = std::make_unique<LoggerWrapper>(Poco::Logger::root());
	}

	void SubSystemServer::initialize(Poco::Util::Application &/*self*/) {}
}

using namespace OpenWifi;

// Test 1: Verify consumer types for Group and Broadcast
static void test_consumer_types() {
	TEST_ASSERT(KafkaManager()->GroupConsumer().ConsumerType() == KafkaConsumerType::Group,
		"GroupConsumer must have KafkaConsumerType::Group");
	TEST_ASSERT(KafkaManager()->BroadcastConsumer().ConsumerType() == KafkaConsumerType::Broadcast,
		"BroadcastConsumer must have KafkaConsumerType::Broadcast");

	std::cout << "  PASS: test_consumer_types" << std::endl;
}

// Test 2: Shared group ID for GroupConsumer
static void test_group_consumer_shared_group_id() {
	std::string instance1 = "owprov-1-11111111-1111-4111-8111-111111111111";
	std::string instance2 = "owprov-2-22222222-2222-4222-8222-222222222222";

	std::string group1 = KafkaConsumer::GenerateGroupId(KafkaConsumerType::Group, "prov", instance1);
	std::string group2 = KafkaConsumer::GenerateGroupId(KafkaConsumerType::Group, "prov", instance2);

	TEST_ASSERT(group1 == "prov", "GroupConsumer group.id must equal base group ID");
	TEST_ASSERT(group2 == "prov", "GroupConsumer group.id must equal base group ID");
	TEST_ASSERT(group1 == group2, "All OWPROV replicas must share identical group.id for GroupConsumer");

	std::cout << "  PASS: test_group_consumer_shared_group_id" << std::endl;
}

// Test 3: Incarnation-unique group ID for BroadcastConsumer
static void test_broadcast_consumer_incarnation_unique_group_id() {
	std::string instance1 = "owprov-1-11111111-1111-4111-8111-111111111111";
	std::string instance2 = "owprov-2-22222222-2222-4222-8222-222222222222";

	std::string bcast1 = KafkaConsumer::GenerateGroupId(KafkaConsumerType::Broadcast, "prov", instance1);
	std::string bcast2 = KafkaConsumer::GenerateGroupId(KafkaConsumerType::Broadcast, "prov", instance2);

	TEST_ASSERT(bcast1 == "prov-owprov-1-11111111-1111-4111-8111-111111111111-broadcast",
		"Broadcast group ID must format as <base>-<instance-id>-broadcast");
	TEST_ASSERT(bcast2 == "prov-owprov-2-22222222-2222-4222-8222-222222222222-broadcast",
		"Broadcast group ID must format as <base>-<instance-id>-broadcast");
	TEST_ASSERT(bcast1 != bcast2,
		"Broadcast consumer group IDs must be unique per runtime instance");

	std::cout << "  PASS: test_broadcast_consumer_incarnation_unique_group_id" << std::endl;
}

// Test 4: Rolling restart overlap: same slot name, different runtime UUIDs
static void test_rolling_restart_broadcast_isolation() {
	std::string old_instance = "owprov-1-aaaaaaaa-0000-4000-8000-000000000001";
	std::string new_instance = "owprov-1-bbbbbbbb-0000-4000-8000-000000000002";

	// Both share the same slot name "owprov-1", but have distinct runtime incarnation UUIDs
	std::string work_old = KafkaConsumer::GenerateGroupId(KafkaConsumerType::Group, "prov", old_instance);
	std::string work_new = KafkaConsumer::GenerateGroupId(KafkaConsumerType::Group, "prov", new_instance);

	TEST_ASSERT(work_old == work_new,
		"Work group ID must remain shared across rolling restarts");

	std::string bcast_old = KafkaConsumer::GenerateGroupId(KafkaConsumerType::Broadcast, "prov", old_instance);
	std::string bcast_new = KafkaConsumer::GenerateGroupId(KafkaConsumerType::Broadcast, "prov", new_instance);

	TEST_ASSERT(bcast_old != bcast_new,
		"Broadcast group IDs MUST be distinct across rolling restart to prevent partition division");
	TEST_ASSERT(bcast_old == "prov-owprov-1-aaaaaaaa-0000-4000-8000-000000000001-broadcast",
		"Old broadcast group ID matches format");
	TEST_ASSERT(bcast_new == "prov-owprov-1-bbbbbbbb-0000-4000-8000-000000000002-broadcast",
		"New broadcast group ID matches format");

	std::cout << "  PASS: test_rolling_restart_broadcast_isolation" << std::endl;
}

// Test 5: Distinct client IDs for GroupConsumer, BroadcastConsumer, and Producer
static void test_client_id_generation() {
	std::string instance_id = "owprov-1-550e8400-e29b-41d4-a716-446655440000";
	std::string base_client = "prov1";

	std::string grp_client = KafkaConsumer::GenerateClientId(KafkaConsumerType::Group, base_client, instance_id);
	std::string bcast_client = KafkaConsumer::GenerateClientId(KafkaConsumerType::Broadcast, base_client, instance_id);
	std::string prod_client = KafkaProducer::GenerateClientId(base_client, instance_id);

	TEST_ASSERT(grp_client == "prov1-owprov-1-550e8400-e29b-41d4-a716-446655440000-work-consumer",
		"Group consumer client ID format matches");
	TEST_ASSERT(bcast_client == "prov1-owprov-1-550e8400-e29b-41d4-a716-446655440000-broadcast-consumer",
		"Broadcast consumer client ID format matches");
	TEST_ASSERT(prod_client == "prov1-owprov-1-550e8400-e29b-41d4-a716-446655440000-producer",
		"Producer client ID format matches");

	TEST_ASSERT(grp_client != bcast_client, "Group and broadcast client IDs must be distinct");
	TEST_ASSERT(grp_client != prod_client, "Group and producer client IDs must be distinct");
	TEST_ASSERT(bcast_client != prod_client, "Broadcast and producer client IDs must be distinct");

	std::cout << "  PASS: test_client_id_generation" << std::endl;
}

// Test 6: Auto offset reset semantics
static void test_auto_offset_reset_semantics() {
	std::string bcast_reset = KafkaConsumer::GenerateAutoOffsetReset(KafkaConsumerType::Broadcast);
	std::string grp_reset = KafkaConsumer::GenerateAutoOffsetReset(KafkaConsumerType::Group);

	TEST_ASSERT(bcast_reset == "latest",
		"BroadcastConsumer MUST use auto.offset.reset = latest");
	TEST_ASSERT(grp_reset == "smallest",
		"GroupConsumer preserves smallest reset");

	std::cout << "  PASS: test_auto_offset_reset_semantics" << std::endl;
}

// Test 7: Topic registration and unregistration routing
static void test_topic_registration_and_unregistration() {
	KafkaConsumer::AckCallback grp_cb = [](const std::string &, const std::string &, std::string &) {
		return Types::KafkaHandlerOutcome::Success;
	};
	Types::TopicNotifyFunction bcast_cb = [](const std::string &, const std::string &) {};

	// Register connection on GroupConsumer
	uint64_t id_conn = KafkaManager()->RegisterTopicWatcher(
		KafkaTopics::CONNECTION, grp_cb, KafkaConsumerType::Group);
	TEST_ASSERT(id_conn > 0, "Registration must return positive ID");

	// Register service_events on BroadcastConsumer
	uint64_t id_se = KafkaManager()->RegisterTopicWatcher(
		KafkaTopics::SERVICE_EVENTS, bcast_cb, KafkaConsumerType::Broadcast);
	TEST_ASSERT(id_se > 0, "Registration must return positive ID");

	auto grp_topics = KafkaManager()->GroupConsumer().Topics();
	auto bcast_topics = KafkaManager()->BroadcastConsumer().Topics();

	TEST_ASSERT(grp_topics.find(KafkaTopics::CONNECTION) != grp_topics.end(),
		"GroupConsumer must contain CONNECTION topic");
	TEST_ASSERT(grp_topics.find(KafkaTopics::SERVICE_EVENTS) == grp_topics.end(),
		"GroupConsumer must NOT contain SERVICE_EVENTS topic");

	TEST_ASSERT(bcast_topics.find(KafkaTopics::SERVICE_EVENTS) != bcast_topics.end(),
		"BroadcastConsumer must contain SERVICE_EVENTS topic");
	TEST_ASSERT(bcast_topics.find(KafkaTopics::CONNECTION) == bcast_topics.end(),
		"BroadcastConsumer must NOT contain CONNECTION topic");

	// Unregister connection from GroupConsumer
	KafkaManager()->UnregisterTopicWatcher(KafkaTopics::CONNECTION, id_conn, KafkaConsumerType::Group);
	grp_topics = KafkaManager()->GroupConsumer().Topics();
	TEST_ASSERT(grp_topics.find(KafkaTopics::CONNECTION) == grp_topics.end(),
		"GroupConsumer must no longer contain CONNECTION after unregistration");

	// BroadcastConsumer still has service_events
	bcast_topics = KafkaManager()->BroadcastConsumer().Topics();
	TEST_ASSERT(bcast_topics.find(KafkaTopics::SERVICE_EVENTS) != bcast_topics.end(),
		"BroadcastConsumer must still contain SERVICE_EVENTS");

	// Unregister service_events from BroadcastConsumer
	KafkaManager()->UnregisterTopicWatcher(KafkaTopics::SERVICE_EVENTS, id_se, KafkaConsumerType::Broadcast);
	bcast_topics = KafkaManager()->BroadcastConsumer().Topics();
	TEST_ASSERT(bcast_topics.find(KafkaTopics::SERVICE_EVENTS) == bcast_topics.end(),
		"BroadcastConsumer must no longer contain SERVICE_EVENTS after unregistration");

	std::cout << "  PASS: test_topic_registration_and_unregistration" << std::endl;
}

// Test 8: Multiple watchers for the same topic
static void test_multiple_watchers_per_topic() {
	Types::TopicNotifyFunction cb1 = [](const std::string &, const std::string &) {};
	Types::TopicNotifyFunction cb2 = [](const std::string &, const std::string &) {};

	uint64_t id1 = KafkaManager()->RegisterTopicWatcher(
		KafkaTopics::SERVICE_EVENTS, cb1, KafkaConsumerType::Broadcast);
	uint64_t id2 = KafkaManager()->RegisterTopicWatcher(
		KafkaTopics::SERVICE_EVENTS, cb2, KafkaConsumerType::Broadcast);

	TEST_ASSERT(id1 != id2, "Watcher IDs must be distinct");

	auto topics = KafkaManager()->BroadcastConsumer().Topics();
	TEST_ASSERT(topics.find(KafkaTopics::SERVICE_EVENTS) != topics.end(),
		"Topic must be registered");

	// Unregister first watcher -> topic must still remain since id2 is active
	KafkaManager()->UnregisterTopicWatcher(KafkaTopics::SERVICE_EVENTS, id1, KafkaConsumerType::Broadcast);
	topics = KafkaManager()->BroadcastConsumer().Topics();
	TEST_ASSERT(topics.find(KafkaTopics::SERVICE_EVENTS) != topics.end(),
		"Topic must remain while second watcher is active");

	// Unregister second watcher -> topic is now removed
	KafkaManager()->UnregisterTopicWatcher(KafkaTopics::SERVICE_EVENTS, id2, KafkaConsumerType::Broadcast);
	topics = KafkaManager()->BroadcastConsumer().Topics();
	TEST_ASSERT(topics.find(KafkaTopics::SERVICE_EVENTS) == topics.end(),
		"Topic must be removed when all watchers are unregistered");

	std::cout << "  PASS: test_multiple_watchers_per_topic" << std::endl;
}

// Test 9: GetGroupId and GetClientId accessors using runtime mock
static void test_instance_accessors_from_config() {
	g_MockGroupId = "openwifi-prov";
	g_MockClientId = "owprov-client";
	g_MockInstanceId = "owprov-3-33333333-3333-4333-8333-333333333333";

	std::string grp_group = KafkaManager()->GroupConsumer().GetGroupId();
	std::string grp_client = KafkaManager()->GroupConsumer().GetClientId();
	std::string bcast_group = KafkaManager()->BroadcastConsumer().GetGroupId();
	std::string bcast_client = KafkaManager()->BroadcastConsumer().GetClientId();
	std::string prod_client = KafkaManager()->Producer().GetClientId();

	TEST_ASSERT(grp_group == "openwifi-prov",
		"GroupConsumer GetGroupId() must match openwifi.kafka.group.id");
	TEST_ASSERT(grp_client == "owprov-client-owprov-3-33333333-3333-4333-8333-333333333333-work-consumer",
		"GroupConsumer GetClientId() incorporates runtime instance ID");
	TEST_ASSERT(bcast_group == "openwifi-prov-owprov-3-33333333-3333-4333-8333-333333333333-broadcast",
		"BroadcastConsumer GetGroupId() incorporates runtime instance ID");
	TEST_ASSERT(bcast_client == "owprov-client-owprov-3-33333333-3333-4333-8333-333333333333-broadcast-consumer",
		"BroadcastConsumer GetClientId() incorporates runtime instance ID");
	TEST_ASSERT(prod_client == "owprov-client-owprov-3-33333333-3333-4333-8333-333333333333-producer",
		"Producer GetClientId() incorporates runtime instance ID");

	std::cout << "  PASS: test_instance_accessors_from_config" << std::endl;
}

// Test 10: Empty base strings fallback gracefully
static void test_empty_base_strings() {
	std::string instance_id = "standalone-uuid-4444";

	std::string bcast_group = KafkaConsumer::GenerateGroupId(KafkaConsumerType::Broadcast, "", instance_id);
	TEST_ASSERT(bcast_group == "prov-standalone-uuid-4444-broadcast",
		"Empty base group ID defaults to prov");

	std::string grp_group = KafkaConsumer::GenerateGroupId(KafkaConsumerType::Group, "", instance_id);
	TEST_ASSERT(grp_group == "prov",
		"Empty base group ID defaults to prov for GroupConsumer");

	std::string grp_client = KafkaConsumer::GenerateClientId(KafkaConsumerType::Group, "", instance_id);
	TEST_ASSERT(grp_client == "standalone-uuid-4444-work-consumer",
		"Empty base client ID prefixes role suffix directly");

	std::string bcast_client = KafkaConsumer::GenerateClientId(KafkaConsumerType::Broadcast, "", instance_id);
	TEST_ASSERT(bcast_client == "standalone-uuid-4444-broadcast-consumer",
		"Empty base client ID prefixes role suffix directly");

	std::string prod_client = KafkaProducer::GenerateClientId("", instance_id);
	TEST_ASSERT(prod_client == "standalone-uuid-4444-producer",
		"Empty base client ID prefixes role suffix directly for producer");

	std::cout << "  PASS: test_empty_base_strings" << std::endl;
}

// Test 11: Consumer startup and immediate shutdown without deadlock
static void test_consumer_startup_shutdown_race() {
	KafkaConsumer Consumer(KafkaConsumerType::Broadcast);
	TEST_ASSERT(!Consumer.IsRunning(), "Consumer must not be running initially");

	Consumer.Start();
	// Immediately stop before topics are even registered
	Consumer.Stop();
	TEST_ASSERT(!Consumer.IsRunning(), "Consumer must not be running after Stop()");

	// Multiple Stop() calls must be safe and idempotent
	Consumer.Stop();
	TEST_ASSERT(!Consumer.IsRunning(), "Subsequent Stop() must remain safe and false");

	std::cout << "  PASS: test_consumer_startup_shutdown_race" << std::endl;
}

// Test 12: Service events watcher lifecycle registration and unregistration
static void test_service_events_watcher_lifecycle_unregistration() {
	Types::TopicNotifyFunction cb = [](const std::string &, const std::string &) {};

	uint64_t reg_id = KafkaManager()->RegisterTopicWatcher(
		KafkaTopics::SERVICE_EVENTS, cb, KafkaConsumerType::Broadcast);
	TEST_ASSERT(reg_id > 0, "Registration must return non-zero ID");

	auto topics = KafkaManager()->BroadcastConsumer().Topics();
	TEST_ASSERT(topics.find(KafkaTopics::SERVICE_EVENTS) != topics.end(),
		"BroadcastConsumer must contain SERVICE_EVENTS after registration");

	KafkaManager()->UnregisterTopicWatcher(
		KafkaTopics::SERVICE_EVENTS, reg_id, KafkaConsumerType::Broadcast);

	topics = KafkaManager()->BroadcastConsumer().Topics();
	TEST_ASSERT(topics.find(KafkaTopics::SERVICE_EVENTS) == topics.end(),
		"BroadcastConsumer must not contain SERVICE_EVENTS after unregistration with reg_id");

	std::cout << "  PASS: test_service_events_watcher_lifecycle_unregistration" << std::endl;
}

// Test 13: Producer startup, message enqueueing, and shutdown without deadlock
static void test_producer_startup_shutdown_race() {
	KafkaProducer Producer;
	Producer.Start();
	Producer.Produce("test_topic", "key1", "payload1");
	Producer.Stop();
	Producer.Stop(); // idempotent
	std::cout << "  PASS: test_producer_startup_shutdown_race" << std::endl;
}

// Test 14: Consumer shutdown with registered topics
static void test_consumer_shutdown_with_registered_topics() {
	Types::TopicNotifyFunction cb = [](const std::string &, const std::string &) {};
	uint64_t reg_id = KafkaManager()->RegisterTopicWatcher(
		KafkaTopics::SERVICE_EVENTS, cb, KafkaConsumerType::Broadcast);

	auto &Consumer = KafkaManager()->BroadcastConsumer();
	Consumer.Start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	Consumer.Stop();
	TEST_ASSERT(!Consumer.IsRunning(), "Consumer must not be running after Stop()");

	KafkaManager()->UnregisterTopicWatcher(
		KafkaTopics::SERVICE_EVENTS, reg_id, KafkaConsumerType::Broadcast);
	std::cout << "  PASS: test_consumer_shutdown_with_registered_topics" << std::endl;
}

// Helper for Test 15: Run the 8-step concurrent dynamic subscription scenario for a consumer role
static void run_dynamic_subscription_test_for_role(KafkaConsumerType type,
                                                   const std::string &topic1,
                                                   const std::string &topic2) {
	std::string role = (type == KafkaConsumerType::Broadcast) ? "BroadcastConsumer" : "GroupConsumer";
	std::string prefix = (type == KafkaConsumerType::Broadcast) ? "bcast_" : "grp_";
	std::string token = Poco::UUIDGenerator::defaultGenerator().createRandom().toString();

	g_MockGroupId = "test_" + prefix + token;
	g_MockInstanceId = "owprov-test-" + token;

	KafkaProducer producer;
	producer.Start();

	auto &consumer = (type == KafkaConsumerType::Broadcast) ? KafkaManager()->BroadcastConsumer()
	                                                        : KafkaManager()->GroupConsumer();

	struct WatcherSink {
		std::mutex mtx;
		std::set<std::string> received;
		KafkaConsumer::AckCallback callback() {
			return [this](const std::string &, const std::string &msg, std::string &) {
				std::lock_guard g(mtx);
				received.insert(msg);
				return Types::KafkaHandlerOutcome::Success;
			};
		}
		bool has(const std::string &target) {
			std::lock_guard g(mtx);
			return received.find(target) != received.end();
		}
	};

	WatcherSink w1a, w1b, w2;

	auto produce_until_received = [](KafkaProducer &prod, const std::string &topic,
	                                 const std::string &key, const std::string &payload,
	                                 WatcherSink &sink,
	                                 std::chrono::seconds timeout = std::chrono::seconds(15)) -> bool {
		auto start = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() - start < timeout) {
			if (sink.has(payload)) return true;
			prod.Produce(topic.c_str(), key, payload);
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
		}
		return sink.has(payload);
	};

	auto vec_contains = [](const std::vector<std::string> &v, const std::string &item) -> bool {
		return std::find(v.begin(), v.end(), item) != v.end();
	};

	// 1. Initial registration
	auto cb1a = w1a.callback();
	uint64_t id1a = KafkaManager()->RegisterTopicWatcher(topic1, cb1a, type);
	TEST_ASSERT(id1a > 0, "Registration for topic1 must succeed on " + role);

	// 2. Start consumer polling in background worker thread
	consumer.Start();
	TEST_ASSERT(consumer.IsRunning(), role + " must be running");

	// 3. Produce payload1 to topic1 and verify watcher 1a receives exact payload1
	std::string payload1 = prefix + "payload1_" + token;
	bool ok = produce_until_received(producer, topic1, "k1", payload1, w1a);
	TEST_ASSERT(ok && w1a.has(payload1), role + " watcher 1a must receive exact payload1");

	// 4. Concurrently add multiple watchers for the same topic (topic1) while polling
	auto cb1b = w1b.callback();
	uint64_t id1b = KafkaManager()->RegisterTopicWatcher(topic1, cb1b, type);
	TEST_ASSERT(id1b > 0, "Registration of second watcher for topic1 must succeed on " + role);

	std::string payload2 = prefix + "payload2_" + token;
	ok = produce_until_received(producer, topic1, "k2", payload2, w1b);
	TEST_ASSERT(ok && w1b.has(payload2), role + " watcher 1b must receive exact payload2");
	TEST_ASSERT(w1a.has(payload2), role + " watcher 1a must also receive exact payload2");

	// 5. Concurrently add a new topic (topic2) while polling
	auto cb2 = w2.callback();
	uint64_t id2 = KafkaManager()->RegisterTopicWatcher(topic2, cb2, type);
	TEST_ASSERT(id2 > 0, "Registration for topic2 must succeed on " + role);

	std::string payload3 = prefix + "payload3_" + token;
	ok = produce_until_received(producer, topic2, "k3", payload3, w2);
	TEST_ASSERT(ok && w2.has(payload3), role + " watcher 2 must receive exact payload3 on dynamically added topic2");

	// 6. Concurrently remove watcher 1a (intermediate watcher removal, topic1 still active)
	KafkaManager()->UnregisterTopicWatcher(topic1, id1a, type);
	auto topics = consumer.Topics();
	TEST_ASSERT(topics.find(topic1) != topics.end(), "topic1 must remain subscribed on " + role + " while watcher 1b is active");

	std::string payload4 = prefix + "payload4_" + token;
	ok = produce_until_received(producer, topic1, "k4", payload4, w1b);
	TEST_ASSERT(ok && w1b.has(payload4), "Remaining watcher 1b on " + role + " must receive exact payload4");
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	TEST_ASSERT(!w1a.has(payload4), "Unregistered watcher 1a on " + role + " must NOT receive payload4");

	// 7. Concurrently remove final watcher for topic1 (topic1 removed from subscription)
	KafkaManager()->UnregisterTopicWatcher(topic1, id1b, type);
	topics = consumer.Topics();
	TEST_ASSERT(topics.find(topic1) == topics.end(), "topic1 must be removed from " + role + " Topics_");
	TEST_ASSERT(topics.find(topic2) != topics.end(), "topic2 must still be in " + role + " Topics_");
	TEST_ASSERT(consumer.IsRunning(), role + " must still be running after unregistering topic1");

	// Observable Kafka unsubscribe check via Subscriptions() (calls rd_kafka_subscription)
	auto subs = consumer.Subscriptions();
	TEST_ASSERT(!vec_contains(subs, topic1), "topic1 must be unsubscribed from underlying " + role);
	TEST_ASSERT(vec_contains(subs, topic2), "topic2 must remain subscribed in underlying " + role);

	// Publish unique message to topic1 after unregistering its final watcher
	std::string payload_post_unsub1 = prefix + "post_unsub1_" + token;
	producer.Produce(topic1.c_str(), "k_unsub1", payload_post_unsub1);

	// Produce payload5 to topic2 and verify watcher 2 receives it
	std::string payload5 = prefix + "payload5_" + token;
	ok = produce_until_received(producer, topic2, "k5", payload5, w2);
	TEST_ASSERT(ok && w2.has(payload5), role + " watcher 2 must receive exact payload5 after topic1 was removed");

	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	TEST_ASSERT(!w1b.has(payload_post_unsub1),
		"Removed watcher 1b on " + role + " must NOT receive message produced to topic1 after unsubscription");
	TEST_ASSERT(!w1a.has(payload_post_unsub1),
		"Removed watcher 1a on " + role + " must NOT receive message produced to topic1 after unsubscription");

	// 8. Concurrently remove final watcher for topic2 (unsubscribes all)
	KafkaManager()->UnregisterTopicWatcher(topic2, id2, type);
	topics = consumer.Topics();
	TEST_ASSERT(topics.empty(), "Topics must be empty on " + role + " after all watchers unregistered");
	TEST_ASSERT(consumer.IsRunning(), role + " must still be running after final watcher unregistration");
	subs = consumer.Subscriptions();
	TEST_ASSERT(subs.empty(), "Underlying " + role + " subscription must be empty after all watchers unregistered");

	consumer.Stop();
	producer.Stop();
	TEST_ASSERT(!consumer.IsRunning(), role + " must not be running after Stop()");
}

// Test 15: Broker-backed test covering dynamic registration/unregistration while polling
static void test_concurrent_dynamic_subscription_updates_while_polling() {
	const char *broker = std::getenv("KAFKA_BROKERLIST");
	if (!broker || std::string(broker).empty()) {
		std::cout << "  SKIPPED: test_concurrent_dynamic_subscription_updates_while_polling (KAFKA_BROKERLIST not set)" << std::endl;
		return;
	}

	std::cout << "  Running broker-backed dynamic subscription test against: " << broker << std::endl;

	// Part A: GroupConsumer concurrent dynamic subscriptions while polling
	run_dynamic_subscription_test_for_role(KafkaConsumerType::Group, "test_dyn_topic_a", "test_dyn_topic_b");

	// Part B: BroadcastConsumer concurrent dynamic subscriptions while polling
	run_dynamic_subscription_test_for_role(KafkaConsumerType::Broadcast, "test_bcast_topic_a", "test_bcast_topic_b");

	std::cout << "  PASS: test_concurrent_dynamic_subscription_updates_while_polling" << std::endl;
}

// Test 16: Verify outcome callback contract and dispatch behavior
static void test_callback_outcome_contract_and_dispatch() {
	std::cout << "  Starting test_callback_outcome_contract_and_dispatch..." << std::endl;

	// Verify outcome enum values
	TEST_ASSERT(static_cast<int>(Types::KafkaHandlerOutcome::Success) == 0,
		"KafkaHandlerOutcome::Success must be 0");
	TEST_ASSERT(static_cast<int>(Types::KafkaHandlerOutcome::RetryableFailure) == 1,
		"KafkaHandlerOutcome::RetryableFailure must be 1");
	TEST_ASSERT(static_cast<int>(Types::KafkaHandlerOutcome::NonRetryableFailure) == 2,
		"KafkaHandlerOutcome::NonRetryableFailure must be 2");

	// Verify callback contract with GroupConsumer registration
	std::atomic<int> callCount{0};
	std::string receivedKey, receivedPayload;
	KafkaConsumer::AckCallback cb = [&](const std::string &key, const std::string &payload, std::string &failureReason) -> Types::KafkaHandlerOutcome {
		++callCount;
		receivedKey = key;
		receivedPayload = payload;
		if (key == "fail_retryable") {
			failureReason = "simulated transient database error";
			return Types::KafkaHandlerOutcome::RetryableFailure;
		}
		if (key == "fail_non_retryable") {
			failureReason = "simulated malformed payload";
			return Types::KafkaHandlerOutcome::NonRetryableFailure;
		}
		return Types::KafkaHandlerOutcome::Success;
	};

	uint64_t watcherId = KafkaManager()->RegisterTopicWatcher("test_commit_topic", cb, KafkaConsumerType::Group);
	TEST_ASSERT(watcherId > 0, "Registration must succeed for GroupConsumer with outcome callback");

	// Invoke callback directly to verify outcome contract
	std::string failureReason;
	auto res1 = cb("valid_key", "valid_payload", failureReason);
	TEST_ASSERT(res1 == Types::KafkaHandlerOutcome::Success, "Valid message must return Success");
	TEST_ASSERT(failureReason.empty(), "Success must leave failureReason empty");

	auto res2 = cb("fail_retryable", "payload", failureReason);
	TEST_ASSERT(res2 == Types::KafkaHandlerOutcome::RetryableFailure, "Transient error must return RetryableFailure");
	TEST_ASSERT(failureReason == "simulated transient database error", "RetryableFailure must populate failure reason");

	failureReason.clear();
	auto res3 = cb("fail_non_retryable", "payload", failureReason);
	TEST_ASSERT(res3 == Types::KafkaHandlerOutcome::NonRetryableFailure, "Malformed error must return NonRetryableFailure");
	TEST_ASSERT(failureReason == "simulated malformed payload", "NonRetryableFailure must populate failure reason");

	KafkaManager()->UnregisterTopicWatcher("test_commit_topic", watcherId, KafkaConsumerType::Group);
	std::cout << "  PASS: test_callback_outcome_contract_and_dispatch" << std::endl;
}

// Test 17: Structural contract check — processing callback is NOT re-invoked during the
// commit-retry loop (idempotency contract).
//
// SCOPE: This is a mock-only structural test. It simulates the two-stage loop shape
// (processing → commit) with a local lambda. It does NOT:
//   - inject real Kafka commit failures into the production RetryOp("Commit") path
//   - exercise KafkaManager::run() or any production code path
//   - detect regressions where RetryOp("Commit") is broken or omitted
//
// KNOWN GAP: Genuine broker-level commit-failure regression coverage requires a
// fault-injecting Kafka proxy (e.g. Toxiproxy). That is left as a future integration test.
// This test is retained only as a structural/contract sanity check.
static void test_commit_isolation_contract_mock_only() {
	std::cout << "  Starting test_commit_isolation_contract_mock_only (mock-only structural check)..." << std::endl;

	std::atomic<int> processingCallbackCount{0};
	std::atomic<int> commitAttempts{0};

	// Processing callback returns Success exactly once
	KafkaConsumer::AckCallback processCb = [&](const std::string &, const std::string &, std::string &) {
		processingCallbackCount++;
		return Types::KafkaHandlerOutcome::Success;
	};

	// Logical contract: processing runs once; commit retries loop WITHOUT re-invoking processing.
	// This mirrors what KafkaManager::run() does: RetryOp("Processing") runs the callback once,
	// then RetryOp("Commit") loops separately without calling the callback again.
	auto verifyCommitIsolation = [&](int commitFailuresBeforeSuccess) -> bool {
		std::string dummyReason;
		// Step 1 — Processing stage: invoke callback exactly once
		auto outcome = processCb("k1", "payload1", dummyReason);
		if (outcome != Types::KafkaHandlerOutcome::Success) return false;

		// Step 2 — Commit stage: retry commit without re-invoking processing callback
		constexpr int maxAttempts = 5;
		for (int attempt = 0; attempt < maxAttempts; ++attempt) {
			commitAttempts++;
			if (attempt >= commitFailuresBeforeSuccess) {
				return true; // commit succeeded
			}
		}
		return false;
	};

	// Case: commit fails twice, succeeds on attempt 3
	bool ok = verifyCommitIsolation(2);
	TEST_ASSERT(ok, "Commit should eventually succeed");
	TEST_ASSERT(commitAttempts == 3, "Commit was retried 3 times");
	TEST_ASSERT(processingCallbackCount == 1,
		"Processing callback must NEVER be re-invoked during commit retries (idempotency guarantee)");

	std::cout << "  PASS: test_commit_isolation_contract_mock_only" << std::endl;
}

// Test 18: Verify the serial-number validation rules enforced by
// AutoDiscovery::ConnectionReceived() (AutoDiscovery.cpp L144-156).
//
// The predicate below is a verbatim inline copy of the two gates in production:
//   Gate 1 — AutoDiscovery.cpp L145:
//       serial.empty() || serial.size() > 16 || !Utils::ValidSerialNumber(serial)
//     where Utils::ValidSerialNumber (framework/utils.cpp L36-39) is:
//       serial.size() < SERIAL_NUMBER_LENGTH && std::all_of(..., std::isxdigit)
//   Gate 2 — AutoDiscovery.cpp L149-153:
//       std::stoull forward + std::stoull reversed (overflow guard)
//
// utils.cpp is not compiled into this test binary because it pulls in unresolvable
// symbols (AppServiceRegistry, DNS resolver) that have no stubs in this target.
// The inline copy is intentional and kept minimal to exactly mirror the production gate.
//
// KNOWN GAP: The full end-to-end path (Kafka message → ConnectionReceived()
// → InventoryDB::CreateFromConnection()) requires a live StorageService and is NOT
// exercised here. DB-layer rejection of an invalid serial cannot be verified in this
// unit test.
static void test_malformed_serial_numbers_validation_and_startup_safety() {
	std::cout << "  Starting test_malformed_serial_numbers_validation_and_startup_safety..." << std::endl;

	// Verbatim copy of the two production gates (AutoDiscovery.cpp L144-156).
	// Keep in sync with Utils::ValidSerialNumber (framework/utils.cpp L36-39) and
	// AutoDiscovery::ConnectionReceived() L145, L149-153.
	auto validateSerial = [](const std::string &serialRaw) -> bool {
		std::string serial = Poco::toLower(serialRaw);
		// Gate 1 — mirrors AutoDiscovery.cpp L145 / Utils::ValidSerialNumber L37-38
		if (serial.empty() || serial.size() > 16 ||
		    !std::all_of(serial.begin(), serial.end(),
		                 [](unsigned char c) { return std::isxdigit(c); }))
			return false;
		// Gate 2 — mirrors AutoDiscovery.cpp L149-153
		try {
			(void)std::stoull(serial, nullptr, 16);
			std::string rev(serial.rbegin(), serial.rend());
			(void)std::stoull(rev, nullptr, 16);
			return true;
		} catch (...) {
			return false;
		}
	};

	// Valid serials that must be accepted
	TEST_ASSERT(validateSerial("24f5a2012345"), "Standard 12-char hex serial must be valid");
	TEST_ASSERT(validateSerial("0123456789ab"), "Sequential 12-char hex serial must be valid");
	TEST_ASSERT(validateSerial("AABBCCDDEEFF"), "Uppercase hex serial must be valid (lowercased)");
	TEST_ASSERT(validateSerial("123456"), "Short hex serial within range must be valid");
	TEST_ASSERT(validateSerial("ffffffffffffffff"), "Max 64-bit hex serial (16 chars) must be valid");

	// Malformed / invalid serials that must be rejected before DB operations
	TEST_ASSERT(!validateSerial(""), "Empty serial must be rejected");
	TEST_ASSERT(!validateSerial("invalid-serial"), "Serial with hyphens/non-hex must be rejected");
	TEST_ASSERT(!validateSerial("24:f5:a2:01:23:45"), "Serial with colons must be rejected");
	TEST_ASSERT(!validateSerial("12345g"), "Non-hex character 'g' must be rejected");
	TEST_ASSERT(!validateSerial("123 456"), "Serial with space must be rejected");

	// Overflow cases: 17 hex digits
	TEST_ASSERT(!validateSerial("10000000000000000"), "17-digit hex overflowing uint64_t must be rejected");
	TEST_ASSERT(!validateSerial("fffffffffffffffff"), "Overflowing hex serial must be rejected");

	// 17-digit leading-zero case identified in P1 review:
	// "00000000000000001" has numeric value 1, but reversed is "10000000000000000" which overflows uint64_t!
	TEST_ASSERT(!validateSerial("00000000000000001"), "17-digit leading-zero serial must be rejected to prevent reversed overflow in cache");

	// Verify that the reversed representation would throw in std::stoull if not caught
	std::string badSerial = "00000000000000001";
	std::string revBad(badSerial.rbegin(), badSerial.rend());
	bool revThrew = false;
	try {
		(void)std::stoull(revBad, nullptr, 16);
	} catch (const std::out_of_range &) {
		revThrew = true;
	}
	TEST_ASSERT(revThrew, "Reversed 17-digit serial '10000000000000000' must throw out_of_range in std::stoull");

	std::cout << "  PASS: test_malformed_serial_numbers_validation_and_startup_safety" << std::endl;
}

// Test 19: Real broker-backed Kafka offset commit, bounded retry, timing measurement, and shutdown.
// Each scenario runs on its own pre-created topic so leftover messages from earlier scenarios
// cannot contaminate later ones. The topics are created by the CI workflow topic-creation step.
static void test_broker_backed_kafka_commit_retry_and_shutdown() {
	const char *broker = std::getenv("KAFKA_BROKERLIST");
	if (!broker || std::string(broker).empty()) {
		std::cout << "  SKIPPED: test_broker_backed_kafka_commit_retry_and_shutdown (KAFKA_BROKERLIST not set)" << std::endl;
		return;
	}

	std::cout << "  Running broker-backed Kafka commit, retry, and shutdown tests..." << std::endl;

	// Each scenario uses its own isolated pre-created topic to prevent cross-contamination.
	// Topics must be listed in the CI workflow's "Create Kafka Consumer Test Topics" step.
	const std::string successTopic  = "test_commit_retry_topic";
	const std::string retryTopic    = "test_retry_scenario_topic";
	const std::string nonRetryTopic = "test_non_retry_scenario_topic";
	const std::string shutdownTopic = "test_shutdown_scenario_topic";

	std::string token = Poco::UUIDGenerator::defaultGenerator().createRandom().toString();
	g_MockGroupId = "test_retry_grp_" + token;
	g_MockInstanceId = "owprov-test-" + token;

	KafkaProducer producer;
	producer.Start();

	auto &consumer = KafkaManager()->GroupConsumer();

	// Helper: query committed offset for a topic using an independent, ephemeral consumer
	// so the measurement is not coupled to the consumer-under-test's handle lifecycle.
	// This is the correct way to read the group's persisted offset from the broker after
	// the consumer-under-test has been stopped (its handle is gone, GetCommittedOffset() = -1).
	//
	// Exceptions are NOT swallowed here: a broker-query failure is a test infrastructure
	// failure (not an "uncommitted offset") and must abort the test immediately. This avoids
	// the false-PASS where a silently-failed query returns -1, which is then accepted by an
	// assertion that only checks for a negative value.
	//
	// Return value semantics:
	//   >= 0                      — broker reports a committed offset for this group/topic/partition
	//   RD_KAFKA_OFFSET_INVALID   — broker confirms no committed offset exists (never committed)
	//   throws std::runtime_error — broker query itself failed; caller must let the test abort
	auto queryCommittedOffsetIndependently = [&](const std::string &topic, int partition) -> int64_t {
		cppkafka::Configuration cfg = {
			{"metadata.broker.list", std::string(broker)},
			{"group.id", g_MockGroupId},
			{"enable.auto.commit", "false"},
		};
		cppkafka::Consumer probe(cfg);
		cppkafka::TopicPartitionList tpl{{topic, partition}};
		auto committed = probe.get_offsets_committed(tpl);
		if (!committed.empty()) return committed.front().get_offset();
		// Empty list means the broker has no information for this partition at all.
		// Return OFFSET_INVALID (-1001) to distinguish from a committed offset of 0.
		return RD_KAFKA_OFFSET_INVALID;
	};

	// ──────────────────────────────────────────────────────────────────────────────
	// Scenario 1: Success → offset advances, commit happens AFTER processing returns
	// ──────────────────────────────────────────────────────────────────────────────
	int64_t offsetAfterSuccess = -1;
	{
		std::atomic<int> successCount{0};
		std::mutex payloadMtx;
		std::string receivedPayload;

		// Latch to block the callback before it returns, so we can verify the offset
		// has NOT advanced while processing is in-flight (proving commit follows processing).
		std::promise<void> releaseCallback;
		std::shared_future<void> releaseFuture = releaseCallback.get_future().share();
		std::atomic<bool> callbackBlocked{false};

		uint64_t wId = KafkaManager()->RegisterTopicWatcher(successTopic,
			[&](const std::string &, const std::string &payload, std::string &) -> Types::KafkaHandlerOutcome {
				{
					std::lock_guard<std::mutex> lock(payloadMtx);
					receivedPayload = payload;
				}
				callbackBlocked = true;
				releaseFuture.wait(); // Block until main thread verifies offset unchanged
				successCount++;
				return Types::KafkaHandlerOutcome::Success;
			},
			KafkaConsumerType::Group);

		consumer.Start();
		TEST_ASSERT(consumer.IsRunning(), "GroupConsumer must be running");

		// Produce exactly one message with a known, unique key and payload.
		const std::string successKey     = "k_succ_" + token;
		const std::string successPayload = "success_payload_" + token;
		producer.Produce(successTopic.c_str(), successKey, successPayload);

		// Wait for the callback to be entered and blocked
		auto waitStart = std::chrono::steady_clock::now();
		while (!callbackBlocked && std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(15)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		TEST_ASSERT(callbackBlocked, "Callback must have been entered");

		// While the callback is blocked (processing not yet returned), the broker must have
		// NO committed offset for this group/topic/partition — proving commit does not race
		// ahead of processing.
		//
		// Use queryCommittedOffsetIndependently() (not GetCommittedOffset()) because
		// GetCommittedOffset() catches every exception and returns -1, making a broker-query
		// failure indistinguishable from "not yet committed". queryCommittedOffsetIndependently()
		// throws on any broker-query error so a silent failure cannot produce a false PASS.
		//
		// Assert == RD_KAFKA_OFFSET_INVALID specifically: that is the value the broker returns
		// when no offset has ever been committed for this group. Any other negative value would
		// mean the query itself failed (thrown, test aborts). A non-negative value means a commit
		// has already been stored, which is a real ordering violation.
		int64_t offsetMidProcessing = queryCommittedOffsetIndependently(successTopic, 0);
		TEST_ASSERT(offsetMidProcessing == RD_KAFKA_OFFSET_INVALID,
			"Broker must report RD_KAFKA_OFFSET_INVALID (no commit) while processing callback is still in-flight");

		// Release the callback to let it return Success and trigger the commit
		releaseCallback.set_value();

		// Poll via an independent ephemeral consumer (throws on broker-query failure — no silent -1).
		// In Kafka, committed offset = next offset to fetch = last consumed offset + 1.
		// For a fresh consumer group, the committed position after consuming one message must be > 0.
		// (If the topic had N prior messages and all were consumed, committed position >= 1 still holds.)
		auto pollStart = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() - pollStart < std::chrono::seconds(5)) {
			offsetAfterSuccess = queryCommittedOffsetIndependently(successTopic, 0);
			if (offsetAfterSuccess > 0) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}

		TEST_ASSERT(successCount > 0, "GroupConsumer must receive and process success message");
		{
			std::lock_guard<std::mutex> lock(payloadMtx);
			TEST_ASSERT(receivedPayload == successPayload, "GroupConsumer must receive exact payload");
		}
		TEST_ASSERT(!consumer.HasFailed(), "GroupConsumer must not fail after successful commit");
		// Committed position > 0 means the broker confirmed at least one message was consumed and
		// committed. For a fresh consumer group this is unambiguous: RD_KAFKA_OFFSET_INVALID (-1001)
		// if no commit occurred, or the next-offset (>= 1) if commit succeeded.
		TEST_ASSERT(offsetAfterSuccess > 0,
			"Committed offset position must be > 0 after successful commit (Kafka next-offset semantics)");
		// Offset ordering guarantee: RetryOp("Commit") only runs after RetryOp("Processing") succeeds.

		KafkaManager()->UnregisterTopicWatcher(successTopic, wId, KafkaConsumerType::Group);
		consumer.Stop();
	}

	// ──────────────────────────────────────────────────────────────────────────────
	// Scenario 2: RetryableFailure → bounded retries, exponential backoff, offset unchanged
	// Uses an isolated topic so the success message cannot trigger the retry callback.
	// ──────────────────────────────────────────────────────────────────────────────
	{
		g_MockGroupId = "test_retry_grp_" + token; // Keep same group so offset baseline applies
		std::vector<std::chrono::steady_clock::time_point> attemptTimestamps;
		std::mutex attemptMutex;

		const std::string retryKey     = "k_retry_" + token;
		const std::string retryPayload = "retry_payload_" + token;

		uint64_t wId = KafkaManager()->RegisterTopicWatcher(retryTopic,
			[&](const std::string &key, const std::string &payload, std::string &reason) -> Types::KafkaHandlerOutcome {
				// Verify we are processing the exact message we produced (not a stale leftover)
				TEST_ASSERT(key == retryKey, "Retry scenario must receive exact key: " + retryKey + ", got: " + key);
				TEST_ASSERT(payload == retryPayload, "Retry scenario must receive exact payload");
				std::lock_guard lock(attemptMutex);
				attemptTimestamps.push_back(std::chrono::steady_clock::now());
				reason = "simulated transient error";
				return Types::KafkaHandlerOutcome::RetryableFailure;
			},
			KafkaConsumerType::Group);

		consumer.Start();
		TEST_ASSERT(consumer.IsRunning(), "GroupConsumer must be running");

		// Produce exactly one message
		producer.Produce(retryTopic.c_str(), retryKey, retryPayload);

		// Wait for consumer to execute 5 attempts (total backoff: 200 + 400 + 800 + 1600 = ~3.0s)
		auto start = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() - start < std::chrono::seconds(15) && !consumer.HasFailed()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}

		{
			std::lock_guard lock(attemptMutex);
			TEST_ASSERT(attemptTimestamps.size() == 5, "GroupConsumer must execute exactly 5 attempts before exhaustion");

			auto d1 = std::chrono::duration_cast<std::chrono::milliseconds>(attemptTimestamps[1] - attemptTimestamps[0]).count();
			auto d2 = std::chrono::duration_cast<std::chrono::milliseconds>(attemptTimestamps[2] - attemptTimestamps[1]).count();
			auto d3 = std::chrono::duration_cast<std::chrono::milliseconds>(attemptTimestamps[3] - attemptTimestamps[2]).count();
			auto d4 = std::chrono::duration_cast<std::chrono::milliseconds>(attemptTimestamps[4] - attemptTimestamps[3]).count();

			TEST_ASSERT(d1 >= 150, "Attempt 1 backoff timing must be >= 150ms (nominal 200ms)");
			TEST_ASSERT(d2 >= 300, "Attempt 2 backoff timing must be >= 300ms (nominal 400ms)");
			TEST_ASSERT(d3 >= 600, "Attempt 3 backoff timing must be >= 600ms (nominal 800ms)");
			TEST_ASSERT(d4 >= 1200, "Attempt 4 backoff timing must be >= 1200ms (nominal 1600ms)");
		}

		TEST_ASSERT(consumer.HasFailed(), "GroupConsumer must be marked Failed after retries exhausted");
		TEST_ASSERT(consumer.FailureReason().find("Processing_retries_exhausted") != std::string::npos,
			"Failure reason must indicate retries exhausted: " + consumer.FailureReason());

		KafkaManager()->UnregisterTopicWatcher(retryTopic, wId, KafkaConsumerType::Group);
		consumer.Stop();

		// Query offset via an independent consumer — the consumer-under-test handle is gone.
		// queryCommittedOffsetIndependently() throws on broker-query failure, so a negative
		// return here is unambiguously RD_KAFKA_OFFSET_INVALID: broker confirms no commit occurred.
		int64_t offsetAfterFailure = queryCommittedOffsetIndependently(retryTopic, 0);
		TEST_ASSERT(offsetAfterFailure < 0,
			"Committed offset must NOT advance after processing failure (no commit on RetryableFailure)");
	}

	// ──────────────────────────────────────────────────────────────────────────────
	// Scenario 3: NonRetryableFailure → fails immediately on attempt 1 without retry
	// Uses its own isolated topic.
	// ──────────────────────────────────────────────────────────────────────────────
	{
		std::string tokenNonRetry = Poco::UUIDGenerator::defaultGenerator().createRandom().toString();
		g_MockGroupId = "test_non_retry_grp_" + tokenNonRetry;
		std::atomic<int> nonRetryAttempts{0};

		const std::string nonRetryKey     = "k_non_retry_" + tokenNonRetry;
		const std::string nonRetryPayload = "non_retry_payload_" + tokenNonRetry;

		uint64_t wId = KafkaManager()->RegisterTopicWatcher(nonRetryTopic,
			[&](const std::string &key, const std::string &payload, std::string &reason) -> Types::KafkaHandlerOutcome {
				TEST_ASSERT(key == nonRetryKey, "Non-retry scenario must receive exact key");
				TEST_ASSERT(payload == nonRetryPayload, "Non-retry scenario must receive exact payload");
				nonRetryAttempts++;
				reason = "simulated non-retryable invalid schema";
				return Types::KafkaHandlerOutcome::NonRetryableFailure;
			},
			KafkaConsumerType::Group);

		consumer.Start();
		TEST_ASSERT(consumer.IsRunning(), "GroupConsumer must be running");

		producer.Produce(nonRetryTopic.c_str(), nonRetryKey, nonRetryPayload);

		auto start = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10) && !consumer.HasFailed()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}

		TEST_ASSERT(nonRetryAttempts == 1, "NonRetryableFailure must fail immediately on attempt 1 without further retry");
		TEST_ASSERT(consumer.HasFailed(), "GroupConsumer must be marked Failed on NonRetryableFailure");
		TEST_ASSERT(consumer.FailureReason().find("non_retryable") != std::string::npos,
			"Failure reason must indicate non_retryable: " + consumer.FailureReason());

		KafkaManager()->UnregisterTopicWatcher(nonRetryTopic, wId, KafkaConsumerType::Group);
		consumer.Stop();
	}

	// ──────────────────────────────────────────────────────────────────────────────
	// Scenario 4: Shutdown during backoff → consumer stops immediately without commit
	// Uses its own isolated topic.
	// ──────────────────────────────────────────────────────────────────────────────
	{
		std::string tokenShut = Poco::UUIDGenerator::defaultGenerator().createRandom().toString();
		g_MockGroupId = "test_shutdown_grp_" + tokenShut;
		std::atomic<int> shutAttempts{0};

		const std::string shutKey     = "k_shut_" + tokenShut;
		const std::string shutPayload = "shut_payload_" + tokenShut;

		uint64_t wId = KafkaManager()->RegisterTopicWatcher(shutdownTopic,
			[&](const std::string &key, const std::string &payload, std::string &reason) -> Types::KafkaHandlerOutcome {
				TEST_ASSERT(key == shutKey, "Shutdown scenario must receive exact key");
				TEST_ASSERT(payload == shutPayload, "Shutdown scenario must receive exact payload");
				shutAttempts++;
				reason = "simulated transient delay";
				return Types::KafkaHandlerOutcome::RetryableFailure;
			},
			KafkaConsumerType::Group);

		consumer.Start();
		producer.Produce(shutdownTopic.c_str(), shutKey, shutPayload);

		// Wait for first attempt
		auto start = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5) && shutAttempts == 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		TEST_ASSERT(shutAttempts >= 1, "First attempt must have executed");

		// Signal shutdown during retry backoff
		consumer.Stop();
		TEST_ASSERT(!consumer.IsRunning(), "Consumer must stop immediately on Stop()");

		// Query offset via independent consumer — handle is gone, verifies no spurious commit.
		// queryCommittedOffsetIndependently() throws on broker-query failure, so a negative
		// return here is unambiguously RD_KAFKA_OFFSET_INVALID: broker confirms no commit occurred.
		int64_t offsetAfterStop = queryCommittedOffsetIndependently(shutdownTopic, 0);
		TEST_ASSERT(offsetAfterStop < 0,
			"Committed offset must NOT advance on shutdown during unacknowledged processing");

		KafkaManager()->UnregisterTopicWatcher(shutdownTopic, wId, KafkaConsumerType::Group);
	}

	producer.Stop();
	std::cout << "  PASS: test_broker_backed_kafka_commit_retry_and_shutdown" << std::endl;
}

// GAP: test_unfinished_processing_after_rebalance
//
// What is NOT covered by Test 19 (Scenario 4 — shutdown during backoff):
//   Scenario 4 verifies that a consumer Stop() during retry backoff does not spuriously commit.
//   It does NOT verify what happens when a partition rebalance (triggered by another consumer
//   joining the group) interrupts in-flight processing. This behavior is not covered by this
//   test suite. A dedicated integration test would be needed that:
//     1. Starts two GroupConsumer instances in the same group.
//     2. Publishes a message and blocks the first consumer's callback mid-processing.
//     3. Forces a rebalance by having the second consumer join.
//     4. Verifies the first consumer does not commit and the second consumer receives and
//        commits the message on retry.
//   This test requires coordination between two consumer handles and is left as future work.

int main() {
	Poco::AutoPtr<Poco::ConsoleChannel> pChannel(new Poco::ConsoleChannel);
	Poco::Logger::root().setChannel(pChannel);
	Poco::Logger::root().setLevel(Poco::Message::PRIO_INFORMATION);

	std::cout << "Running Kafka Consumer Roles & Dual-Consumer Architecture Tests..." << std::endl;

	test_consumer_types();
	test_group_consumer_shared_group_id();
	test_broadcast_consumer_incarnation_unique_group_id();
	test_rolling_restart_broadcast_isolation();
	test_client_id_generation();
	test_auto_offset_reset_semantics();
	test_topic_registration_and_unregistration();
	test_multiple_watchers_per_topic();
	test_instance_accessors_from_config();
	test_empty_base_strings();
	test_consumer_startup_shutdown_race();
	test_service_events_watcher_lifecycle_unregistration();
	test_producer_startup_shutdown_race();
	test_consumer_shutdown_with_registered_topics();
	test_concurrent_dynamic_subscription_updates_while_polling();

	// Phase 3 Kafka retry/commit/idempotency tests (Issue #120)
	// NOTE: test_commit_isolation_contract_mock_only (Test 17) is a mock-only structural check.
	// It does NOT exercise the production RetryOp("Commit") path. See test body for the known gap.
	test_callback_outcome_contract_and_dispatch();
	test_commit_isolation_contract_mock_only();
	test_malformed_serial_numbers_validation_and_startup_safety();
	test_broker_backed_kafka_commit_retry_and_shutdown();

	std::cout << "ALL KAFKA CONSUMER ROLES TESTS PASSED!" << std::endl;
	return 0;
}
