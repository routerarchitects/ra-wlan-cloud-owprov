//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//

#include <any>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "fmt/core.h"
#include "nlohmann/json.hpp"
#include "cppkafka/cppkafka.h"

#include "Poco/DateTimeFormatter.h"
#include "Poco/JSON/Object.h"
#include "Poco/JSON/Parser.h"
#include "Poco/Logger.h"
#include "Poco/Net/HTTPRequestHandler.h"
#include "Poco/Net/HTTPServerResponse.h"
#include "framework/KafkaTopics.h"
#include "framework/OpenAPIRequests.h"
#include "framework/OpenWifiTypes.h"
#include "framework/RESTAPI_GenericServerAccounting.h"
#include "framework/RESTAPI_Handler.h"
#include "framework/utils.h"
#include "RESTObjects/RESTAPI_SecurityObjects.h"

#define private public
#define protected public
#include "framework/MicroService.h"
#undef private
#undef protected
#include "framework/OpenWifiTypes.h"
#include "framework/RESTAPI_GenericServerAccounting.h"
#include "framework/RESTAPI_Handler.h"
#include "framework/utils.h"
#include "RESTObjects/RESTAPI_SecurityObjects.h"

#define TEST_ASSERT(cond, msg)                                                                   \
	do {                                                                                         \
		if (!(cond)) {                                                                           \
			std::cerr << "TEST FAILURE [" << __FILE__ << ":" << __LINE__ << "]: " << msg        \
					  << std::endl;                                                              \
			std::exit(1);                                                                        \
		}                                                                                        \
	} while (0)

namespace OpenWifi {

	// Stubs for daemon and REST subsystem hooks that are referenced by MicroService
	// but unused in service discovery unit tests.
	void DaemonPostInitialization([[maybe_unused]] Poco::Util::Application &app) {}

	Poco::Net::HTTPRequestHandler *RESTAPI_IntRouter(
		[[maybe_unused]] const std::string &Path,
		[[maybe_unused]] RESTAPIHandler::BindingMap &Bindings,
		[[maybe_unused]] Poco::Logger &L,
		[[maybe_unused]] RESTAPI_GenericServerAccounting &S,
		[[maybe_unused]] uint64_t Id) {
		return nullptr;
	}

	Poco::Net::HTTPRequestHandler *RESTAPI_ExtRouter(
		[[maybe_unused]] const std::string &Path,
		[[maybe_unused]] RESTAPIHandler::BindingMap &Bindings,
		[[maybe_unused]] Poco::Logger &L,
		[[maybe_unused]] RESTAPI_GenericServerAccounting &S,
		[[maybe_unused]] uint64_t Id) {
		return nullptr;
	}

	Poco::Net::HTTPServerResponse::HTTPStatus
	OpenAPIRequestGet::Do([[maybe_unused]] Poco::JSON::Object::Ptr &ResponseObject,
						  [[maybe_unused]] const std::string &BearerToken) {
		return Poco::Net::HTTPServerResponse::HTTP_OK;
	}

	namespace SecurityObjects {
		bool UserInfoAndPolicy::from_json([[maybe_unused]] const Poco::JSON::Object::Ptr &obj) {
			return true;
		}
	} // namespace SecurityObjects

	// Test harness class exposing protected/internal state for verification
	class TestMicroService : public MicroService {
	  public:
		TestMicroService(const std::string &AppName = "owprov")
			: MicroService("owprov.properties", "OWPROV_ROOT", "OWPROV_CONFIG", AppName, 10, {}) {
			InitializeInstanceIdentity("owprov-1");
		}

		Types::MicroServiceInstanceMap GetServiceInstances(const std::string &Type) {
			std::lock_guard G(InfraMutex_);
			auto it = Services_.find(Poco::toLower(Type));
			return (it != Services_.end()) ? it->second : Types::MicroServiceInstanceMap{};
		}

		Types::MicroServiceTypeMap GetServiceRegistry() {
			std::lock_guard G(InfraMutex_);
			return Services_;
		}

		void SetInstanceLastUpdateForTest(const std::string &Type, const std::string &InstId, uint64_t ts) {
			std::lock_guard G(InfraMutex_);
			auto it = Services_.find(Poco::toLower(Type));
			if (it != Services_.end()) {
				auto itInst = it->second.find(InstId);
				if (itInst != it->second.end()) {
					itInst->second.LastUpdate = ts;
				}
			}
		}
	};

} // namespace OpenWifi

using namespace OpenWifi;

// Helper to construct a JSON service_events payload
static std::string MakePayload(const std::string &event,
                               const std::string &id,
                               const std::string &type,
                               const std::string &privateEndpoint,
                               const std::string &publicEndpoint,
                               const std::string &key = "test-key-hash",
                               const std::string &version = "1.0.0") {
	Poco::JSON::Object obj;
	obj.set(KafkaTopics::ServiceEvents::Fields::EVENT, event);
	obj.set(KafkaTopics::ServiceEvents::Fields::ID, id);
	obj.set("instanceId", id);
	obj.set(KafkaTopics::ServiceEvents::Fields::TYPE, type);
	obj.set(KafkaTopics::ServiceEvents::Fields::PRIVATE, privateEndpoint);
	obj.set(KafkaTopics::ServiceEvents::Fields::PUBLIC, publicEndpoint);
	obj.set(KafkaTopics::ServiceEvents::Fields::KEY, key);
	obj.set(KafkaTopics::ServiceEvents::Fields::VRSN, version);
	std::stringstream ss;
	Poco::JSON::Stringifier::stringify(obj, ss);
	return ss.str();
}

// Test 1: Two instances sharing the same private endpoint register independently
//         and GetServices("owgw") returns exactly 1 deduplicated logical endpoint.
static void test_multi_instance_same_endpoint_registration() {
	TestMicroService svc;

	std::string instA = "owgw-1-11111111-1111-4111-8111-111111111111";
	std::string instB = "owgw-2-22222222-2222-4222-8222-222222222222";
	std::string sharedEndpoint = "https://owgw-internal:17004";

	svc.BusMessageReceived("service_events",
		MakePayload("join", instA, "owgw", sharedEndpoint, "https://owgw.example.com:16004"));
	svc.BusMessageReceived("service_events",
		MakePayload("join", instB, "owgw", sharedEndpoint, "https://owgw.example.com:16004"));

	// Registry must track both instances
	auto instances = svc.GetServiceInstances("owgw");
	TEST_ASSERT(instances.size() == 2, "Registry must contain 2 instances for owgw");
	TEST_ASSERT(instances.find(instA) != instances.end(), "Instance A must be present in registry");
	TEST_ASSERT(instances.find(instB) != instances.end(), "Instance B must be present in registry");

	// But client endpoint resolution must deduplicate: return exactly 1 logical route
	auto endpoints = svc.GetServices("owgw");
	TEST_ASSERT(endpoints.size() == 1, "GetServices(owgw) must return exactly 1 deduplicated endpoint");
	TEST_ASSERT(endpoints[0].PrivateEndPoint == sharedEndpoint, "Resolved endpoint matches shared private URI");

	std::cout << "  PASS: test_multi_instance_same_endpoint_registration" << std::endl;
}

// Test 2: Idempotent JOIN and KEEP_ALIVE updates do not inflate replica count
static void test_idempotent_join_and_keepalive() {
	TestMicroService svc;

	std::string instA = "owgw-1-11111111-1111-4111-8111-111111111111";
	std::string sharedEndpoint = "https://owgw-internal:17004";

	// Repeated JOIN
	svc.BusMessageReceived("service_events",
		MakePayload("join", instA, "owgw", sharedEndpoint, "https://owgw.example.com:16004"));
	svc.BusMessageReceived("service_events",
		MakePayload("join", instA, "owgw", sharedEndpoint, "https://owgw.example.com:16004"));

	auto instances = svc.GetServiceInstances("owgw");
	TEST_ASSERT(instances.size() == 1, "Duplicate JOIN must not create duplicate entries");

	// KEEP_ALIVE updates timestamp idempotently
	svc.BusMessageReceived("service_events",
		MakePayload("keep-alive", instA, "owgw", sharedEndpoint, "https://owgw.example.com:16004"));

	instances = svc.GetServiceInstances("owgw");
	TEST_ASSERT(instances.size() == 1, "KEEP_ALIVE must preserve single instance entry");

	std::cout << "  PASS: test_idempotent_join_and_keepalive" << std::endl;
}

// Test 3: Independent LEAVE does NOT drop the shared service endpoint while another replica remains
static void test_independent_leave_preserves_routing() {
	TestMicroService svc;

	std::string instA = "owgw-1-11111111-1111-4111-8111-111111111111";
	std::string instB = "owgw-2-22222222-2222-4222-8222-222222222222";
	std::string sharedEndpoint = "https://owgw-internal:17004";

	svc.BusMessageReceived("service_events",
		MakePayload("join", instA, "owgw", sharedEndpoint, "https://owgw.example.com:16004"));
	svc.BusMessageReceived("service_events",
		MakePayload("join", instB, "owgw", sharedEndpoint, "https://owgw.example.com:16004"));

	// Instance A leaves (graceful restart)
	svc.BusMessageReceived("service_events",
		MakePayload("leave", instA, "owgw", sharedEndpoint, "https://owgw.example.com:16004"));

	// Instance A is removed, but Instance B remains
	auto instances = svc.GetServiceInstances("owgw");
	TEST_ASSERT(instances.size() == 1, "One instance must remain registered");
	TEST_ASSERT(instances.find(instA) == instances.end(), "Instance A must be erased");
	TEST_ASSERT(instances.find(instB) != instances.end(), "Instance B must still be present");

	// Critical check: Service routing is NOT dropped!
	auto endpoints = svc.GetServices("owgw");
	TEST_ASSERT(endpoints.size() == 1, "GetServices(owgw) must still return the active route");
	TEST_ASSERT(endpoints[0].PrivateEndPoint == sharedEndpoint, "Routable private URI preserved");

	std::cout << "  PASS: test_independent_leave_preserves_routing" << std::endl;
}

// Test 4: All instances leaving cleanly removes the logical route
static void test_all_instances_leave_removes_route() {
	TestMicroService svc;

	std::string instA = "owsec-1-aaaaaaaa-1111-4111-8111-111111111111";
	std::string endpoint = "https://owsec-internal:16001";

	svc.BusMessageReceived("service_events",
		MakePayload("join", instA, "owsec", endpoint, "https://owsec.example.com:16001"));

	TEST_ASSERT(!svc.GetServices("owsec").empty(), "owsec must be routable after JOIN");

	// Instance A leaves
	svc.BusMessageReceived("service_events",
		MakePayload("leave", instA, "owsec", endpoint, "https://owsec.example.com:16001"));

	TEST_ASSERT(svc.GetServiceInstances("owsec").empty(), "No instances remain for owsec");
	TEST_ASSERT(svc.GetServices("owsec").empty(), "Logical route for owsec must be removed when all instances leave");

	std::cout << "  PASS: test_all_instances_leave_removes_route" << std::endl;
}

// Test 5: Rolling restart overlap: old and replacement instances run concurrently
static void test_rolling_restart_overlap() {
	TestMicroService svc;

	std::string oldInst = "owprov-1-aaaaaaaa-0000-4000-8000-000000000001";
	std::string newInst = "owprov-1-bbbbbbbb-0000-4000-8000-000000000002";
	std::string sharedEndpoint = "https://owprov-internal:17005";

	// Old container is running
	svc.BusMessageReceived("service_events",
		MakePayload("join", oldInst, "owprov", sharedEndpoint, "https://owprov.example.com:16005"));

	// Replacement container starts up before old container finishes draining
	svc.BusMessageReceived("service_events",
		MakePayload("join", newInst, "owprov", sharedEndpoint, "https://owprov.example.com:16005"));

	// Both coexist during overlap
	TEST_ASSERT(svc.GetServiceInstances("owprov").size() == 2, "Both replicas present during rolling restart overlap");
	TEST_ASSERT(svc.GetServices("owprov").size() == 1, "Logical route remains single load-balanced endpoint");

	// Old container terminates and sends LEAVE
	svc.BusMessageReceived("service_events",
		MakePayload("leave", oldInst, "owprov", sharedEndpoint, "https://owprov.example.com:16005"));

	// New container continues serving without gap
	auto instances = svc.GetServiceInstances("owprov");
	TEST_ASSERT(instances.size() == 1, "Only new replica remains");
	TEST_ASSERT(instances.find(newInst) != instances.end(), "New replica is the active instance");
	TEST_ASSERT(svc.GetServices("owprov").size() == 1, "Route never blinked during rolling restart");

	std::cout << "  PASS: test_rolling_restart_overlap" << std::endl;
}

// Test 6: Self-event detection ignores our own broadcast
static void test_self_event_ignored() {
	TestMicroService svc;

	std::string myId = svc.InstanceId();
	std::string endpoint = "https://owprov-internal:17005";

	// Self-broadcast with matching instance ID
	svc.BusMessageReceived("service_events",
		MakePayload("join", myId, "owprov", endpoint, "https://owprov.example.com:16005"));

	// Should NOT be added to Services_ (ignored as self-event)
	auto instances = svc.GetServiceInstances("owprov");
	TEST_ASSERT(instances.find(myId) == instances.end(), "Self-broadcast must be ignored");

	std::cout << "  PASS: test_self_event_ignored" << std::endl;
}

// Test 7: Legacy peer compatibility (numeric ID in service_events)
static void test_legacy_peer_compatibility() {
	TestMicroService svc;

	// Legacy service emitting numeric id: 123
	Poco::JSON::Object obj;
	obj.set(KafkaTopics::ServiceEvents::Fields::EVENT, "join");
	obj.set(KafkaTopics::ServiceEvents::Fields::ID, (uint64_t)123);
	obj.set(KafkaTopics::ServiceEvents::Fields::TYPE, "owsec");
	obj.set(KafkaTopics::ServiceEvents::Fields::PRIVATE, "https://owsec-internal:16001");
	obj.set(KafkaTopics::ServiceEvents::Fields::PUBLIC, "https://owsec.example.com:16001");
	obj.set(KafkaTopics::ServiceEvents::Fields::KEY, "legacy-key");
	obj.set(KafkaTopics::ServiceEvents::Fields::VRSN, "0.9.0");
	std::stringstream ss;
	Poco::JSON::Stringifier::stringify(obj, ss);

	svc.BusMessageReceived("service_events", ss.str());

	auto instances = svc.GetServiceInstances("owsec");
	TEST_ASSERT(instances.size() == 1, "Legacy service must be registered");
	TEST_ASSERT(instances.find("123@https://owsec-internal:16001") != instances.end(), "Legacy numeric ID and endpoint parsed as fallback instance key");

	auto routes = svc.GetServices("owsec");
	TEST_ASSERT(routes.size() == 1, "Legacy service endpoint is discoverable");
	TEST_ASSERT(routes[0].PrivateEndPoint == "https://owsec-internal:16001", "Private endpoint matches");

	std::cout << "  PASS: test_legacy_peer_compatibility" << std::endl;
}

// Test 8: GetPrivateEndPointServiceKey looks up metadata across the two-level map
static void test_get_private_endpoint_service_key() {
	TestMicroService svc;

	std::string inst = "owfms-1-11111111-2222-3333-4444-555555555555";
	std::string endpoint = "https://owfms-internal:16004";
	std::string accessKey = "secret-access-key-hash";

	svc.BusMessageReceived("service_events",
		MakePayload("join", inst, "owfms", endpoint, "https://owfms.example.com:16004", accessKey));

	auto optMeta = svc.GetPrivateEndPointServiceKey(endpoint);
	TEST_ASSERT(optMeta.has_value(), "Metadata must be found for private endpoint");
	TEST_ASSERT(optMeta->AccessKey == accessKey, "Access key matches");
	TEST_ASSERT(optMeta->Type == "owfms", "Type matches");

	auto missing = svc.GetPrivateEndPointServiceKey("https://nonexistent:9999");
	TEST_ASSERT(!missing.has_value(), "Missing endpoint returns nullopt");

	std::cout << "  PASS: test_get_private_endpoint_service_key" << std::endl;
}

// Test 9: MakeSystemEventMessage publishes numeric ID for legacy peers and instanceId for multi-instance peers
static void test_make_system_event_message_canonical_identity() {
	TestMicroService svc;

	std::string joinMsg = svc.MakeSystemEventMessage("join");
	Poco::JSON::Parser parser;
	auto obj = parser.parse(joinMsg).extract<Poco::JSON::Object::Ptr>();

	TEST_ASSERT(obj->has(KafkaTopics::ServiceEvents::Fields::ID), "Message must have id field");
	TEST_ASSERT(obj->get(KafkaTopics::ServiceEvents::Fields::ID).isInteger(), "id field must carry numeric ID for legacy peer microservices");

	TEST_ASSERT(obj->has("instanceId"), "Message must have instanceId field");
	std::string instVal = obj->get("instanceId").toString();
	TEST_ASSERT(instVal == svc.InstanceId(), "instanceId field must carry canonical runtime incarnation ID");
	TEST_ASSERT(instVal.rfind("owprov-1-", 0) == 0, "instanceId is slot-prefixed composite identity");

	// Self-event check using the exact message produced by MakeSystemEventMessage
	svc.BusMessageReceived("service_events", joinMsg);
	auto instances = svc.GetServiceInstances("owprov");
	TEST_ASSERT(instances.find(svc.InstanceId()) == instances.end(),
		"MicroService must suppress its own MakeSystemEventMessage broadcast");

	std::cout << "  PASS: test_make_system_event_message_canonical_identity" << std::endl;
}

// Test 10: Partial KEEP_ALIVE preserves valid credentials; incomplete new registration is rejected
static void test_partial_keepalive_preserves_credentials() {
	TestMicroService svc;

	std::string inst = "owsec-1-auth-test";
	std::string endpoint = "https://owsec-internal:16001";
	std::string validKey = "secret-token-hash-xyz";

	// 1. Initial valid JOIN
	svc.BusMessageReceived("service_events",
		MakePayload("join", inst, "owsec", endpoint, "https://owsec.example.com:16001", validKey, "1.2.3"));

	auto instances = svc.GetServiceInstances("owsec");
	TEST_ASSERT(instances.size() == 1, "owsec registered");
	TEST_ASSERT(instances[inst].AccessKey == validKey, "Initial AccessKey saved");
	TEST_ASSERT(instances[inst].PublicEndPoint == "https://owsec.example.com:16001", "Initial PublicEndPoint saved");

	// 2. Subsequent partial KEEP_ALIVE missing KEY and PUBLIC
	Poco::JSON::Object partialKA;
	partialKA.set(KafkaTopics::ServiceEvents::Fields::EVENT, "keep-alive");
	partialKA.set(KafkaTopics::ServiceEvents::Fields::ID, inst);
	partialKA.set("instanceId", inst);
	partialKA.set(KafkaTopics::ServiceEvents::Fields::TYPE, "owsec");
	partialKA.set(KafkaTopics::ServiceEvents::Fields::PRIVATE, endpoint);
	std::stringstream ss;
	Poco::JSON::Stringifier::stringify(partialKA, ss);

	svc.BusMessageReceived("service_events", ss.str());

	// Critical check: AccessKey, PublicEndPoint, Version must NOT be cleared by partial KEEP_ALIVE
	instances = svc.GetServiceInstances("owsec");
	TEST_ASSERT(instances[inst].AccessKey == validKey, "AccessKey preserved across partial KEEP_ALIVE");
	TEST_ASSERT(instances[inst].PublicEndPoint == "https://owsec.example.com:16001", "PublicEndPoint preserved");
	TEST_ASSERT(instances[inst].Version == "1.2.3", "Version preserved");

	auto routes = svc.GetServices("owsec");
	TEST_ASSERT(routes.size() == 1, "GetServices returns route");
	TEST_ASSERT(routes[0].AccessKey == validKey, "Resolved endpoint retains valid AccessKey for REST clients");

	// 3. New registration with missing credentials must be rejected
	Poco::JSON::Object badNewJoin;
	badNewJoin.set(KafkaTopics::ServiceEvents::Fields::EVENT, "join");
	badNewJoin.set(KafkaTopics::ServiceEvents::Fields::ID, "unseen-bad-inst");
	badNewJoin.set("instanceId", "unseen-bad-inst");
	badNewJoin.set(KafkaTopics::ServiceEvents::Fields::TYPE, "owfms");
	badNewJoin.set(KafkaTopics::ServiceEvents::Fields::PRIVATE, "https://owfms-internal:16004");
	// missing KEY and PUBLIC
	std::stringstream ssBad;
	Poco::JSON::Stringifier::stringify(badNewJoin, ssBad);

	svc.BusMessageReceived("service_events", ssBad.str());
	TEST_ASSERT(svc.GetServiceInstances("owfms").empty(), "Incomplete new registration rejected without valid credentials");

	std::cout << "  PASS: test_partial_keepalive_preserves_credentials" << std::endl;
}

// Test 11: Empty or missing instance ID in LEAVE/JOIN is rejected without affecting shared endpoints
static void test_empty_instance_id_rejected_preserves_replicas() {
	TestMicroService svc;

	std::string instA = "owgw-1-11111111-1111-4111-8111-111111111111";
	std::string instB = "owgw-2-22222222-2222-4222-8222-222222222222";
	std::string sharedEndpoint = "https://owgw-internal:17004";

	svc.BusMessageReceived("service_events",
		MakePayload("join", instA, "owgw", sharedEndpoint, "https://owgw.example.com:16004"));
	svc.BusMessageReceived("service_events",
		MakePayload("join", instB, "owgw", sharedEndpoint, "https://owgw.example.com:16004"));

	TEST_ASSERT(svc.GetServiceInstances("owgw").size() == 2, "Both instances initially registered");

	// Malformed LEAVE event with empty instanceId and empty id
	Poco::JSON::Object badLeave;
	badLeave.set(KafkaTopics::ServiceEvents::Fields::EVENT, "leave");
	badLeave.set(KafkaTopics::ServiceEvents::Fields::ID, "");
	badLeave.set("instanceId", "");
	badLeave.set(KafkaTopics::ServiceEvents::Fields::TYPE, "owgw");
	badLeave.set(KafkaTopics::ServiceEvents::Fields::PRIVATE, sharedEndpoint);
	badLeave.set(KafkaTopics::ServiceEvents::Fields::PUBLIC, "https://owgw.example.com:16004");
	badLeave.set(KafkaTopics::ServiceEvents::Fields::KEY, "key");
	badLeave.set(KafkaTopics::ServiceEvents::Fields::VRSN, "1.0.0");
	std::stringstream ss;
	Poco::JSON::Stringifier::stringify(badLeave, ss);

	svc.BusMessageReceived("service_events", ss.str());

	// CRITICAL: Empty instanceId must NOT wipe out instances sharing the private endpoint!
	auto instances = svc.GetServiceInstances("owgw");
	TEST_ASSERT(instances.size() == 2, "Malformed LEAVE with empty instanceId must be rejected; replicas preserved");
	TEST_ASSERT(instances.find(instA) != instances.end(), "Instance A remains active");
	TEST_ASSERT(instances.find(instB) != instances.end(), "Instance B remains active");
	TEST_ASSERT(svc.GetServices("owgw").size() == 1, "Logical route still intact");

	// Malformed JOIN event with empty instanceId
	Poco::JSON::Object badJoin;
	badJoin.set(KafkaTopics::ServiceEvents::Fields::EVENT, "join");
	badJoin.set(KafkaTopics::ServiceEvents::Fields::ID, "");
	badJoin.set("instanceId", "");
	badJoin.set(KafkaTopics::ServiceEvents::Fields::TYPE, "owsec");
	badJoin.set(KafkaTopics::ServiceEvents::Fields::PRIVATE, "https://owsec-internal:16001");
	badJoin.set(KafkaTopics::ServiceEvents::Fields::PUBLIC, "https://owsec.example.com:16001");
	badJoin.set(KafkaTopics::ServiceEvents::Fields::KEY, "key");
	badJoin.set(KafkaTopics::ServiceEvents::Fields::VRSN, "1.0.0");
	std::stringstream ss2;
	Poco::JSON::Stringifier::stringify(badJoin, ss2);

	svc.BusMessageReceived("service_events", ss2.str());
	TEST_ASSERT(svc.GetServiceInstances("owsec").empty(), "Malformed JOIN with empty instanceId must be rejected");

	std::cout << "  PASS: test_empty_instance_id_rejected_preserves_replicas" << std::endl;
}

// Test 12: UUID string in ID field does not cause exceptions during parsing
static void test_uuid_id_and_numeric_id_no_exceptions() {
	TestMicroService svc;

	// Message with UUID string in both id and instanceId
	std::string uuidInst = "owanalytics-1-33333333-4444-5555-6666-777777777777";
	Poco::JSON::Object obj;
	obj.set(KafkaTopics::ServiceEvents::Fields::EVENT, "join");
	obj.set(KafkaTopics::ServiceEvents::Fields::ID, uuidInst);
	obj.set("instanceId", uuidInst);
	obj.set(KafkaTopics::ServiceEvents::Fields::TYPE, "owanalytics");
	obj.set(KafkaTopics::ServiceEvents::Fields::PRIVATE, "https://owanalytics-internal:16005");
	obj.set(KafkaTopics::ServiceEvents::Fields::PUBLIC, "https://owanalytics.example.com:16005");
	obj.set(KafkaTopics::ServiceEvents::Fields::KEY, "key");
	obj.set(KafkaTopics::ServiceEvents::Fields::VRSN, "1.0.0");
	std::stringstream ss;
	Poco::JSON::Stringifier::stringify(obj, ss);

	// Must process without throwing any exceptions
	bool threw = false;
	try {
		svc.BusMessageReceived("service_events", ss.str());
	} catch (...) {
		threw = true;
	}
	TEST_ASSERT(!threw, "BusMessageReceived must not throw on UUID string in id field");

	auto instances = svc.GetServiceInstances("owanalytics");
	TEST_ASSERT(instances.size() == 1, "Instance registered from UUID message");
	TEST_ASSERT(instances.find(uuidInst) != instances.end(), "Instance matches UUID key");

	std::cout << "  PASS: test_uuid_id_and_numeric_id_no_exceptions" << std::endl;
}

// Test 13: Stale instances older than 120s are pruned during normal event processing
static void test_zombie_sweep_pruning() {
	TestMicroService svc;

	std::string instActive = "owfms-1-active";
	std::string instStale = "owfms-2-stale";
	std::string endpoint = "https://owfms-internal:16004";

	svc.BusMessageReceived("service_events",
		MakePayload("join", instActive, "owfms", endpoint, "https://owfms.example.com:16004"));
	svc.BusMessageReceived("service_events",
		MakePayload("join", instStale, "owfms", endpoint, "https://owfms.example.com:16004"));

	TEST_ASSERT(svc.GetServiceInstances("owfms").size() == 2, "2 instances registered");

	// Mark instStale as updated 130 seconds ago
	svc.SetInstanceLastUpdateForTest("owfms", instStale, Utils::Now() - 130);

	// Event-driven stale cleanup executes on received event and prunes stale instance
	svc.BusMessageReceived("service_events",
		MakePayload("keep-alive", instActive, "owfms", endpoint, "https://owfms.example.com:16004"));

	TEST_ASSERT(svc.GetServiceInstances("owfms").size() == 1, "Stale instance pruned after 120s timeout");
	TEST_ASSERT(svc.GetServiceInstances("owfms").find(instActive) != svc.GetServiceInstances("owfms").end(), "Active instance retained");
	TEST_ASSERT(!svc.GetServices("owfms").empty(), "Route active for surviving instance");

	// Self-event also triggers stale cleanup
	std::string instStale2 = "owfms-3-stale";
	svc.BusMessageReceived("service_events",
		MakePayload("join", instStale2, "owfms", endpoint, "https://owfms.example.com:16004"));
	TEST_ASSERT(svc.GetServiceInstances("owfms").size() == 2, "instActive and instStale2 registered");
	svc.SetInstanceLastUpdateForTest("owfms", instStale2, Utils::Now() - 130);

	std::string selfMsg = svc.MakeSystemEventMessage("keep-alive");
	svc.BusMessageReceived("service_events", selfMsg);
	TEST_ASSERT(svc.GetServiceInstances("owfms").size() == 1, "Self-event triggers stale cleanup; instStale2 pruned");

	std::cout << "  PASS: test_zombie_sweep_pruning" << std::endl;
}

// Test 14: Empty or invalid endpoint URIs are rejected without creating empty registry groups
static void test_empty_or_invalid_endpoint_uri_rejected_no_empty_group() {
	TestMicroService svc;

	// 1. JOIN with empty private endpoint
	Poco::JSON::Object badPrivate;
	badPrivate.set(KafkaTopics::ServiceEvents::Fields::EVENT, "join");
	badPrivate.set(KafkaTopics::ServiceEvents::Fields::ID, "bad-inst-1");
	badPrivate.set(KafkaTopics::ServiceEvents::Fields::TYPE, "owsubsystem1");
	badPrivate.set(KafkaTopics::ServiceEvents::Fields::PRIVATE, "");
	badPrivate.set(KafkaTopics::ServiceEvents::Fields::PUBLIC, "https://owsubsystem1.example.com:16001");
	badPrivate.set(KafkaTopics::ServiceEvents::Fields::KEY, "valid-key");
	badPrivate.set(KafkaTopics::ServiceEvents::Fields::VRSN, "1.0.0");
	std::stringstream ss1;
	Poco::JSON::Stringifier::stringify(badPrivate, ss1);

	svc.BusMessageReceived("service_events", ss1.str());

	// Verify rejected and NO empty group created in Services_
	auto reg = svc.GetServiceRegistry();
	TEST_ASSERT(reg.find("owsubsystem1") == reg.end(), "Empty private endpoint must be rejected; no empty group created");
	TEST_ASSERT(svc.GetServices("owsubsystem1").empty(), "No routes for rejected service");

	// 2. JOIN with invalid public endpoint URI
	Poco::JSON::Object badPublic;
	badPublic.set(KafkaTopics::ServiceEvents::Fields::EVENT, "join");
	badPublic.set(KafkaTopics::ServiceEvents::Fields::ID, "bad-inst-3");
	badPublic.set(KafkaTopics::ServiceEvents::Fields::TYPE, "owsubsystem3");
	badPublic.set(KafkaTopics::ServiceEvents::Fields::PRIVATE, "https://owsubsystem3-internal:16003");
	badPublic.set(KafkaTopics::ServiceEvents::Fields::PUBLIC, "");
	badPublic.set(KafkaTopics::ServiceEvents::Fields::KEY, "valid-key");
	badPublic.set(KafkaTopics::ServiceEvents::Fields::VRSN, "1.0.0");
	std::stringstream ss3;
	Poco::JSON::Stringifier::stringify(badPublic, ss3);

	svc.BusMessageReceived("service_events", ss3.str());

	reg = svc.GetServiceRegistry();
	TEST_ASSERT(reg.find("owsubsystem3") == reg.end(), "Empty public endpoint must be rejected; no empty group created");

	std::cout << "  PASS: test_empty_or_invalid_endpoint_uri_rejected_no_empty_group" << std::endl;
}

// Test 15: Clean LEAVE removal and re-join lifecycle
static void test_leave_and_rejoin_lifecycle() {
	TestMicroService svc;

	std::string instA = "owsec-1-test-A";
	std::string instB = "owsec-1-test-B";
	std::string endpoint = "https://owsec-internal:16001";
	std::string validKey = "secret-token-hash-xyz";

	// 1. JOIN UUID-A -> registered
	svc.BusMessageReceived("service_events",
		MakePayload("join", instA, "owsec", endpoint, "https://owsec.example.com:16001", validKey, "1.0.0"));

	TEST_ASSERT(svc.GetServiceInstances("owsec").size() == 1, "owsec registered after JOIN");
	TEST_ASSERT(!svc.GetServices("owsec").empty(), "Route discoverable after JOIN");

	// 2. LEAVE UUID-A -> removed
	svc.BusMessageReceived("service_events",
		MakePayload("leave", instA, "owsec", endpoint, "https://owsec.example.com:16001"));

	TEST_ASSERT(svc.GetServiceInstances("owsec").empty(), "owsec instance removed after LEAVE");
	TEST_ASSERT(svc.GetServices("owsec").empty(), "Route removed after LEAVE");

	// 3. New process incarnation UUID-B joins -> registered cleanly
	svc.BusMessageReceived("service_events",
		MakePayload("join", instB, "owsec", endpoint, "https://owsec.example.com:16001", validKey, "1.0.0"));

	TEST_ASSERT(svc.GetServiceInstances("owsec").size() == 1, "New process incarnation UUID-B registered");
	TEST_ASSERT(svc.GetServiceInstances("owsec").find(instB) != svc.GetServiceInstances("owsec").end(), "UUID-B is active");

	std::cout << "  PASS: test_leave_and_rejoin_lifecycle" << std::endl;
}

// Test 16: Logical endpoint resolution operates in O(1) without scanning all replicas
static void test_logical_endpoints_efficient_lookup() {
	TestMicroService svc;

	std::string sharedEndpoint = "https://owgw-internal:17004";
	std::string sharedKey = "shared-owgw-key";

	// Register 100 replicas sharing the same private endpoint
	for (int i = 1; i <= 100; ++i) {
		std::string instId = fmt::format("owgw-{}-replica-uuid-{}", i, i);
		svc.BusMessageReceived("service_events",
			MakePayload("join", instId, "owgw", sharedEndpoint, "https://owgw.example.com:16004", sharedKey));
	}

	TEST_ASSERT(svc.GetServiceInstances("owgw").size() == 100, "100 replicas tracked in registry");

	// Logical resolution returns exactly 1 deduplicated route in O(1)
	auto routes = svc.GetServices("owgw");
	TEST_ASSERT(routes.size() == 1, "Exactly 1 deduplicated logical endpoint returned");
	TEST_ASSERT(routes[0].PrivateEndPoint == sharedEndpoint, "Endpoint URI matches shared private URI");
	TEST_ASSERT(routes[0].AccessKey == sharedKey, "Access key matches shared key");

	// Private endpoint key lookup finds key in O(1) across service types
	auto optKey = svc.GetPrivateEndPointServiceKey(sharedEndpoint);
	TEST_ASSERT(optKey.has_value(), "Private endpoint key found");
	TEST_ASSERT(optKey->AccessKey == sharedKey, "Key matches in registry");

	// Leave 99 replicas -> 1 remains, route still intact
	for (int i = 1; i <= 99; ++i) {
		std::string instId = fmt::format("owgw-{}-replica-uuid-{}", i, i);
		svc.BusMessageReceived("service_events",
			MakePayload("leave", instId, "owgw", sharedEndpoint, "https://owgw.example.com:16004"));
	}
	TEST_ASSERT(svc.GetServiceInstances("owgw").size() == 1, "1 replica remains in registry");
	TEST_ASSERT(svc.GetServices("owgw").size() == 1, "Route still alive while 1 replica remains");

	// Leave last replica -> route drops
	svc.BusMessageReceived("service_events",
		MakePayload("leave", "owgw-100-replica-uuid-100", "owgw", sharedEndpoint, "https://owgw.example.com:16004"));

	TEST_ASSERT(svc.GetServiceInstances("owgw").empty(), "No replicas remain");
	TEST_ASSERT(svc.GetServices("owgw").empty(), "Logical route removed when all replicas have left");
	TEST_ASSERT(!svc.GetPrivateEndPointServiceKey(sharedEndpoint).has_value(), "Private endpoint key removed");

	// Representative endpoint resolution across distinct service types
	std::string epGw = "https://owgw-internal:17004";
	std::string epSec = "https://owsec-internal:16001";
	svc.BusMessageReceived("service_events",
		MakePayload("join", "owgw-rep-1", "owgw", epGw, "https://owgw.example.com:16004", "key-gw"));
	svc.BusMessageReceived("service_events",
		MakePayload("join", "owsec-rep-1", "owsec", epSec, "https://owsec.example.com:16001", "key-sec"));

	auto routesGw = svc.GetServices("owgw");
	TEST_ASSERT(routesGw.size() == 1, "GetServices(owgw) returns 1 representative instance in O(1)");
	TEST_ASSERT(routesGw[0].PrivateEndPoint == epGw, "Matches OWGW endpoint");

	auto allRoutes = svc.GetServices();
	TEST_ASSERT(allRoutes.size() == 2, "GetServices() returns 1 representative instance per service type");

	auto keyGw = svc.GetPrivateEndPointServiceKey(epGw);
	TEST_ASSERT(keyGw.has_value() && keyGw->AccessKey == "key-gw", "OWGW endpoint key found");
	auto keySec = svc.GetPrivateEndPointServiceKey(epSec);
	TEST_ASSERT(keySec.has_value() && keySec->AccessKey == "key-sec", "OWSEC endpoint key found");

	svc.BusMessageReceived("service_events",
		MakePayload("leave", "owgw-rep-1", "owgw", epGw, "https://owgw.example.com:16004"));
	svc.BusMessageReceived("service_events",
		MakePayload("leave", "owsec-rep-1", "owsec", epSec, "https://owsec.example.com:16001"));

	std::cout << "  PASS: test_logical_endpoints_efficient_lookup" << std::endl;
}

// Test 17: Peer discovery consumer preserves shared endpoint route on replica departure
static void test_peer_discovery_consumer_preserves_shared_route_on_replica_leave() {
	// Simulate an upgraded peer service (e.g. OWGW / OWSEC) observing multiple OWPROV replicas
	TestMicroService peerConsumer("owgw");

	std::string provReplicaA = "owprov-1-aaaaaaaa-1111-4111-8111-111111111111";
	std::string provReplicaB = "owprov-2-bbbbbbbb-2222-4222-8222-222222222222";
	std::string sharedProvEndpoint = "https://owprov-internal:17005";
	std::string sharedProvKey = "owprov-shared-key-hash";

	// Both OWPROV replicas join
	peerConsumer.BusMessageReceived("service_events",
		MakePayload("join", provReplicaA, "owprov", sharedProvEndpoint, "https://owprov.example.com:16005", sharedProvKey));
	peerConsumer.BusMessageReceived("service_events",
		MakePayload("join", provReplicaB, "owprov", sharedProvEndpoint, "https://owprov.example.com:16005", sharedProvKey));

	TEST_ASSERT(peerConsumer.GetServiceInstances("owprov").size() == 2, "Peer consumer tracks both OWPROV replicas");
	TEST_ASSERT(peerConsumer.GetServices("owprov").size() == 1, "Peer consumer routes to single shared load-balanced endpoint");

	// OWPROV Replica A shuts down and emits LEAVE
	peerConsumer.BusMessageReceived("service_events",
		MakePayload("leave", provReplicaA, "owprov", sharedProvEndpoint, "https://owprov.example.com:16005"));

	// Verification: Upgraded peer consumer does NOT unregister the shared route!
	TEST_ASSERT(peerConsumer.GetServiceInstances("owprov").size() == 1, "Replica B remains in peer registry");
	TEST_ASSERT(peerConsumer.GetServices("owprov").size() == 1, "Peer consumer preserves shared route while Replica B is active");
	TEST_ASSERT(peerConsumer.GetServices("owprov")[0].PrivateEndPoint == sharedProvEndpoint, "Shared endpoint remains routable");

	// OWPROV Replica B shuts down and emits LEAVE
	peerConsumer.BusMessageReceived("service_events",
		MakePayload("leave", provReplicaB, "owprov", sharedProvEndpoint, "https://owprov.example.com:16005"));

	// Only when the last replica leaves is the route removed
	TEST_ASSERT(peerConsumer.GetServiceInstances("owprov").empty(), "No replicas remain in peer registry");
	TEST_ASSERT(peerConsumer.GetServices("owprov").empty(), "Route dropped once all replicas departed");

	std::cout << "  PASS: test_peer_discovery_consumer_preserves_shared_route_on_replica_leave" << std::endl;
}

// Test 18: Self-event detection uses instanceId and does not collide on identical default numeric ID
static void test_self_event_detection_uses_instance_identity() {
	TestMicroService svc; // Initialized with InstanceId "owprov-1", default numeric ID_ = 10

	// 1. Peer instance "owprov-2" has a different instanceId, but identical numeric ID (10)
	// Must NOT be ignored because instanceId is present and distinct!
	Poco::JSON::Object peerMsg;
	peerMsg.set(KafkaTopics::ServiceEvents::Fields::EVENT, "join");
	peerMsg.set(KafkaTopics::ServiceEvents::Fields::ID, (uint64_t)10); // Same numeric ID as svc (ID_ = 10)
	peerMsg.set("instanceId", "owprov-2"); // Different instance ID
	peerMsg.set(KafkaTopics::ServiceEvents::Fields::TYPE, "owprov");
	peerMsg.set(KafkaTopics::ServiceEvents::Fields::PRIVATE, "https://owprov-internal:17005");
	peerMsg.set(KafkaTopics::ServiceEvents::Fields::PUBLIC, "https://owprov.example.com:16005");
	peerMsg.set(KafkaTopics::ServiceEvents::Fields::KEY, "valid-key");
	peerMsg.set(KafkaTopics::ServiceEvents::Fields::VRSN, "1.0.0");
	std::stringstream ssPeer;
	Poco::JSON::Stringifier::stringify(peerMsg, ssPeer);

	svc.BusMessageReceived("service_events", ssPeer.str());
	TEST_ASSERT(svc.GetServiceInstances("owprov").size() == 1, "Peer with different UUID registered, not ignored despite colliding numeric ID");

	// 2. Self event with matching instance identity must be ignored
	Poco::JSON::Object selfMsg;
	selfMsg.set(KafkaTopics::ServiceEvents::Fields::EVENT, "join");
	selfMsg.set(KafkaTopics::ServiceEvents::Fields::ID, (uint64_t)10);
	selfMsg.set("instanceId", svc.InstanceId()); // Matches svc.InstanceId()
	selfMsg.set(KafkaTopics::ServiceEvents::Fields::TYPE, "owprov");
	selfMsg.set(KafkaTopics::ServiceEvents::Fields::PRIVATE, "https://owprov-internal:17005");
	selfMsg.set(KafkaTopics::ServiceEvents::Fields::PUBLIC, "https://owprov.example.com:16005");
	selfMsg.set(KafkaTopics::ServiceEvents::Fields::KEY, "valid-key");
	selfMsg.set(KafkaTopics::ServiceEvents::Fields::VRSN, "1.0.0");
	std::stringstream ssSelf;
	Poco::JSON::Stringifier::stringify(selfMsg, ssSelf);

	svc.BusMessageReceived("service_events", ssSelf.str());
	auto instMapAfterSelf = svc.GetServiceInstances("owprov");
	TEST_ASSERT(instMapAfterSelf.find(svc.InstanceId()) == instMapAfterSelf.end(),
		"Self-broadcast with matching UUID ignored");

	// 3. Legacy peer event with numeric ID colliding with ID_ = 10 and same type "owprov"
	// must NOT be dropped as self-event, because it lacks the process's unique runtime instanceId UUID.
	Poco::JSON::Object legacyMsg;
	legacyMsg.set(KafkaTopics::ServiceEvents::Fields::EVENT, "join");
	legacyMsg.set(KafkaTopics::ServiceEvents::Fields::ID, (uint64_t)10); // Same numeric ID as svc (ID_ = 10)
	legacyMsg.set(KafkaTopics::ServiceEvents::Fields::TYPE, "owprov");
	legacyMsg.set(KafkaTopics::ServiceEvents::Fields::PRIVATE, "https://owprov-legacy:17005");
	legacyMsg.set(KafkaTopics::ServiceEvents::Fields::PUBLIC, "https://owprov-legacy.example.com:16005");
	legacyMsg.set(KafkaTopics::ServiceEvents::Fields::KEY, "valid-key");
	legacyMsg.set(KafkaTopics::ServiceEvents::Fields::VRSN, "1.0.0");
	std::stringstream ssLegacy;
	Poco::JSON::Stringifier::stringify(legacyMsg, ssLegacy);

	svc.BusMessageReceived("service_events", ssLegacy.str());
	auto instMapAfterLegacy = svc.GetServiceInstances("owprov");
	TEST_ASSERT(instMapAfterLegacy.find("10@https://owprov-legacy:17005") != instMapAfterLegacy.end(),
		"Legacy peer event with colliding numeric ID and same type must be registered, not dropped as self");

	std::cout << "  PASS: test_self_event_detection_uses_instance_identity" << std::endl;
}

// Test 19: Legacy peer services vs Upgraded peer services migration behavior
// Verifies Section 12.4 requirement: legacy peers keying by PrivateEndPoint drop routes on replica LEAVE,
// requiring peer services to be upgraded to instance-aware discovery prior to multi-replica scaling.
static void test_legacy_peer_vs_upgraded_peer_migration_behavior() {
	std::string sharedEndpoint = "https://owprov-internal:17005";
	std::string sharedKey = "shared-prov-key";
	std::string replicaA = "owprov-1-replica-A";
	std::string replicaB = "owprov-2-replica-B";

	// 1. Upgraded peer consumer (tracks Services_[Type][InstanceId])
	TestMicroService upgradedPeer("owgw");

	upgradedPeer.BusMessageReceived("service_events",
		MakePayload("join", replicaA, "owprov", sharedEndpoint, "https://owprov.example.com:16005", sharedKey));
	upgradedPeer.BusMessageReceived("service_events",
		MakePayload("join", replicaB, "owprov", sharedEndpoint, "https://owprov.example.com:16005", sharedKey));

	TEST_ASSERT(upgradedPeer.GetServiceInstances("owprov").size() == 2, "Upgraded peer tracks both replicas");
	TEST_ASSERT(upgradedPeer.GetServices("owprov").size() == 1, "Upgraded peer routes to shared endpoint");

	// Replica A leaves
	upgradedPeer.BusMessageReceived("service_events",
		MakePayload("leave", replicaA, "owprov", sharedEndpoint, "https://owprov.example.com:16005"));

	// Upgraded peer preserves shared route
	TEST_ASSERT(upgradedPeer.GetServiceInstances("owprov").size() == 1, "Upgraded peer retains Replica B");
	TEST_ASSERT(!upgradedPeer.GetServices("owprov").empty(), "Upgraded peer preserves shared route while Replica B lives");

	// 2. Legacy peer consumer simulation (mimics pre-Phase 3 MicroService.cpp keyed by PrivateEndPoint)
	struct LegacyPeerSimulator {
		std::map<std::string, Types::MicroServiceMeta> Services; // PrivateEndPoint -> Meta

		void HandleEvent(const std::string &event, const std::string &privateEndpoint, const std::string &type) {
			if (event == "leave") {
				// Legacy behavior: unregisters the entire shared private endpoint!
				Services.erase(privateEndpoint);
			} else if (event == "join" || event == "keep-alive") {
				Services[privateEndpoint] = Types::MicroServiceMeta{
					.InstanceId = "",
					.Id = 0,
					.Type = type,
					.PrivateEndPoint = privateEndpoint,
					.PublicEndPoint = "https://" + type + ".example.com",
					.AccessKey = "legacy-key",
					.Version = "1.0.0",
					.LastUpdate = Utils::Now()
				};
			}
		}
	} legacyPeer;

	// Both replicas join
	legacyPeer.HandleEvent("join", sharedEndpoint, "owprov");
	TEST_ASSERT(legacyPeer.Services.find(sharedEndpoint) != legacyPeer.Services.end(), "Legacy peer registers shared endpoint");

	// Replica A leaves: in legacy peer, this drops the shared endpoint even though Replica B is running!
	legacyPeer.HandleEvent("leave", sharedEndpoint, "owprov");
	TEST_ASSERT(legacyPeer.Services.find(sharedEndpoint) == legacyPeer.Services.end(),
		"Legacy peer unregisters shared endpoint on replica LEAVE (demonstrates migration prerequisite)");

	std::cout << "  PASS: test_legacy_peer_vs_upgraded_peer_migration_behavior" << std::endl;
}

// Test 20: Distinct private endpoints of the same service type are both discovered and resolvable
static void test_distinct_endpoints_same_service_type() {
	TestMicroService svc;

	std::string instEast1 = "owgw-east-1";
	std::string instEast2 = "owgw-east-2";
	std::string instWest1 = "owgw-west-1";

	std::string epEast = "https://owgw-east-internal:17004";
	std::string epWest = "https://owgw-west-internal:17004";
	std::string keyEast = "key-east-region";
	std::string keyWest = "key-west-region";

	// Register 2 replicas in East region sharing epEast, and 1 replica in West region with epWest
	svc.BusMessageReceived("service_events",
		MakePayload("join", instEast1, "owgw", epEast, "https://owgw-east.example.com:16004", keyEast));
	svc.BusMessageReceived("service_events",
		MakePayload("join", instEast2, "owgw", epEast, "https://owgw-east.example.com:16004", keyEast));
	svc.BusMessageReceived("service_events",
		MakePayload("join", instWest1, "owgw", epWest, "https://owgw-west.example.com:16004", keyWest));

	// All 3 instances tracked
	auto instances = svc.GetServiceInstances("owgw");
	TEST_ASSERT(instances.size() == 3, "All 3 instances tracked in registry");

	// GetServices("owgw") deduplicates shared epEast while preserving distinct epWest -> returns 2 endpoints
	auto routes = svc.GetServices("owgw");
	TEST_ASSERT(routes.size() == 2, "GetServices(owgw) must return both distinct endpoints");

	std::set<std::string> endpoints;
	for (const auto &r : routes) {
		endpoints.insert(r.PrivateEndPoint);
	}
	TEST_ASSERT(endpoints.count(epEast) == 1, "East endpoint is returned");
	TEST_ASSERT(endpoints.count(epWest) == 1, "West endpoint is returned");

	// Both endpoints resolvable via GetPrivateEndPointServiceKey
	auto metaEast = svc.GetPrivateEndPointServiceKey(epEast);
	TEST_ASSERT(metaEast.has_value(), "epEast resolvable");
	TEST_ASSERT(metaEast->AccessKey == keyEast, "East access key matches");

	auto metaWest = svc.GetPrivateEndPointServiceKey(epWest);
	TEST_ASSERT(metaWest.has_value(), "epWest resolvable");
	TEST_ASSERT(metaWest->AccessKey == keyWest, "West access key matches");

	// Replica East 1 leaves -> East 2 and West 1 still active, so both endpoints still returned
	svc.BusMessageReceived("service_events",
		MakePayload("leave", instEast1, "owgw", epEast, "https://owgw-east.example.com:16004"));

	routes = svc.GetServices("owgw");
	TEST_ASSERT(routes.size() == 2, "Both endpoints still active after 1 of 2 East replicas leaves");

	// Replica East 2 leaves -> only West endpoint remains
	svc.BusMessageReceived("service_events",
		MakePayload("leave", instEast2, "owgw", epEast, "https://owgw-east.example.com:16004"));

	routes = svc.GetServices("owgw");
	TEST_ASSERT(routes.size() == 1, "Only West endpoint remains after all East replicas leave");
	TEST_ASSERT(routes[0].PrivateEndPoint == epWest, "Remaining endpoint is West");
	TEST_ASSERT(!svc.GetPrivateEndPointServiceKey(epEast).has_value(), "epEast no longer resolvable");

	std::cout << "  PASS: test_distinct_endpoints_same_service_type" << std::endl;
}

// Test 21: Backward clock adjustment does NOT mark healthy instances as stale
static void test_backward_clock_adjustment_safety() {
	TestMicroService svc;

	std::string instNormal = "owsec-1-normal";
	std::string instFuture = "owsec-2-future";
	std::string instStale = "owsec-3-stale";
	std::string endpoint = "https://owsec-internal:16001";

	svc.BusMessageReceived("service_events",
		MakePayload("join", instNormal, "owsec", endpoint, "https://owsec.example.com:16001"));
	svc.BusMessageReceived("service_events",
		MakePayload("join", instFuture, "owsec", endpoint, "https://owsec.example.com:16001"));
	svc.BusMessageReceived("service_events",
		MakePayload("join", instStale, "owsec", endpoint, "https://owsec.example.com:16001"));

	TEST_ASSERT(svc.GetServiceInstances("owsec").size() == 3, "3 instances initially registered");

	auto now = Utils::Now();
	// instNormal: updated now
	svc.SetInstanceLastUpdateForTest("owsec", instNormal, now);
	// instFuture: updated with timestamp in the future relative to `now` (simulates clock moved backward)
	svc.SetInstanceLastUpdateForTest("owsec", instFuture, now + 60);
	// instStale: updated 150s ago (genuinely stale)
	svc.SetInstanceLastUpdateForTest("owsec", instStale, now - 150);

	// Receive event: stale cleanup executes immediately on the received event
	svc.BusMessageReceived("service_events",
		MakePayload("keep-alive", instNormal, "owsec", endpoint, "https://owsec.example.com:16001"));

	// Crucial assertion:
	// instStale must be pruned (>120s old)
	// instNormal must be preserved (healthy)
	// instFuture must be preserved (now < LastUpdate does NOT underflow into >120)
	auto instances = svc.GetServiceInstances("owsec");
	TEST_ASSERT(instances.size() == 2, "Only genuinely stale instance pruned; healthy instances preserved under backward clock");
	TEST_ASSERT(instances.find(instNormal) != instances.end(), "instNormal preserved");
	TEST_ASSERT(instances.find(instFuture) != instances.end(), "instFuture preserved despite now < LastUpdate");
	TEST_ASSERT(instances.find(instStale) == instances.end(), "instStale pruned");

	std::cout << "  PASS: test_backward_clock_adjustment_safety" << std::endl;
}

// Test 22: GetServices() preserves distinct service types even if they share the same private endpoint
static void test_get_services_preserves_distinct_types_sharing_endpoint() {
	TestMicroService svc;

	std::string sharedIngress = "https://shared-ingress.internal:8443";
	std::string gwInst1 = "owgw-1";
	std::string gwInst2 = "owgw-2";
	std::string secInst1 = "owsec-1";
	std::string secInst2 = "owsec-2";

	// 2 replicas of owgw sharing sharedIngress
	svc.BusMessageReceived("service_events",
		MakePayload("join", gwInst1, "owgw", sharedIngress, "https://owgw.example.com:16004", "key-gw"));
	svc.BusMessageReceived("service_events",
		MakePayload("join", gwInst2, "owgw", sharedIngress, "https://owgw.example.com:16004", "key-gw"));

	// 2 replicas of owsec sharing the same sharedIngress
	svc.BusMessageReceived("service_events",
		MakePayload("join", secInst1, "owsec", sharedIngress, "https://owsec.example.com:16001", "key-sec"));
	svc.BusMessageReceived("service_events",
		MakePayload("join", secInst2, "owsec", sharedIngress, "https://owsec.example.com:16001", "key-sec"));

	// GetServices("owgw") and GetServices("owsec") both return 1 deduplicated route
	TEST_ASSERT(svc.GetServices("owgw").size() == 1, "owgw returns 1 deduplicated route");
	TEST_ASSERT(svc.GetServices("owsec").size() == 1, "owsec returns 1 deduplicated route");

	// Critical check: Unfiltered GetServices() must return 2 entries (one for owgw and one for owsec),
	// NOT collapse them into 1 just because they share the same PrivateEndPoint!
	auto allServices = svc.GetServices();
	TEST_ASSERT(allServices.size() == 2, "GetServices() must return 2 services, one for each distinct type");

	std::set<std::string> returnedTypes;
	for (const auto &s : allServices) {
		returnedTypes.insert(s.Type);
	}
	TEST_ASSERT(returnedTypes.count("owgw") == 1, "owgw is present in GetServices()");
	TEST_ASSERT(returnedTypes.count("owsec") == 1, "owsec is present in GetServices()");

	std::cout << "  PASS: test_get_services_preserves_distinct_types_sharing_endpoint" << std::endl;
}

// Test 23: Malformed events are rejected without bypassing stale-instance cleanup
static void test_malformed_event_does_not_bypass_stale_cleanup() {
	TestMicroService svc;

	std::string instActive = "owgw-active";
	std::string instStale = "owgw-stale";
	std::string endpoint = "https://owgw-internal:17004";

	svc.BusMessageReceived("service_events",
		MakePayload("join", instActive, "owgw", endpoint, "https://owgw.example.com:16004"));
	svc.BusMessageReceived("service_events",
		MakePayload("join", instStale, "owgw", endpoint, "https://owgw.example.com:16004"));

	TEST_ASSERT(svc.GetServiceInstances("owgw").size() == 2, "2 instances registered");

	// Mark instStale as expired (>120s old)
	svc.SetInstanceLastUpdateForTest("owgw", instStale, Utils::Now() - 130);

	// Receive a malformed event with empty instanceId and empty id
	Poco::JSON::Object badMsg;
	badMsg.set(KafkaTopics::ServiceEvents::Fields::EVENT, "join");
	badMsg.set(KafkaTopics::ServiceEvents::Fields::ID, "");
	badMsg.set("instanceId", "");
	badMsg.set(KafkaTopics::ServiceEvents::Fields::TYPE, "owgw");
	badMsg.set(KafkaTopics::ServiceEvents::Fields::PRIVATE, endpoint);
	badMsg.set(KafkaTopics::ServiceEvents::Fields::PUBLIC, "https://owgw.example.com:16004");
	badMsg.set(KafkaTopics::ServiceEvents::Fields::KEY, "key");
	badMsg.set(KafkaTopics::ServiceEvents::Fields::VRSN, "1.0.0");
	std::stringstream ss;
	Poco::JSON::Stringifier::stringify(badMsg, ss);

	svc.BusMessageReceived("service_events", ss.str());

	// Verify that the malformed event was rejected AND the stale instance was pruned by downstream cleanup!
	auto instances = svc.GetServiceInstances("owgw");
	TEST_ASSERT(instances.size() == 1, "Malformed event rejected and stale instance pruned");
	TEST_ASSERT(instances.find(instActive) != instances.end(), "Active instance retained");
	TEST_ASSERT(instances.find(instStale) == instances.end(), "Stale instance pruned despite malformed event");

	std::cout << "  PASS: test_malformed_event_does_not_bypass_stale_cleanup" << std::endl;
}

// Test 24: Legacy peers with same numeric ID but distinct endpoints are preserved independently
static void test_legacy_peers_distinct_endpoints_same_numeric_id() {
	TestMicroService svc;

	std::string epEast = "https://owgw-east:17004";
	std::string epWest = "https://owgw-west:17004";

	// Peer East emits legacy JOIN (id = 10, no instanceId)
	Poco::JSON::Object msgEast;
	msgEast.set(KafkaTopics::ServiceEvents::Fields::EVENT, "join");
	msgEast.set(KafkaTopics::ServiceEvents::Fields::ID, (uint64_t)10);
	msgEast.set(KafkaTopics::ServiceEvents::Fields::TYPE, "owgw");
	msgEast.set(KafkaTopics::ServiceEvents::Fields::PRIVATE, epEast);
	msgEast.set(KafkaTopics::ServiceEvents::Fields::PUBLIC, "https://owgw-east.example.com:16004");
	msgEast.set(KafkaTopics::ServiceEvents::Fields::KEY, "east-key");
	msgEast.set(KafkaTopics::ServiceEvents::Fields::VRSN, "1.0.0");
	std::stringstream ssEast;
	Poco::JSON::Stringifier::stringify(msgEast, ssEast);
	svc.BusMessageReceived("service_events", ssEast.str());

	// Peer West emits legacy JOIN (same id = 10, no instanceId, different endpoint)
	Poco::JSON::Object msgWest;
	msgWest.set(KafkaTopics::ServiceEvents::Fields::EVENT, "join");
	msgWest.set(KafkaTopics::ServiceEvents::Fields::ID, (uint64_t)10);
	msgWest.set(KafkaTopics::ServiceEvents::Fields::TYPE, "owgw");
	msgWest.set(KafkaTopics::ServiceEvents::Fields::PRIVATE, epWest);
	msgWest.set(KafkaTopics::ServiceEvents::Fields::PUBLIC, "https://owgw-west.example.com:16004");
	msgWest.set(KafkaTopics::ServiceEvents::Fields::KEY, "west-key");
	msgWest.set(KafkaTopics::ServiceEvents::Fields::VRSN, "1.0.0");
	std::stringstream ssWest;
	Poco::JSON::Stringifier::stringify(msgWest, ssWest);
	svc.BusMessageReceived("service_events", ssWest.str());

	auto instances = svc.GetServiceInstances("owgw");
	TEST_ASSERT(instances.size() == 2, "Both legacy instances registered despite sharing numeric ID 10");
	TEST_ASSERT(instances.find("10@" + epEast) != instances.end(), "East legacy instance registered");
	TEST_ASSERT(instances.find("10@" + epWest) != instances.end(), "West legacy instance registered");

	auto routes = svc.GetServices("owgw");
	TEST_ASSERT(routes.size() == 2, "Both distinct private endpoints available in GetServices");

	// Peer East sends LEAVE
	Poco::JSON::Object leaveEast;
	leaveEast.set(KafkaTopics::ServiceEvents::Fields::EVENT, "leave");
	leaveEast.set(KafkaTopics::ServiceEvents::Fields::ID, (uint64_t)10);
	leaveEast.set(KafkaTopics::ServiceEvents::Fields::TYPE, "owgw");
	leaveEast.set(KafkaTopics::ServiceEvents::Fields::PRIVATE, epEast);
	leaveEast.set(KafkaTopics::ServiceEvents::Fields::PUBLIC, "https://owgw-east.example.com:16004");
	leaveEast.set(KafkaTopics::ServiceEvents::Fields::KEY, "east-key");
	leaveEast.set(KafkaTopics::ServiceEvents::Fields::VRSN, "1.0.0");
	std::stringstream ssLeaveEast;
	Poco::JSON::Stringifier::stringify(leaveEast, ssLeaveEast);
	svc.BusMessageReceived("service_events", ssLeaveEast.str());

	instances = svc.GetServiceInstances("owgw");
	TEST_ASSERT(instances.size() == 1, "Only East legacy instance removed on LEAVE");
	TEST_ASSERT(instances.find("10@" + epWest) != instances.end(), "West legacy instance retained");

	routes = svc.GetServices("owgw");
	TEST_ASSERT(routes.size() == 1, "West route preserved in GetServices");
	TEST_ASSERT(routes[0].PrivateEndPoint == epWest, "Remaining route is West endpoint");

	std::cout << "  PASS: test_legacy_peers_distinct_endpoints_same_numeric_id" << std::endl;
}

int main() {
	std::cout << "=== Running Service Discovery Framework Tests ===" << std::endl;

	test_multi_instance_same_endpoint_registration();
	test_idempotent_join_and_keepalive();
	test_independent_leave_preserves_routing();
	test_all_instances_leave_removes_route();
	test_rolling_restart_overlap();
	test_self_event_ignored();
	test_legacy_peer_compatibility();
	test_get_private_endpoint_service_key();
	test_make_system_event_message_canonical_identity();
	test_partial_keepalive_preserves_credentials();
	test_empty_instance_id_rejected_preserves_replicas();
	test_uuid_id_and_numeric_id_no_exceptions();
	test_zombie_sweep_pruning();
	test_empty_or_invalid_endpoint_uri_rejected_no_empty_group();
	test_leave_and_rejoin_lifecycle();
	test_logical_endpoints_efficient_lookup();
	test_peer_discovery_consumer_preserves_shared_route_on_replica_leave();
	test_self_event_detection_uses_instance_identity();
	test_legacy_peer_vs_upgraded_peer_migration_behavior();
	test_distinct_endpoints_same_service_type();
	test_backward_clock_adjustment_safety();
	test_get_services_preserves_distinct_types_sharing_endpoint();
	test_malformed_event_does_not_bypass_stale_cleanup();
	test_legacy_peers_distinct_endpoints_same_numeric_id();

	std::cout << "=== All Service Discovery Tests PASSED! ===" << std::endl;
	return 0;
}
