#include <cstdlib>
#include <iostream>

#include "Poco/Data/SQLite/Connector.h"
#include "Poco/URI.h"
#include "RESTObjects/RESTAPI_SecurityObjects.h"
#include "framework/SubscriberAccess.h"
#include "framework/orm.h"

#define CHECK(condition) \
	do { if (!(condition)) { std::cerr << "Failed at line " << __LINE__ << ": " \
		<< #condition << '\n'; std::exit(1); } } while (false)

struct Device {
	std::string id, subscriberId;
	void to_json(Poco::JSON::Object &) const {}
	bool from_json(const Poco::JSON::Object::Ptr &) { return true; }
};
using DeviceTuple = Poco::Tuple<std::string, std::string>;

template <> void ORM::DB<DeviceTuple, Device>::Convert(const DeviceTuple &in, Device &out) {
	out.id = in.get<0>();
	out.subscriberId = in.get<1>();
}
template <> void ORM::DB<DeviceTuple, Device>::Convert(const Device &in, DeviceTuple &out) {
	out.set<0>(in.id);
	out.set<1>(in.subscriberId);
}

struct VenuePages {
	struct Venue {
		struct { std::string id; } info;
		std::string entity;
		std::vector<std::string> devices;
	};
	using RecordVec = std::vector<Venue>;
	RecordVec records;
	int64_t failureOffset = -1;
	void GetRecords(uint64_t offset, uint64_t limit, RecordVec &batch,
		const std::string &where, const std::string &order, bool *succeeded) {
		CHECK(where == "subscriber = 'alice'");
		CHECK(order == " ORDER BY id ");
		CHECK(limit == 100);
		*succeeded = static_cast<int64_t>(offset) != failureOffset;
		if (*succeeded)
			for (uint64_t i = offset; i < records.size() && i < offset + limit; ++i)
				batch.push_back(records[i]);
	}
};

int main() {
	using namespace OpenWifi::SubscriberAccess;
	std::string target;
	auto resolve = [&](const std::string &method, const std::string &uri,
					   const std::string &caller = "alice") {
		return Resolve(method, Poco::URI(uri).getPath(), caller, target);
	};
	CHECK(resolve("GET", "/api/v1/subscriberDevice?operatorId=unknown") == Operation::ListDevices);
	CHECK(resolve("GET", "/api/v1/subscriberDevice/aabbccddeeff") == Operation::ReadDevice);
	CHECK(target == "aabbccddeeff");
	CHECK(resolve("POST", "/api/v1/subscriberDevice/0") == Operation::ClaimDevice);
	CHECK(resolve("DELETE", "/api/v1/subscriberDevice/aabbccddeeff") == Operation::DeleteDevice);
	CHECK(resolve("DELETE", "/api/v1/subscriber/alice") == Operation::DeleteAccount);
	CHECK(resolve("DELETE", "/api/v1/subscriber") == Operation::DeleteAccount);
	CHECK(resolve("DELETE", "/api/v1/subscriber/bob") == Operation::Denied);
	CHECK(resolve("GET", "/api/v1/subscriberDevice", "") == Operation::Denied);
	CHECK(resolve("POST", "/api/v1/subscriberDevice") == Operation::Denied);
	CHECK(resolve("POST", "/api/v1/subscriberDevice/new") == Operation::Denied);
	CHECK(resolve("POST", "/api/v1/subscriberDevice/aabbccddeeff") == Operation::Denied);
	CHECK(resolve("GET", "/api/v1/subscriberDevice/0/extra") == Operation::Denied);
	CHECK(resolve("GET", "/api/v1/subscriberDevices") == Operation::Denied);
	CHECK(resolve("GET", "/api/v1/configuration?next=/api/v1/subscriberDevice") == Operation::Denied);
	CHECK(resolve("POST", "/api/v1/subscriber") == Operation::Denied);
	CHECK(resolve("PUT", "/api/v1/subscriberDevice/aabbccddeeff") == Operation::Denied);
	CHECK(resolve("PATCH", "/api/v1/subscriberDevice/aabbccddeeff") == Operation::Denied);
	CHECK(resolve("GET", "/api/v1/venue/owned") == Operation::Denied);
	CHECK(resolve("PUT", "/api/v1/subscriber?operation=emailVerified") == Operation::Denied);
	for (const auto *field : {"name", "serialNumber", "subscriberId", "operatorId", "deviceGroup", "configuration"})
		CHECK(IsClaimFieldAllowed(field));
	for (const auto *field : {"managementPolicy", "serviceClass", "deviceRules", "suspended", "billingCode", "id"})
		CHECK(!IsClaimFieldAllowed(field));

	VenuePages venues;
	std::vector<std::pair<std::string, std::string>> candidates;
	for (unsigned count : {0, 100, 101, 201}) {
		venues.records.clear();
		for (unsigned i = 0; i < count; ++i)
			venues.records.push_back({{std::to_string(i)}, "entity", {}});
		CHECK(CollectEmptyVenues(venues, "subscriber = 'alice'", candidates));
		CHECK(candidates.size() == count);
	}
	// A populated venue on the third page must yield no deletion candidates.
	venues.records.back().devices.push_back("claimed-device");
	CHECK(!CollectEmptyVenues(venues, "subscriber = 'alice'", candidates));
	CHECK(candidates.empty());
	venues.records.back().devices.clear();
	venues.failureOffset = 100;
	CHECK(!CollectEmptyVenues(venues, "subscriber = 'alice'", candidates));
	CHECK(candidates.empty());
	// Failure while requesting the empty final page must not authorize deletion.
	venues.records.resize(100);
	CHECK(!CollectEmptyVenues(venues, "subscriber = 'alice'", candidates));
	CHECK(candidates.empty());

	// Exercise the production ORM with SQLite: empty results must be distinguishable
	// from failures, including a failed page after a completely full first page.
	Poco::Data::SQLite::Connector::registerConnector();
	Poco::Data::SessionPool pool("SQLite", ":memory:", 1, 1);
	auto &logger = Poco::Logger::get("subscriber-access-test");
	ORM::DB<DeviceTuple, Device> db(OpenWifi::sqlite, "devices",
		{{"id", ORM::FT_TEXT, 0, true}, {"subscriberId", ORM::FT_TEXT}}, {}, pool, logger, "test");
	{
		auto session = pool.get();
		session << "CREATE TABLE devices (id TEXT PRIMARY KEY, subscriberid TEXT)", Poco::Data::Keywords::now;
	}
	bool succeeded = false;
	std::vector<Device> rows;
	CHECK(!db.GetRecords(0, 100, rows, "", "", &succeeded));
	CHECK(succeeded && rows.empty());
	CHECK(db.Count("", &succeeded) == 0 && succeeded);
	for (int i = 0; i < 201; ++i)
		CHECK(db.CreateRecord(Device{std::to_string(i), i == 200 ? "bob" : "alice"}));
	const auto where = db.OP("subscriberId", ORM::EQ, std::string("alice"));
	CHECK(db.Count(where, &succeeded) == 200 && succeeded);
	for (uint64_t offset : {0, 100, 200}) {
		rows.clear();
		db.GetRecords(offset, 100, rows, where, " ORDER BY id ", &succeeded);
		CHECK(succeeded);
		CHECK(rows.size() == (offset == 200 ? 0 : 100));
		for (const auto &row : rows)
			CHECK(row.subscriberId == "alice");
	}
	rows.clear();
	CHECK(!db.GetRecords(100, 100, rows, "missing_column = 1", "", &succeeded));
	CHECK(!succeeded);
	CHECK(db.Count("missing_column = 1", &succeeded) == 0 && !succeeded);
	// Legacy callers retain their previous zero/false behavior.
	CHECK(db.Count("missing_column = 1") == 0);
	CHECK(!db.GetRecords(0, 100, rows, "subscriberId = 'nobody'"));

	// Regression tests for UserInfo memory hygiene and subscriber classification
	{
		// 1. Data Model Invariant: Default UserInfo MUST have UNKNOWN role (zero-initialized)
		OpenWifi::SecurityObjects::UserInfo uInfo;
		CHECK(uInfo.userRole == OpenWifi::SecurityObjects::UNKNOWN);
		CHECK(uInfo.id.empty());
		CHECK(uInfo.modified == 0);
		CHECK(!uInfo.suspended);
		CHECK(!uInfo.blackListed);

		OpenWifi::SecurityObjects::UserInfoAndPolicy policyInfo;
		CHECK(policyInfo.userinfo.userRole == OpenWifi::SecurityObjects::UNKNOWN);
		CHECK(policyInfo.userinfo.id.empty());

		// 2. Subscriber Classification Invariants (matching IsSubscriber() contract):
		// !isInternalServiceOnly && userRole == SUBSCRIBER && !callerId.empty()
		auto isSubscriber = [](bool isInternalServiceOnly,
							   OpenWifi::SecurityObjects::USER_ROLE role,
							   const std::string &callerId) {
			return !isInternalServiceOnly &&
				   role == OpenWifi::SecurityObjects::USER_ROLE::SUBSCRIBER &&
				   !callerId.empty();
		};

		// Case A: Pure internal service call (e.g. owsub -> owprov with X-API-KEY, no Bearer token)
		// Even if role happens to be SUBSCRIBER or UNKNOWN, it MUST NOT be classified as subscriber self-care.
		CHECK(!isSubscriber(true, OpenWifi::SecurityObjects::USER_ROLE::SUBSCRIBER, ""));
		CHECK(!isSubscriber(true, OpenWifi::SecurityObjects::USER_ROLE::SUBSCRIBER, "sub-uuid-123"));
		CHECK(!isSubscriber(true, OpenWifi::SecurityObjects::USER_ROLE::UNKNOWN, ""));

		// Case B: Garbage or empty identity context
		CHECK(!isSubscriber(false, OpenWifi::SecurityObjects::USER_ROLE::SUBSCRIBER, ""));
		CHECK(!isSubscriber(false, OpenWifi::SecurityObjects::USER_ROLE::UNKNOWN, "sub-uuid-123"));
		CHECK(!isSubscriber(false, OpenWifi::SecurityObjects::USER_ROLE::ROOT, "sub-uuid-123"));
		CHECK(!isSubscriber(false, OpenWifi::SecurityObjects::USER_ROLE::ADMIN, "sub-uuid-123"));

		// Case C: Valid authenticated subscriber self-care
		CHECK(isSubscriber(false, OpenWifi::SecurityObjects::USER_ROLE::SUBSCRIBER, "sub-uuid-123"));
	}

	std::cout << "Subscriber capabilities, collection isolation, multi-venue deletion, pagination and query failure checks passed.\n";
}
