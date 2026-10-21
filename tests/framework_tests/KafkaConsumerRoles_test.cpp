//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "Poco/ConsoleChannel.h"
#include "Poco/UUIDGenerator.h"
#include "framework/KafkaManager.h"
#include "framework/KafkaTopics.h"

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
	Types::TopicNotifyFunction cb = [](const std::string &, const std::string &) {};

	// Register connection on GroupConsumer
	uint64_t id_conn = KafkaManager()->RegisterTopicWatcher(
		KafkaTopics::CONNECTION, cb, KafkaConsumerType::Group);
	TEST_ASSERT(id_conn > 0, "Registration must return positive ID");

	// Register service_events on BroadcastConsumer
	uint64_t id_se = KafkaManager()->RegisterTopicWatcher(
		KafkaTopics::SERVICE_EVENTS, cb, KafkaConsumerType::Broadcast);
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
		Types::TopicNotifyFunction callback() {
			return [this](const std::string &, const std::string &msg) {
				std::lock_guard g(mtx);
				received.insert(msg);
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

	std::cout << "ALL KAFKA CONSUMER ROLES TESTS PASSED!" << std::endl;
	return 0;
}
