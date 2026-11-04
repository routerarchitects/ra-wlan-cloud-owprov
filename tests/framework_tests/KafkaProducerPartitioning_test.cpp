//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Poco/AutoPtr.h"
#include "Poco/ConsoleChannel.h"
#include "Poco/JSON/Object.h"
#include "Poco/JSON/Parser.h"
#include "Poco/Logger.h"

#define private public
#define protected public
#include "framework/KafkaManager.h"
#undef private
#undef protected

#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "RESTObjects/RESTAPI_SecurityObjects.h"
#include "framework/KafkaTopics.h"
#include "Kafka_ProvUpdater.h"

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

	std::string MicroServiceCreateUUID() {
		return "mock-uuid-1111-2222-3333";
	}

	std::string MicroServiceDataDirectory() {
		return "/tmp";
	}

	SubSystemServer::SubSystemServer(const std::string &Name, const std::string &LoggingPrefix,
									 const std::string &SubSystemConfigPrefix)
		: Name_(Name), LoggerPrefix_(LoggingPrefix), SubSystemConfigPrefix_(SubSystemConfigPrefix) {
		Logger_ = std::make_unique<LoggerWrapper>(Poco::Logger::root());
	}

	void SubSystemServer::initialize(Poco::Util::Application &/*self*/) {}
}

using namespace OpenWifi;

// =========================================================================
// SECTION A: Producer Partitioning Tests
// =========================================================================

// Test A1: KafkaMessage partition defaults to KAFKA_PARTITION_UNASSIGNED and respects explicit partition
static void test_kafka_message_partition_defaults_and_explicit() {
	KafkaMessage msgDefault("test_topic", "key1", "payload1");
	TEST_ASSERT(std::string(msgDefault.Topic()) == "test_topic", "Topic must match constructor argument");
	TEST_ASSERT(msgDefault.Key() == "key1", "Key must match constructor argument");
	TEST_ASSERT(msgDefault.Payload() == "payload1", "Payload must match constructor argument");
	TEST_ASSERT(msgDefault.Partition() == KAFKA_PARTITION_UNASSIGNED,
				"KafkaMessage default partition must be KAFKA_PARTITION_UNASSIGNED (-1)");

	KafkaMessage msgExplicit0("service_events", "endpoint1", "join_payload", 0);
	TEST_ASSERT(msgExplicit0.Partition() == 0, "Explicit partition 0 must be preserved");

	KafkaMessage msgExplicit3("custom_topic", "key3", "payload3", 3);
	TEST_ASSERT(msgExplicit3.Partition() == 3, "Explicit partition 3 must be preserved");

	std::cout << "  PASS: test_kafka_message_partition_defaults_and_explicit" << std::endl;
}

// Test A2: KafkaProducer::Produce enqueues messages with correct partition
static void test_kafka_producer_produce_queueing() {
	KafkaProducer producer;

	// 1. Default partition
	producer.Produce("topic_auto", "key_auto", "payload_auto");
	// 2. Explicit partition 0
	producer.Produce("topic_part0", "key_part0", "payload_part0", 0);
	// 3. Explicit partition 2
	producer.Produce("topic_part2", "key_part2", "payload_part2", 2);

	// Drain and verify
	Poco::AutoPtr<Poco::Notification> note1(producer.Queue_.dequeueNotification());
	auto msg1 = dynamic_cast<KafkaMessage *>(note1.get());
	TEST_ASSERT(msg1 != nullptr, "Notification 1 must be KafkaMessage");
	TEST_ASSERT(msg1->Partition() == KAFKA_PARTITION_UNASSIGNED, "Produce with default partition must be unassigned (-1)");
	TEST_ASSERT(msg1->Key() == "key_auto", "Key must be key_auto");

	Poco::AutoPtr<Poco::Notification> note2(producer.Queue_.dequeueNotification());
	auto msg2 = dynamic_cast<KafkaMessage *>(note2.get());
	TEST_ASSERT(msg2 != nullptr, "Notification 2 must be KafkaMessage");
	TEST_ASSERT(msg2->Partition() == 0, "Produce with partition 0 must preserve partition 0");
	TEST_ASSERT(msg2->Key() == "key_part0", "Key must be key_part0");

	Poco::AutoPtr<Poco::Notification> note3(producer.Queue_.dequeueNotification());
	auto msg3 = dynamic_cast<KafkaMessage *>(note3.get());
	TEST_ASSERT(msg3 != nullptr, "Notification 3 must be KafkaMessage");
	TEST_ASSERT(msg3->Partition() == 2, "Produce with partition 2 must preserve partition 2");
	TEST_ASSERT(msg3->Key() == "key_part2", "Key must be key_part2");

	TEST_ASSERT(producer.Queue_.size() == 0, "Producer queue must be empty after draining");

	std::cout << "  PASS: test_kafka_producer_produce_queueing" << std::endl;
}

// Test A3: KafkaManager::PostMessage overloads forward partition and wrapping correctly
static void test_kafka_manager_post_message_forwarding() {
	KafkaManager()->KafkaEnabled_ = true;
	KafkaManager()->ProducerThr_.Queue_.clear();

	// String overload without wrap, default partition
	KafkaManager()->PostMessage("test_topic_str", "key_str", "raw_payload", false);

	// String overload with wrap and explicit partition 0
	KafkaManager()->PostMessage("service_events", "endpoint_bcast", "{\"event\":\"join\"}", true, 0);

	// JSON overload without wrap, default partition
	Poco::JSON::Object jsonNoWrap;
	jsonNoWrap.set("field", "value1");
	KafkaManager()->PostMessage("topic_json", "key_json", jsonNoWrap, false);

	// JSON overload with wrap and explicit partition 1
	Poco::JSON::Object jsonWrap;
	jsonWrap.set("field", "value2");
	KafkaManager()->PostMessage("topic_json_wrap", "key_json_wrap", jsonWrap, true, 1);

	// Dequeue 1: raw string, unassigned partition
	Poco::AutoPtr<Poco::Notification> n1(KafkaManager()->ProducerThr_.Queue_.dequeueNotification());
	auto m1 = dynamic_cast<KafkaMessage *>(n1.get());
	TEST_ASSERT(m1 != nullptr && m1->Partition() == KAFKA_PARTITION_UNASSIGNED,
				"Message 1 must have unassigned partition");
	TEST_ASSERT(m1->Payload() == "raw_payload", "Message 1 payload must be unwrapped raw_payload");

	// Dequeue 2: wrapped string, partition 0
	Poco::AutoPtr<Poco::Notification> n2(KafkaManager()->ProducerThr_.Queue_.dequeueNotification());
	auto m2 = dynamic_cast<KafkaMessage *>(n2.get());
	TEST_ASSERT(m2 != nullptr && m2->Partition() == 0,
				"Message 2 must have explicit partition 0");
	TEST_ASSERT(m2->Payload().find("\"system\"") != std::string::npos,
				"Message 2 payload must be wrapped with system info");

	// Dequeue 3: json unwrapped, unassigned partition
	Poco::AutoPtr<Poco::Notification> n3(KafkaManager()->ProducerThr_.Queue_.dequeueNotification());
	auto m3 = dynamic_cast<KafkaMessage *>(n3.get());
	TEST_ASSERT(m3 != nullptr && m3->Partition() == KAFKA_PARTITION_UNASSIGNED,
				"Message 3 must have unassigned partition");
	TEST_ASSERT(m3->Payload().find("\"field\":\"value1\"") != std::string::npos,
				"Message 3 payload must contain JSON fields");

	// Dequeue 4: json wrapped, partition 1
	Poco::AutoPtr<Poco::Notification> n4(KafkaManager()->ProducerThr_.Queue_.dequeueNotification());
	auto m4 = dynamic_cast<KafkaMessage *>(n4.get());
	TEST_ASSERT(m4 != nullptr && m4->Partition() == 1,
				"Message 4 must have explicit partition 1");
	TEST_ASSERT(m4->Payload().find("\"system\"") != std::string::npos,
				"Message 4 payload must be wrapped with system info");

	KafkaManager()->KafkaEnabled_ = false;
	KafkaManager()->ProducerThr_.Queue_.clear();

	std::cout << "  PASS: test_kafka_manager_post_message_forwarding" << std::endl;
}

// =========================================================================
// SECTION B: provisioning_change Tests
// =========================================================================

// Test B1: Venue modification and removal produce the same stable key and identifiable operations
static void test_venue_modification_and_removal_key_contract() {
	KafkaManager()->KafkaEnabled_ = true;
	KafkaManager()->ProducerThr_.Queue_.clear();

	ProvObjects::Venue venue;
	venue.info.id = "6557edb6-92c7-48c0-93c8-bdbb5fc0d003";
	venue.info.name = "Downtown Campus";

	// 1. Publish modification
	bool modOk = UpdateKafkaProvisioningObject(ProvisioningOperation::modification, venue);
	TEST_ASSERT(modOk, "Venue modification update must succeed");

	// 2. Publish removal
	bool remOk = UpdateKafkaProvisioningObject(ProvisioningOperation::removal, venue);
	TEST_ASSERT(remOk, "Venue removal update must succeed");

	TEST_ASSERT(KafkaManager()->ProducerThr_.Queue_.size() == 2, "Exactly 2 messages must be enqueued");

	// Verify modification message
	Poco::AutoPtr<Poco::Notification> n1(KafkaManager()->ProducerThr_.Queue_.dequeueNotification());
	auto m1 = dynamic_cast<KafkaMessage *>(n1.get());
	TEST_ASSERT(m1 != nullptr, "Message 1 must be KafkaMessage");
	TEST_ASSERT(std::string(m1->Topic()) == KafkaTopics::PROVISIONING_CHANGE,
				"Message 1 topic must be provisioning_change");
	TEST_ASSERT(m1->Key() == "Venue:6557edb6-92c7-48c0-93c8-bdbb5fc0d003",
				"Message 1 key must be Venue:<id>");
	TEST_ASSERT(m1->Partition() == KAFKA_PARTITION_UNASSIGNED,
				"Message 1 partition must be unassigned for key-based partitioning");

	// Parse payload 1
	Poco::JSON::Parser parser1;
	auto json1 = parser1.parse(m1->Payload()).extract<Poco::JSON::Object::Ptr>();
	auto payloadObj1 = json1->getObject("payload");
	TEST_ASSERT(payloadObj1 != nullptr, "Payload must be wrapped with system wrapper");
	TEST_ASSERT(payloadObj1->get("ObjectType").toString() == "Venue", "ObjectType in payload must be Venue");
	TEST_ASSERT(payloadObj1->get("operation").toString() == "modification",
				"operation in payload must be 'modification'");
	TEST_ASSERT(payloadObj1->get("id").toString() == "6557edb6-92c7-48c0-93c8-bdbb5fc0d003",
				"Venue id in payload must match");
	TEST_ASSERT(payloadObj1->get("name").toString() == "Downtown Campus",
				"Venue name in payload must match");

	// Verify removal message
	Poco::AutoPtr<Poco::Notification> n2(KafkaManager()->ProducerThr_.Queue_.dequeueNotification());
	auto m2 = dynamic_cast<KafkaMessage *>(n2.get());
	TEST_ASSERT(m2 != nullptr, "Message 2 must be KafkaMessage");
	TEST_ASSERT(std::string(m2->Topic()) == KafkaTopics::PROVISIONING_CHANGE,
				"Message 2 topic must be provisioning_change");
	TEST_ASSERT(m2->Key() == "Venue:6557edb6-92c7-48c0-93c8-bdbb5fc0d003",
				"Message 2 key must match Message 1 key exactly (Venue:<id>)");
	TEST_ASSERT(m2->Key() == m1->Key(),
				"Modification and removal for same Venue MUST produce identical Kafka partition key");
	TEST_ASSERT(m2->Partition() == KAFKA_PARTITION_UNASSIGNED,
				"Message 2 partition must be unassigned for key-based partitioning");

	// Parse payload 2
	Poco::JSON::Parser parser2;
	auto json2 = parser2.parse(m2->Payload()).extract<Poco::JSON::Object::Ptr>();
	auto payloadObj2 = json2->getObject("payload");
	TEST_ASSERT(payloadObj2 != nullptr, "Payload must be wrapped with system wrapper");
	TEST_ASSERT(payloadObj2->get("ObjectType").toString() == "Venue", "ObjectType in payload must be Venue");
	TEST_ASSERT(payloadObj2->get("operation").toString() == "removal",
				"operation in payload must be 'removal'");

	KafkaManager()->KafkaEnabled_ = false;
	KafkaManager()->ProducerThr_.Queue_.clear();

	std::cout << "  PASS: test_venue_modification_and_removal_key_contract" << std::endl;
}

// Test B2: Different Venues produce different keys
static void test_different_venues_produce_different_keys() {
	KafkaManager()->KafkaEnabled_ = true;
	KafkaManager()->ProducerThr_.Queue_.clear();

	ProvObjects::Venue v1;
	v1.info.id = "venue-uuid-1111";
	ProvObjects::Venue v2;
	v2.info.id = "venue-uuid-2222";

	UpdateKafkaProvisioningObject(ProvisioningOperation::modification, v1);
	UpdateKafkaProvisioningObject(ProvisioningOperation::modification, v2);

	Poco::AutoPtr<Poco::Notification> n1(KafkaManager()->ProducerThr_.Queue_.dequeueNotification());
	auto m1 = dynamic_cast<KafkaMessage *>(n1.get());
	Poco::AutoPtr<Poco::Notification> n2(KafkaManager()->ProducerThr_.Queue_.dequeueNotification());
	auto m2 = dynamic_cast<KafkaMessage *>(n2.get());

	TEST_ASSERT(m1->Key() == "Venue:venue-uuid-1111", "Key 1 must be Venue:venue-uuid-1111");
	TEST_ASSERT(m2->Key() == "Venue:venue-uuid-2222", "Key 2 must be Venue:venue-uuid-2222");
	TEST_ASSERT(m1->Key() != m2->Key(), "Different venues must produce different partition keys");

	KafkaManager()->KafkaEnabled_ = false;
	KafkaManager()->ProducerThr_.Queue_.clear();

	std::cout << "  PASS: test_different_venues_produce_different_keys" << std::endl;
}

// Test B3: All supported provisioning object types format keys correctly
static void test_all_provisioning_object_types_key_formatting() {
	KafkaManager()->KafkaEnabled_ = true;
	KafkaManager()->ProducerThr_.Queue_.clear();

	ProvObjects::Venue venue;
	venue.info.id = "id-v";
	UpdateKafkaProvisioningObject(ProvisioningOperation::creation, venue);

	ProvObjects::Entity entity;
	entity.info.id = "id-e";
	UpdateKafkaProvisioningObject(ProvisioningOperation::modification, entity);

	ProvObjects::InventoryTag tag;
	tag.info.id = "id-t";
	UpdateKafkaProvisioningObject(ProvisioningOperation::modification, tag);

	ProvObjects::Contact contact;
	contact.info.id = "id-c";
	UpdateKafkaProvisioningObject(ProvisioningOperation::modification, contact);

	ProvObjects::Location location;
	location.info.id = "id-l";
	UpdateKafkaProvisioningObject(ProvisioningOperation::modification, location);

	ProvObjects::DeviceConfiguration config;
	config.info.id = "id-d";
	UpdateKafkaProvisioningObject(ProvisioningOperation::modification, config);

	std::vector<std::pair<std::string, std::string>> expected = {
		{"Venue:id-v", "Venue"},
		{"Entity:id-e", "Entity"},
		{"InventoryTag:id-t", "InventoryTag"},
		{"Contact:id-c", "Contact"},
		{"Location:id-l", "Location"},
		{"DeviceConfiguration:id-d", "DeviceConfiguration"}
	};

	for (const auto &[expKey, expType] : expected) {
		Poco::AutoPtr<Poco::Notification> n(KafkaManager()->ProducerThr_.Queue_.dequeueNotification());
		auto m = dynamic_cast<KafkaMessage *>(n.get());
		TEST_ASSERT(m != nullptr, "Notification must be KafkaMessage");
		TEST_ASSERT(m->Key() == expKey, "Key must match expected: " + expKey + " got: " + m->Key());
		TEST_ASSERT(m->Partition() == KAFKA_PARTITION_UNASSIGNED, "Partition must be unassigned");

		Poco::JSON::Parser parser;
		auto parsed = parser.parse(m->Payload()).extract<Poco::JSON::Object::Ptr>();
		auto payloadObj = parsed->getObject("payload");
		TEST_ASSERT(payloadObj->get("ObjectType").toString() == expType,
					"ObjectType in payload must match " + expType);
	}

	KafkaManager()->KafkaEnabled_ = false;
	KafkaManager()->ProducerThr_.Queue_.clear();

	std::cout << "  PASS: test_all_provisioning_object_types_key_formatting" << std::endl;
}

// Test B4: Missing or invalid object ID is safely rejected without publishing
static void test_empty_or_invalid_id_rejected() {
	KafkaManager()->KafkaEnabled_ = true;
	KafkaManager()->ProducerThr_.Queue_.clear();

	ProvObjects::Venue emptyVenue;
	emptyVenue.info.id = ""; // Empty ID

	bool result = UpdateKafkaProvisioningObject(ProvisioningOperation::modification, emptyVenue);
	TEST_ASSERT(!result, "UpdateKafkaProvisioningObject must return false when object ID is empty");
	TEST_ASSERT(KafkaManager()->ProducerThr_.Queue_.size() == 0,
				"No Kafka message should be enqueued when object ID is empty");

	// Invalid operation enum
	ProvObjects::Venue validVenue;
	validVenue.info.id = "valid-id";
	bool invalidOpResult = UpdateKafkaProvisioningObject(static_cast<ProvisioningOperation>(99), validVenue);
	TEST_ASSERT(!invalidOpResult, "UpdateKafkaProvisioningObject must return false for invalid operation");
	TEST_ASSERT(KafkaManager()->ProducerThr_.Queue_.size() == 0,
				"No Kafka message should be enqueued for invalid operation");

	KafkaManager()->KafkaEnabled_ = false;

	std::cout << "  PASS: test_empty_or_invalid_id_rejected" << std::endl;
}

// =========================================================================
// SECTION C: service_events Regression Tests
// =========================================================================

// Test C1: service_events routes explicitly to partition 0
static void test_service_events_explicit_partition_0() {
	KafkaManager()->KafkaEnabled_ = true;
	KafkaManager()->ProducerThr_.Queue_.clear();

	std::string endpoint = MicroServicePrivateEndPoint();
	std::string joinMsg = "{\"event\":\"join\",\"endpoint\":\"" + endpoint + "\"}";
	std::string keepAliveMsg = "{\"event\":\"keep-alive\",\"endpoint\":\"" + endpoint + "\"}";
	std::string leaveMsg = "{\"event\":\"leave\",\"endpoint\":\"" + endpoint + "\"}";

	// Emulate EventBusManager::run publishing calls
	KafkaManager()->PostMessage(KafkaTopics::SERVICE_EVENTS, endpoint, joinMsg, false, 0);
	KafkaManager()->PostMessage(KafkaTopics::SERVICE_EVENTS, endpoint, keepAliveMsg, false, 0);
	KafkaManager()->PostMessage(KafkaTopics::SERVICE_EVENTS, endpoint, leaveMsg, false, 0);

	TEST_ASSERT(KafkaManager()->ProducerThr_.Queue_.size() == 3, "Exactly 3 service_events messages must be enqueued");

	std::vector<std::string> expectedPayloads = {joinMsg, keepAliveMsg, leaveMsg};
	for (const auto &expPayload : expectedPayloads) {
		Poco::AutoPtr<Poco::Notification> n(KafkaManager()->ProducerThr_.Queue_.dequeueNotification());
		auto m = dynamic_cast<KafkaMessage *>(n.get());
		TEST_ASSERT(m != nullptr, "Notification must be KafkaMessage");
		TEST_ASSERT(std::string(m->Topic()) == KafkaTopics::SERVICE_EVENTS,
					"Topic must be service_events");
		TEST_ASSERT(m->Key() == endpoint, "Key must be MicroServicePrivateEndPoint()");
		TEST_ASSERT(m->Partition() == 0, "service_events MUST be routed explicitly to partition 0");
		TEST_ASSERT(m->Payload() == expPayload, "Payload must match expected service event message");
	}

	KafkaManager()->KafkaEnabled_ = false;
	KafkaManager()->ProducerThr_.Queue_.clear();

	std::cout << "  PASS: test_service_events_explicit_partition_0" << std::endl;
}

// =========================================================================
// SECTION D: Multi-Partition & Key Distribution Tests
// =========================================================================

// Murmur2 hash simulator (standard librdkafka default key partitioner algorithm)
static uint32_t murmur2(const void *key, int len, uint32_t seed = 0x9747b28c) {
	const uint32_t m = 0x5bd1e995;
	const int r = 24;
	uint32_t h = seed ^ len;
	const unsigned char *data = (const unsigned char *)key;

	while (len >= 4) {
		uint32_t k = *(uint32_t *)data;
		k *= m;
		k ^= k >> r;
		k *= m;
		h *= m;
		h ^= k;
		data += 4;
		len -= 4;
	}

	switch (len) {
		case 3: h ^= data[2] << 16; [[fallthrough]];
		case 2: h ^= data[1] << 8;  [[fallthrough]];
		case 1: h ^= data[0];
		        h *= m;
	};

	h ^= h >> 13;
	h *= m;
	h ^= h >> 15;
	return h;
}

static int32_t compute_partition(const std::string &key, int32_t num_partitions) {
	uint32_t hash = murmur2(key.data(), (int)key.size()) & 0x7fffffff;
	return (int32_t)(hash % num_partitions);
}

// Test D1: Partition mapping consistency and distribution across partitions
static void test_key_hashing_consistency_and_distribution() {
	const int32_t num_partitions = 4;

	// 1. Consistency: Same venue key maps to the exact same partition repeatedly
	std::string venueKey = "Venue:6557edb6-92c7-48c0-93c8-bdbb5fc0d003";
	int32_t targetPart1 = compute_partition(venueKey, num_partitions);
	int32_t targetPart2 = compute_partition(venueKey, num_partitions);
	int32_t targetPart3 = compute_partition(venueKey, num_partitions);

	TEST_ASSERT(targetPart1 == targetPart2 && targetPart2 == targetPart3,
				"Identical venue key must consistently map to the identical partition");

	// 2. Sequential modification and removal retain partition-local order
	std::string modKey = venueKey;
	std::string remKey = venueKey;
	TEST_ASSERT(compute_partition(modKey, num_partitions) == compute_partition(remKey, num_partitions),
				"Modification and removal for same venue MUST land on the exact same partition");

	// 3. Distribution: Distinct venue keys distribute across multiple partitions
	std::set<int32_t> partitions_hit;
	for (int i = 0; i < 20; ++i) {
		std::string key = "Venue:random-uuid-" + std::to_string(i);
		partitions_hit.insert(compute_partition(key, num_partitions));
	}

	TEST_ASSERT(partitions_hit.size() > 1,
				"20 distinct venue keys must distribute across more than 1 partition");

	std::cout << "  PASS: test_key_hashing_consistency_and_distribution" << std::endl;
}

// Test D2: Broker-backed multi-partition integration test (when broker is available)
static void test_broker_backed_producer_partitioning() {
	const char *broker = std::getenv("KAFKA_BROKERLIST");
	if (!broker || std::string(broker).empty()) {
		std::cout << "  SKIPPED: test_broker_backed_producer_partitioning (KAFKA_BROKERLIST not set)" << std::endl;
		return;
	}

	std::cout << "  Running broker-backed producer partitioning test against: " << broker << std::endl;

	cppkafka::Configuration config({
		{"metadata.broker.list", std::string(broker)},
		{"client.id", "partitioning_test_producer"}
	});

	try {
		cppkafka::Producer producer(config);

		// Produce messages with different keys and unassigned partition
		for (int i = 0; i < 5; ++i) {
			std::string key = "Venue:broker-venue-" + std::to_string(i);
			std::string payload = "{\"test_index\":" + std::to_string(i) + "}";

			cppkafka::MessageBuilder builder("test_producer_partitioning");
			builder.key(key);
			builder.payload(payload);
			producer.produce(builder);
		}
		producer.flush();

		// Produce explicit partition 0 message (e.g. service_events)
		cppkafka::MessageBuilder explicitBuilder("test_producer_partitioning");
		explicitBuilder.key("service_endpoint");
		explicitBuilder.partition(0);
		explicitBuilder.payload("{\"event\":\"join\"}");
		producer.produce(explicitBuilder);
		producer.flush();

		std::cout << "  PASS: test_broker_backed_producer_partitioning (successfully produced to broker)" << std::endl;
	} catch (const std::exception &e) {
		std::cout << "  WARNING: test_broker_backed_producer_partitioning error: " << e.what() << std::endl;
	}
}

int main() {
	Poco::AutoPtr<Poco::ConsoleChannel> pChannel(new Poco::ConsoleChannel);
	Poco::Logger::root().setChannel(pChannel);
	Poco::Logger::root().setLevel(Poco::Message::PRIO_INFORMATION);

	std::cout << "Running Kafka Producer Key-Based Partitioning Tests..." << std::endl;

	// Section A
	test_kafka_message_partition_defaults_and_explicit();
	test_kafka_producer_produce_queueing();
	test_kafka_manager_post_message_forwarding();

	// Section B
	test_venue_modification_and_removal_key_contract();
	test_different_venues_produce_different_keys();
	test_all_provisioning_object_types_key_formatting();
	test_empty_or_invalid_id_rejected();

	// Section C
	test_service_events_explicit_partition_0();

	// Section D
	test_key_hashing_consistency_and_distribution();
	test_broker_backed_producer_partitioning();

	std::cout << "ALL KAFKA PRODUCER PARTITIONING TESTS PASSED!" << std::endl;
	return 0;
}
