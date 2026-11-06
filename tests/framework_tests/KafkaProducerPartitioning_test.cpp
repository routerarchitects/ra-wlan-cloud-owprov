//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Poco/AutoPtr.h"
#include "Poco/Checksum.h"
#include "Poco/ConsoleChannel.h"
#include "Poco/JSON/Object.h"
#include "Poco/JSON/Parser.h"
#include "Poco/Logger.h"

#define private public
#define protected public
#include "framework/KafkaManager.h"
#include "framework/EventBusManager.h"
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

	static std::atomic<bool> g_MockRedisRefreshed = false;
	static std::atomic<bool> g_MockRedisDeleted = false;

	void MicroServiceRefreshRedisRegistration() {
		g_MockRedisRefreshed = true;
	}

	void MicroServiceDeleteRedisRegistration() {
		g_MockRedisDeleted = true;
	}

	std::uint64_t MicroServiceDaemonBusTimer() {
		return 50; // 50 milliseconds for fast loop execution in tests
	}

	std::string MicroServiceMakeSystemEventMessage(const char *Type) {
		Poco::JSON::Object Obj;
		Obj.set(KafkaTopics::ServiceEvents::Fields::EVENT, Type);
		Obj.set(KafkaTopics::ServiceEvents::Fields::ID, (Poco::Int64)g_MockSystemId);
		Obj.set(KafkaTopics::ServiceEvents::Fields::TYPE, "owprov");
		Obj.set(KafkaTopics::ServiceEvents::Fields::PUBLIC, g_MockPrivateEndPoint);
		Obj.set(KafkaTopics::ServiceEvents::Fields::PRIVATE, g_MockPrivateEndPoint);
		std::ostringstream OS;
		Obj.stringify(OS);
		return OS.str();
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

// Test A1: KafkaMessage partition defaults to KAFKA_PARTITION_DEFAULT (0) and respects explicit partition
static void test_kafka_message_partition_defaults_and_explicit() {
	KafkaMessage msgDefault("test_topic", "key1", "payload1");
	TEST_ASSERT(std::string(msgDefault.Topic()) == "test_topic", "Topic must match constructor argument");
	TEST_ASSERT(msgDefault.Key() == "key1", "Key must match constructor argument");
	TEST_ASSERT(msgDefault.Payload() == "payload1", "Payload must match constructor argument");
	TEST_ASSERT(msgDefault.Partition() == KAFKA_PARTITION_DEFAULT,
				"KafkaMessage default partition must be KAFKA_PARTITION_DEFAULT (0)");

	KafkaMessage msgUnassigned("test_topic", "key_u", "payload_u", KAFKA_PARTITION_UNASSIGNED);
	TEST_ASSERT(msgUnassigned.Partition() == KAFKA_PARTITION_UNASSIGNED,
				"KafkaMessage explicit unassigned partition must be KAFKA_PARTITION_UNASSIGNED (-1)");

	KafkaMessage msgExplicit0("service_events", "endpoint1", "join_payload", 0);
	TEST_ASSERT(msgExplicit0.Partition() == 0, "Explicit partition 0 must be preserved");

	KafkaMessage msgExplicit3("custom_topic", "key3", "payload3", 3);
	TEST_ASSERT(msgExplicit3.Partition() == 3, "Explicit partition 3 must be preserved");

	std::cout << "  PASS: test_kafka_message_partition_defaults_and_explicit" << std::endl;
}

// Test A2: KafkaProducer::Produce enqueues messages with correct partition
static void test_kafka_producer_produce_queueing() {
	KafkaProducer producer;

	// 1. Default partition (should default to KAFKA_PARTITION_DEFAULT = 0)
	producer.Produce("topic_default", "key_default", "payload_default");
	// 2. Explicit unassigned partition (-1)
	producer.Produce("topic_auto", "key_auto", "payload_auto", KAFKA_PARTITION_UNASSIGNED);
	// 3. Explicit partition 0
	producer.Produce("topic_part0", "key_part0", "payload_part0", 0);
	// 4. Explicit partition 2
	producer.Produce("topic_part2", "key_part2", "payload_part2", 2);

	// Drain and verify
	Poco::AutoPtr<Poco::Notification> note1(producer.Queue_.dequeueNotification());
	auto msg1 = dynamic_cast<KafkaMessage *>(note1.get());
	TEST_ASSERT(msg1 != nullptr, "Notification 1 must be KafkaMessage");
	TEST_ASSERT(msg1->Partition() == KAFKA_PARTITION_DEFAULT, "Produce with default partition must be KAFKA_PARTITION_DEFAULT (0)");
	TEST_ASSERT(msg1->Key() == "key_default", "Key must be key_default");

	Poco::AutoPtr<Poco::Notification> note2(producer.Queue_.dequeueNotification());
	auto msg2 = dynamic_cast<KafkaMessage *>(note2.get());
	TEST_ASSERT(msg2 != nullptr, "Notification 2 must be KafkaMessage");
	TEST_ASSERT(msg2->Partition() == KAFKA_PARTITION_UNASSIGNED, "Produce with KAFKA_PARTITION_UNASSIGNED must be -1");
	TEST_ASSERT(msg2->Key() == "key_auto", "Key must be key_auto");

	Poco::AutoPtr<Poco::Notification> note3(producer.Queue_.dequeueNotification());
	auto msg3 = dynamic_cast<KafkaMessage *>(note3.get());
	TEST_ASSERT(msg3 != nullptr, "Notification 3 must be KafkaMessage");
	TEST_ASSERT(msg3->Partition() == 0, "Produce with partition 0 must preserve partition 0");
	TEST_ASSERT(msg3->Key() == "key_part0", "Key must be key_part0");

	Poco::AutoPtr<Poco::Notification> note4(producer.Queue_.dequeueNotification());
	auto msg4 = dynamic_cast<KafkaMessage *>(note4.get());
	TEST_ASSERT(msg4 != nullptr, "Notification 4 must be KafkaMessage");
	TEST_ASSERT(msg4->Partition() == 2, "Produce with partition 2 must preserve partition 2");
	TEST_ASSERT(msg4->Key() == "key_part2", "Key must be key_part2");

	TEST_ASSERT(producer.Queue_.size() == 0, "Producer queue must be empty after draining");

	std::cout << "  PASS: test_kafka_producer_produce_queueing" << std::endl;
}

// Test A3: KafkaManager::PostMessage overloads forward partition and wrapping correctly
static void test_kafka_manager_post_message_forwarding() {
	KafkaManager()->KafkaEnabled_ = true;
	KafkaManager()->ProducerThr_.Queue_.clear();

	// String overload without wrap, default partition (should default to KAFKA_PARTITION_DEFAULT = 0)
	KafkaManager()->PostMessage("test_topic_str", "key_str", "raw_payload", false);

	// String overload without wrap, explicit unassigned partition
	KafkaManager()->PostMessage("test_topic_unassigned", "key_unassigned", "unassigned_payload", false, KAFKA_PARTITION_UNASSIGNED);

	// String overload with wrap and explicit partition 0
	KafkaManager()->PostMessage("service_events", "endpoint_bcast", "{\"event\":\"join\"}", true, 0);

	// JSON overload without wrap, default partition (should default to KAFKA_PARTITION_DEFAULT = 0)
	Poco::JSON::Object jsonNoWrap;
	jsonNoWrap.set("field", "value1");
	KafkaManager()->PostMessage("topic_json", "key_json", jsonNoWrap, false);

	// JSON overload without wrap, explicit unassigned partition
	Poco::JSON::Object jsonUnassigned;
	jsonUnassigned.set("field", "value_u");
	KafkaManager()->PostMessage("topic_json_u", "key_json_u", jsonUnassigned, false, KAFKA_PARTITION_UNASSIGNED);

	// JSON overload with wrap and explicit partition 1
	Poco::JSON::Object jsonWrap;
	jsonWrap.set("field", "value2");
	KafkaManager()->PostMessage("topic_json_wrap", "key_json_wrap", jsonWrap, true, 1);

	// Dequeue 1: raw string, default partition 0
	Poco::AutoPtr<Poco::Notification> n1(KafkaManager()->ProducerThr_.Queue_.dequeueNotification());
	auto m1 = dynamic_cast<KafkaMessage *>(n1.get());
	TEST_ASSERT(m1 != nullptr && m1->Partition() == KAFKA_PARTITION_DEFAULT,
				"Message 1 must have default partition 0");
	TEST_ASSERT(m1->Payload() == "raw_payload", "Message 1 payload must be unwrapped raw_payload");

	// Dequeue 2: raw string, explicit unassigned partition (-1)
	Poco::AutoPtr<Poco::Notification> n2(KafkaManager()->ProducerThr_.Queue_.dequeueNotification());
	auto m2 = dynamic_cast<KafkaMessage *>(n2.get());
	TEST_ASSERT(m2 != nullptr && m2->Partition() == KAFKA_PARTITION_UNASSIGNED,
				"Message 2 must have unassigned partition (-1)");
	TEST_ASSERT(m2->Payload() == "unassigned_payload", "Message 2 payload must be unassigned_payload");

	// Dequeue 3: wrapped string, partition 0
	Poco::AutoPtr<Poco::Notification> n3(KafkaManager()->ProducerThr_.Queue_.dequeueNotification());
	auto m3 = dynamic_cast<KafkaMessage *>(n3.get());
	TEST_ASSERT(m3 != nullptr && m3->Partition() == 0,
				"Message 3 must have explicit partition 0");
	TEST_ASSERT(m3->Payload().find("\"system\"") != std::string::npos,
				"Message 3 payload must be wrapped with system info");

	// Dequeue 4: json unwrapped, default partition 0
	Poco::AutoPtr<Poco::Notification> n4(KafkaManager()->ProducerThr_.Queue_.dequeueNotification());
	auto m4 = dynamic_cast<KafkaMessage *>(n4.get());
	TEST_ASSERT(m4 != nullptr && m4->Partition() == KAFKA_PARTITION_DEFAULT,
				"Message 4 must have default partition 0");
	TEST_ASSERT(m4->Payload().find("\"field\":\"value1\"") != std::string::npos,
				"Message 4 payload must contain JSON fields");

	// Dequeue 5: json unwrapped, explicit unassigned partition (-1)
	Poco::AutoPtr<Poco::Notification> n5(KafkaManager()->ProducerThr_.Queue_.dequeueNotification());
	auto m5 = dynamic_cast<KafkaMessage *>(n5.get());
	TEST_ASSERT(m5 != nullptr && m5->Partition() == KAFKA_PARTITION_UNASSIGNED,
				"Message 5 must have unassigned partition (-1)");
	TEST_ASSERT(m5->Payload().find("\"field\":\"value_u\"") != std::string::npos,
				"Message 5 payload must contain JSON fields");

	// Dequeue 6: json wrapped, partition 1
	Poco::AutoPtr<Poco::Notification> n6(KafkaManager()->ProducerThr_.Queue_.dequeueNotification());
	auto m6 = dynamic_cast<KafkaMessage *>(n6.get());
	TEST_ASSERT(m6 != nullptr && m6->Partition() == 1,
				"Message 6 must have explicit partition 1");
	TEST_ASSERT(m6->Payload().find("\"system\"") != std::string::npos,
				"Message 6 payload must be wrapped with system info");

	KafkaManager()->KafkaEnabled_ = false;
	KafkaManager()->ProducerThr_.Queue_.clear();

	std::cout << "  PASS: test_kafka_manager_post_message_forwarding" << std::endl;
}

// Test A4: Invalid negative partition values (< -1) are rejected and dropped
static void test_invalid_negative_partition_values_rejected() {
	KafkaProducer producer;

	// Valid partitions: -1 (unassigned) and >= 0
	producer.Produce("valid_topic", "key", "payload", KAFKA_PARTITION_UNASSIGNED);
	TEST_ASSERT(producer.Queue_.size() == 1, "KAFKA_PARTITION_UNASSIGNED must be accepted");

	producer.Produce("valid_topic", "key", "payload", 0);
	TEST_ASSERT(producer.Queue_.size() == 2, "Partition 0 must be accepted");

	producer.Produce("valid_topic", "key", "payload", 5);
	TEST_ASSERT(producer.Queue_.size() == 3, "Partition 5 must be accepted");

	// Invalid negative partitions (< -1) must be rejected and dropped
	producer.Produce("invalid_topic", "key", "payload", -2);
	TEST_ASSERT(producer.Queue_.size() == 3, "Partition -2 must be rejected and not enqueued");

	producer.Produce("invalid_topic", "key", "payload", -10);
	TEST_ASSERT(producer.Queue_.size() == 3, "Partition -10 must be rejected and not enqueued");

	producer.Produce("invalid_topic", "key", "payload", -999);
	TEST_ASSERT(producer.Queue_.size() == 3, "Partition -999 must be rejected and not enqueued");

	producer.Produce("invalid_topic", "key", "payload", INT32_MIN);
	TEST_ASSERT(producer.Queue_.size() == 3, "Partition INT32_MIN must be rejected and not enqueued");

	// Also verify KafkaManager::PostMessage overloads reject < -1
	KafkaManager()->KafkaEnabled_ = true;
	KafkaManager()->ProducerThr_.Queue_.clear();

	// String overload rejection
	KafkaManager()->PostMessage("test_topic", "key", "payload", false, -2);
	TEST_ASSERT(KafkaManager()->ProducerThr_.Queue_.size() == 0,
				"PostMessage(string) must reject partition -2");

	KafkaManager()->PostMessage("test_topic", "key", "payload", false, -50);
	TEST_ASSERT(KafkaManager()->ProducerThr_.Queue_.size() == 0,
				"PostMessage(string) must reject partition -50");

	// JSON overload rejection
	Poco::JSON::Object jsonMsg;
	jsonMsg.set("test", "val");
	KafkaManager()->PostMessage("test_topic", "key", jsonMsg, false, -2);
	TEST_ASSERT(KafkaManager()->ProducerThr_.Queue_.size() == 0,
				"PostMessage(JSON) must reject partition -2");

	// Valid partitions still accepted via PostMessage
	KafkaManager()->PostMessage("test_topic", "key", "payload", false, KAFKA_PARTITION_UNASSIGNED);
	TEST_ASSERT(KafkaManager()->ProducerThr_.Queue_.size() == 1,
				"PostMessage must accept KAFKA_PARTITION_UNASSIGNED");

	KafkaManager()->PostMessage("test_topic", "key", "payload", false, 0);
	TEST_ASSERT(KafkaManager()->ProducerThr_.Queue_.size() == 2,
				"PostMessage must accept explicit partition 0");

	KafkaManager()->KafkaEnabled_ = false;
	KafkaManager()->ProducerThr_.Queue_.clear();

	std::cout << "  PASS: test_invalid_negative_partition_values_rejected" << std::endl;
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

// Test C1: service_events routes explicitly to partition 0 via production EventBusManager
static void test_service_events_explicit_partition_0() {
	KafkaManager()->KafkaEnabled_ = true;
	KafkaManager()->ProducerThr_.Queue_.clear();
	g_MockRedisRefreshed = false;
	g_MockRedisDeleted = false;

	// 1. Start EventBusManager background worker thread which runs EventBusManager::run()
	EventBusManager()->Start();

	// Wait for EVENT_JOIN and at least one EVENT_KEEP_ALIVE to be enqueued
	auto startTime = std::chrono::steady_clock::now();
	while (KafkaManager()->ProducerThr_.Queue_.size() < 2 &&
		   std::chrono::steady_clock::now() - startTime < std::chrono::seconds(2)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	// 2. Stop EventBusManager, which causes loop exit and emits EVENT_LEAVE
	EventBusManager()->Stop();

	TEST_ASSERT(g_MockRedisRefreshed, "EventBusManager must refresh Redis registration");
	TEST_ASSERT(g_MockRedisDeleted, "EventBusManager must delete Redis registration upon stopping");

	size_t queueSize = KafkaManager()->ProducerThr_.Queue_.size();
	TEST_ASSERT(queueSize >= 3, "EventBusManager must have published at least JOIN, KEEP_ALIVE, and LEAVE (got "
				+ std::to_string(queueSize) + ")");

	std::vector<std::pair<std::string, std::string>> eventsSeen; // (event, payload)

	while (!KafkaManager()->ProducerThr_.Queue_.empty()) {
		Poco::AutoPtr<Poco::Notification> n(KafkaManager()->ProducerThr_.Queue_.dequeueNotification());
		auto m = dynamic_cast<KafkaMessage *>(n.get());
		TEST_ASSERT(m != nullptr, "Notification must be KafkaMessage");
		TEST_ASSERT(std::string(m->Topic()) == KafkaTopics::SERVICE_EVENTS,
					"Topic must be service_events");
		TEST_ASSERT(m->Key() == MicroServicePrivateEndPoint(),
					"Key must be MicroServicePrivateEndPoint()");

		// CRITICAL CONTRACT CHECK: partition MUST be explicitly 0
		TEST_ASSERT(m->Partition() == 0,
					"EventBusManager MUST explicitly route service_events to partition 0 (got: "
					+ std::to_string(m->Partition()) + ")");

		// Parse payload and record event type
		Poco::JSON::Parser parser;
		auto parsed = parser.parse(m->Payload()).extract<Poco::JSON::Object::Ptr>();
		TEST_ASSERT(parsed->has(KafkaTopics::ServiceEvents::Fields::EVENT), "service_events payload must contain 'event' field");
		std::string eventType = parsed->get(KafkaTopics::ServiceEvents::Fields::EVENT).toString();
		eventsSeen.emplace_back(eventType, m->Payload());
	}

	// Verify lifecycle sequence from production EventBusManager: JOIN first, LEAVE last
	TEST_ASSERT(eventsSeen.front().first == KafkaTopics::ServiceEvents::EVENT_JOIN,
				"First event from EventBusManager must be 'join'");
	TEST_ASSERT(eventsSeen.back().first == KafkaTopics::ServiceEvents::EVENT_LEAVE,
				"Final event from EventBusManager must be 'leave'");

	bool foundKeepAlive = false;
	for (size_t i = 1; i < eventsSeen.size() - 1; ++i) {
		if (eventsSeen[i].first == KafkaTopics::ServiceEvents::EVENT_KEEP_ALIVE) {
			foundKeepAlive = true;
			break;
		}
	}
	TEST_ASSERT(foundKeepAlive, "EventBusManager must have published 'keep-alive'");

	KafkaManager()->KafkaEnabled_ = false;
	KafkaManager()->ProducerThr_.Queue_.clear();

	std::cout << "  PASS: test_service_events_explicit_partition_0 (verified EventBusManager lifecycle: JOIN, KEEP_ALIVE, LEAVE all on partition 0)" << std::endl;
}

// =========================================================================
// SECTION D: Multi-Partition & Key Distribution Tests
// =========================================================================

// CRC32 hash simulator matching librdkafka's default 'consistent_random' partitioner
// (which uses CRC32 checksum for non-empty message keys to determine target partition)
static uint32_t librdkafka_crc32(const std::string &key) {
	Poco::Checksum checksum(Poco::Checksum::TYPE_CRC32);
	checksum.update(key);
	return checksum.checksum();
}

static int32_t compute_partition(const std::string &key, int32_t num_partitions) {
	uint32_t hash = librdkafka_crc32(key);
	return static_cast<int32_t>(hash % num_partitions);
}

// Test D1: Partition mapping consistency and distribution across partitions using librdkafka default (consistent_random CRC32)
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

	std::cout << "  PASS: test_key_hashing_consistency_and_distribution (consistent_random CRC32)" << std::endl;
}

// Test D2: Broker-backed multi-partition integration test (when broker and isolated test topic are available)
static void test_broker_backed_producer_partitioning() {
	const char *broker = std::getenv("KAFKA_BROKERLIST");
	if (!broker || std::string(broker).empty()) {
		std::cout << "  SKIPPED: test_broker_backed_producer_partitioning (KAFKA_BROKERLIST not set)" << std::endl;
		return;
	}

	const char *topicEnv = std::getenv("KAFKA_PARTITION_TEST_TOPIC");
	if (!topicEnv || std::string(topicEnv).empty()) {
		std::cout << "  SKIPPED: test_broker_backed_producer_partitioning (KAFKA_PARTITION_TEST_TOPIC not set - isolated test topic required)" << std::endl;
		return;
	}
	const std::string topic = topicEnv;

	std::cout << "  Running broker-backed producer partitioning test against: " << broker
			  << " on isolated test topic: " << topic << std::endl;

	try {
		// 1. Verify multi-partition topology on the broker using a consumer handle
		cppkafka::Configuration consumerConfig({
			{"metadata.broker.list", std::string(broker)},
			{"client.id", "partitioning_test_verifier_consumer"},
			{"group.id", "partitioning_verifier_" + std::to_string(std::time(nullptr))},
			{"enable.auto.commit", false},
			{"enable.partition.eof", false}
		});

		cppkafka::Consumer consumer(consumerConfig);
		auto topicMeta = consumer.get_metadata(consumer.get_topic(topic));
		const auto &partitions = topicMeta.get_partitions();

		// Issue C fix: Do not silently skip multi-partition check; topic MUST have >1 partition
		TEST_ASSERT(partitions.size() > 1,
					"Topic '" + topic + "' must have at least 2 partitions on broker for multi-partition testing (found: "
					+ std::to_string(partitions.size()) + ")");

		// Query current high watermark offsets for all partitions and assign consumer directly
		cppkafka::TopicPartitionList tpl;
		for (const auto &p : partitions) {
			auto offsets = consumer.query_offsets(cppkafka::TopicPartition(topic, p.get_id()));
			int64_t high = std::get<1>(offsets);
			tpl.emplace_back(topic, p.get_id(), high);
		}
		consumer.assign(tpl);

		// 2. Start the production OWPROV KafkaProducer worker thread
		KafkaManager()->KafkaEnabled_ = true;
		KafkaManager()->Producer().Start();
		TEST_ASSERT(KafkaManager()->Producer().IsRunning(), "Production KafkaProducer worker must be running");

		std::string token = std::to_string(std::time(nullptr)) + "_" + MicroServiceCreateUUID();

		// 3. Post production messages through KafkaManager()->PostMessage()
		// 3a. Same venue key: modification & removal (must land on identical partition in strict FIFO order)
		std::string sameVenueKey = "Venue:same-venue-" + token;
		std::string modPayload = "{\"operation\":\"modification\",\"token\":\"" + token + "\"}";
		std::string remPayload = "{\"operation\":\"removal\",\"token\":\"" + token + "\"}";
		KafkaManager()->PostMessage(topic.c_str(), sameVenueKey, modPayload, true, KAFKA_PARTITION_UNASSIGNED);
		KafkaManager()->PostMessage(topic.c_str(), sameVenueKey, remPayload, true, KAFKA_PARTITION_UNASSIGNED);

		// 3b. Explicit partition 0 (service_events contract)
		std::string svcKey = "https://owprov-internal:17005/" + token;
		std::string svcPayload = "{\"event\":\"join\",\"token\":\"" + token + "\"}";
		KafkaManager()->PostMessage(topic.c_str(), svcKey, svcPayload, false, 0);

		// 3c. Multiple distinct venue keys (must distribute across multiple partitions)
		const int numDistinct = 6;
		std::vector<std::string> distinctKeys;
		for (int i = 0; i < numDistinct; ++i) {
			std::string dKey = "Venue:distinct-" + token + "-" + std::to_string(i);
			distinctKeys.push_back(dKey);
			std::string dPayload = "{\"operation\":\"modification\",\"index\":" + std::to_string(i) + ",\"token\":\"" + token + "\"}";
			KafkaManager()->PostMessage(topic.c_str(), dKey, dPayload, true, KAFKA_PARTITION_UNASSIGNED);
		}

		const int totalExpectedMessages = 2 + 1 + numDistinct; // 9 messages

		// 4. Issue A fix: Drain the production queue before calling Stop()
		auto drainStart = std::chrono::steady_clock::now();
		while (KafkaManager()->Producer().Queue_.size() > 0 &&
			   std::chrono::steady_clock::now() - drainStart < std::chrono::seconds(5)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		TEST_ASSERT(KafkaManager()->Producer().Queue_.empty(),
					"Production KafkaProducer queue must drain completely before stopping");

		// Allow the worker thread to finish flushing all enqueued messages to the broker
		std::this_thread::sleep_for(std::chrono::milliseconds(500));

		KafkaManager()->Producer().Stop();
		KafkaManager()->KafkaEnabled_ = false;

		// 5. Issue B fix: Verify actual production messages consumed from the broker
		struct ConsumedRecord {
			std::string operation;
			int32_t partition;
			int64_t offset;
		};
		std::map<std::string, std::vector<ConsumedRecord>> recordsByKey;
		int received_count = 0;
		auto pollStart = std::chrono::steady_clock::now();

		while (received_count < totalExpectedMessages &&
			   std::chrono::steady_clock::now() - pollStart < std::chrono::seconds(10)) {
			cppkafka::Message msg = consumer.poll(std::chrono::milliseconds(200));
			if (!msg) continue;
			if (msg.get_error()) {
				if (msg.get_error().get_error() == RD_KAFKA_RESP_ERR__PARTITION_EOF) continue;
				TEST_ASSERT(false, "Broker error while consuming production messages: " + msg.get_error().to_string());
			}

			std::string payload = msg.get_payload() ? std::string(msg.get_payload()) : "";
			if (payload.find(token) == std::string::npos) {
				continue; // Message from a different run or external producer
			}

			std::string key = msg.get_key() ? std::string(msg.get_key()) : "";
			std::string op;
			if (payload.find("\"operation\":\"modification\"") != std::string::npos ||
				payload.find("\"operation\": \"modification\"") != std::string::npos) {
				op = "modification";
			} else if (payload.find("\"operation\":\"removal\"") != std::string::npos ||
					   payload.find("\"operation\": \"removal\"") != std::string::npos) {
				op = "removal";
			} else if (payload.find("\"event\":\"join\"") != std::string::npos ||
					   payload.find("\"event\": \"join\"") != std::string::npos) {
				op = "join";
			}

			recordsByKey[key].push_back({op, msg.get_partition(), msg.get_offset()});
			received_count++;
		}

		consumer.unsubscribe();
		consumer.unassign();

		// Assert all production messages were delivered to and retrieved from the broker
		TEST_ASSERT(received_count == totalExpectedMessages,
					"Expected " + std::to_string(totalExpectedMessages) +
					" production messages from broker, but received " + std::to_string(received_count));

		// Assert same-key affinity and chronological ordering on production messages
		const auto &sameVenueRecords = recordsByKey[sameVenueKey];
		TEST_ASSERT(sameVenueRecords.size() == 2,
					"Expected exactly 2 messages for sameVenueKey from production producer");

		const auto &modRecord = sameVenueRecords[0];
		const auto &remRecord = sameVenueRecords[1];

		// Verify chronological operation sequence
		TEST_ASSERT(modRecord.operation == "modification",
					"First consumed message for venue must be 'modification', got: " + modRecord.operation);
		TEST_ASSERT(remRecord.operation == "removal",
					"Second consumed message for venue must be 'removal', got: " + remRecord.operation);

		// Verify partition equality
		TEST_ASSERT(modRecord.partition == remRecord.partition,
					"Production producer: modification and removal for the same venue MUST land on the exact same partition (got: "
					+ std::to_string(modRecord.partition) + " and " + std::to_string(remRecord.partition) + ")");

		// Verify partition-local Kafka offset ordering (FIFO ordering contract)
		TEST_ASSERT(modRecord.offset < remRecord.offset,
					"Production producer: modification offset (" + std::to_string(modRecord.offset) +
					") must be strictly less than removal offset (" + std::to_string(remRecord.offset) +
					") to guarantee FIFO partition-local ordering");

		// Assert explicit partition 0 routing contract on production message
		TEST_ASSERT(!recordsByKey[svcKey].empty(),
					"Production producer: explicit partition 0 message must be received");
		TEST_ASSERT(recordsByKey[svcKey][0].partition == 0,
					"Production producer: explicit partition 0 message must land on partition 0 (got: "
					+ std::to_string(recordsByKey[svcKey][0].partition) + ")");

		// Assert cross-partition distribution of distinct keys on production messages
		std::set<int32_t> distinct_parts;
		for (const auto &dKey : distinctKeys) {
			TEST_ASSERT(!recordsByKey[dKey].empty(),
						"Production message for key " + dKey + " must be received");
			distinct_parts.insert(recordsByKey[dKey][0].partition);
		}
		TEST_ASSERT(distinct_parts.size() > 1,
					"Production producer: distinct venue keys must distribute across multiple partitions on broker (spanned "
					+ std::to_string(distinct_parts.size()) + " of " + std::to_string(partitions.size()) + " partitions)");

		std::cout << "  PASS: test_broker_backed_producer_partitioning (verified "
				  << received_count << " production messages: FIFO offset order preserved across "
				  << distinct_parts.size() << " broker partitions)" << std::endl;
	} catch (const std::exception &e) {
		TEST_ASSERT(false, std::string("Broker-backed producer partitioning test failed: ") + e.what());
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
	test_invalid_negative_partition_values_rejected();

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
