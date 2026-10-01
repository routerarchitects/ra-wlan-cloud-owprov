//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "framework/InstanceIdentity.h"

#define TEST_ASSERT(cond, msg)                                                                   \
	do {                                                                                         \
		if (!(cond)) {                                                                           \
			std::cerr << "TEST FAILURE [" << __FILE__ << ":" << __LINE__ << "]: " << msg        \
					  << std::endl;                                                              \
			std::exit(1);                                                                        \
		}                                                                                        \
	} while (0)

// Known UUIDs so tests are deterministic and independent of the real UUID generator.
static const std::string kUUID1 = "a1b2c3d4-e5f6-4a1b-9c2d-3e4f5a6b7c8d";
static const std::string kUUID2 = "11111111-2222-4333-8444-555555555555";

// A. RuntimeIncarnationId is always available after construction (before Initialize).
//
//    Design note on InstanceId() pre-init behavior:
//    InstanceId() intentionally throws before Initialize() to enforce value stability.
//    If it returned RuntimeIncarnationId early and then returned a slot-prefixed value
//    after Initialize(), the same accessor would return two different strings during one
//    process lifetime -- violating the stability contract.
//    Early-startup code that needs an identity must use RuntimeIncarnationId() instead.
static void test_runtime_id_available_before_init() {
	OpenWifi::InstanceIdentity id(kUUID1);

	TEST_ASSERT(!id.IsInitialized(),
		"IsInitialized must be false before Initialize()");
	TEST_ASSERT(id.RuntimeIncarnationId() == kUUID1,
		"RuntimeIncarnationId must be accessible before Initialize()");
	// SlotId returns empty before Initialize.
	TEST_ASSERT(id.SlotId().empty(),
		"SlotId must be empty before Initialize()");
	// InstanceId() throws before Initialize() to prevent exposing a value
	// that would later change once the slot is loaded from config.
	bool instanceIdThrew = false;
	try {
		(void)id.InstanceId();
	} catch (const std::logic_error &) {
		instanceIdThrew = true;
	}
	TEST_ASSERT(instanceIdThrew,
		"InstanceId() must throw std::logic_error before Initialize()");

	std::cout << "  PASS: test_runtime_id_available_before_init" << std::endl;
}

// B. Empty slot - no slot configured.
static void test_empty_slot_id() {
	OpenWifi::InstanceIdentity id(kUUID1);
	bool initResult = id.Initialize("");

	TEST_ASSERT(initResult, "First Initialize() call must succeed");
	TEST_ASSERT(id.IsInitialized(), "Identity must be marked initialized");
	TEST_ASSERT(id.RuntimeIncarnationId() == kUUID1,
		"RuntimeIncarnationId must match the UUID passed at construction");
	TEST_ASSERT(id.SlotId().empty(),
		"SlotId must be empty when not configured");
	TEST_ASSERT(id.InstanceId() == kUUID1,
		"InstanceId must equal RuntimeIncarnationId when slot is empty");

	std::cout << "  PASS: test_empty_slot_id" << std::endl;
}

// C. Configured slot.
static void test_configured_slot_id() {
	OpenWifi::InstanceIdentity id(kUUID1);
	bool initResult = id.Initialize("owprov-1");

	TEST_ASSERT(initResult, "First Initialize() call must succeed");
	TEST_ASSERT(id.IsInitialized(), "Identity must be marked initialized");
	TEST_ASSERT(id.SlotId() == "owprov-1",
		"SlotId must equal configured value");
	TEST_ASSERT(id.RuntimeIncarnationId() == kUUID1,
		"RuntimeIncarnationId must remain unchanged");
	TEST_ASSERT(id.InstanceId() == "owprov-1-" + kUUID1,
		"InstanceId must be <slot-id>-<runtime-incarnation-id>");

	std::cout << "  PASS: test_configured_slot_id" << std::endl;
}

// D. Stable reads.
static void test_read_stability() {
	OpenWifi::InstanceIdentity id(kUUID1);
	id.Initialize("owprov-1");

	const std::string r1 = id.InstanceId();
	const std::string r2 = id.InstanceId();
	const std::string r3 = id.InstanceId();
	TEST_ASSERT(r1 == r2 && r2 == r3,
		"Multiple reads must return the same InstanceId value");

	const std::string s1 = id.SlotId();
	const std::string s2 = id.SlotId();
	TEST_ASSERT(s1 == s2, "Multiple reads must return the same SlotId value");

	const std::string u1 = id.RuntimeIncarnationId();
	const std::string u2 = id.RuntimeIncarnationId();
	TEST_ASSERT(u1 == u2, "Multiple reads must return the same RuntimeIncarnationId value");

	std::cout << "  PASS: test_read_stability" << std::endl;
}

// E. Different runtime incarnations produce different InstanceIds.
static void test_different_incarnations_differ() {
	OpenWifi::InstanceIdentity id1(kUUID1);
	id1.Initialize("owprov-1");

	OpenWifi::InstanceIdentity id2(kUUID2);
	id2.Initialize("owprov-1");

	TEST_ASSERT(id1.RuntimeIncarnationId() != id2.RuntimeIncarnationId(),
		"Different incarnation UUIDs must produce different RuntimeIncarnationIds");
	TEST_ASSERT(id1.InstanceId() != id2.InstanceId(),
		"Different incarnation UUIDs must produce different InstanceIds");

	std::cout << "  PASS: test_different_incarnations_differ" << std::endl;
}

// F. Initialization is one-time.
static void test_initialization_is_one_time() {
	OpenWifi::InstanceIdentity id(kUUID1);

	bool firstInit = id.Initialize("owprov-1");
	TEST_ASSERT(firstInit, "Initial Initialize() call must succeed");

	bool secondInit = id.Initialize("owprov-2");
	TEST_ASSERT(!secondInit, "Subsequent Initialize() call must return false");

	TEST_ASSERT(id.SlotId() == "owprov-1",
		"SlotId must remain owprov-1 after attempted reinitialization");
	TEST_ASSERT(id.RuntimeIncarnationId() == kUUID1,
		"RuntimeIncarnationId must remain unchanged");
	TEST_ASSERT(id.InstanceId() == "owprov-1-" + kUUID1,
		"InstanceId must remain owprov-1-<UUID>");

	std::cout << "  PASS: test_initialization_is_one_time" << std::endl;
}

// G. Empty slot cannot later be replaced.
static void test_empty_slot_cannot_be_replaced() {
	OpenWifi::InstanceIdentity id(kUUID1);

	bool firstInit = id.Initialize("");
	TEST_ASSERT(firstInit, "Initial Initialize('') call must succeed");

	bool secondInit = id.Initialize("owprov-1");
	TEST_ASSERT(!secondInit, "Subsequent Initialize() call must return false");

	TEST_ASSERT(id.SlotId().empty(), "SlotId must remain empty");
	TEST_ASSERT(id.InstanceId() == kUUID1,
		"InstanceId must remain RuntimeIncarnationId");

	std::cout << "  PASS: test_empty_slot_cannot_be_replaced" << std::endl;
}

// H. Strict runtime UUID validation at construction.
static void test_runtime_uuid_validation() {
	const std::string InvalidUUIDs[] = {
		"",                                          // empty
		"not-a-uuid",                                // non-uuid string
		"a1b2c3d4-e5f6-4a1b-9c2d-3e4f5a6b7c8",      // 35 chars (too short)
		"a1b2c3d4-e5f6-4a1b-9c2d-3e4f5a6b7c8de",    // 37 chars (too long)
		"a1b2c3d4e5f64a1b9c2d3e4f5a6b7c8d",         // 32 chars without hyphens
		"a1b2c3d4-e5f6-4a1b-9c2d-3e4f5a6b7c8g",     // invalid hex digit 'g'
		"a1b2c3d4-e5f6-1a1b-9c2d-3e4f5a6b7c8d",     // version 1 instead of 4
		"a1b2c3d4-e5f6-4a1b-0c2d-3e4f5a6b7c8d",     // variant 0 instead of RFC 4122
	};

	for (const auto &BadUUID : InvalidUUIDs) {
		bool constructorThrew = false;
		try {
			OpenWifi::InstanceIdentity id(BadUUID);
		} catch (const std::invalid_argument &) {
			constructorThrew = true;
		}
		TEST_ASSERT(constructorThrew,
			"InstanceIdentity constructor must reject invalid runtime UUID: '" + BadUUID + "'");
	}

	std::cout << "  PASS: test_runtime_uuid_validation" << std::endl;
}

// I. Slot ID whitespace trimming.
static void test_slot_id_is_trimmed() {
	// Leading and trailing whitespace must be stripped.
	OpenWifi::InstanceIdentity id(kUUID1);
	id.Initialize("  owprov-1  ");

	TEST_ASSERT(id.SlotId() == "owprov-1",
		"Slot ID must be trimmed of surrounding whitespace");
	TEST_ASSERT(id.InstanceId() == "owprov-1-" + kUUID1,
		"InstanceId must use trimmed slot");

	std::cout << "  PASS: test_slot_id_is_trimmed" << std::endl;
}

// J. All-whitespace slot normalizes to empty (no slot).
static void test_all_whitespace_slot_becomes_empty() {
	OpenWifi::InstanceIdentity id(kUUID1);
	id.Initialize("   \t  ");

	TEST_ASSERT(id.SlotId().empty(),
		"All-whitespace slot must normalize to empty");
	TEST_ASSERT(id.InstanceId() == kUUID1,
		"InstanceId must equal RuntimeIncarnationId when slot normalizes to empty");

	std::cout << "  PASS: test_all_whitespace_slot_becomes_empty" << std::endl;
}

// K. Valid slot characters - letters, digits, dot, underscore, hyphen.
static void test_valid_slot_characters() {
	const std::string ValidSlots[] = {
		"owprov-1",
		"OWPROV_EAST.2",
		"slot.0-a",
		"abc123",
		"A-Z.a_z-0.9",
	};
	for (const auto &Slot : ValidSlots) {
		OpenWifi::InstanceIdentity id(kUUID1);
		bool ok = false;
		try {
			id.Initialize(Slot);
			ok = true;
		} catch (...) {}
		TEST_ASSERT(ok, "Valid slot '" + Slot + "' must be accepted");
	}
	std::cout << "  PASS: test_valid_slot_characters" << std::endl;
}

// L. Invalid slot characters are rejected.
static void test_invalid_slot_characters_rejected() {
	const std::string InvalidSlots[] = {
		"owprov 1",    // space in middle
		"owprov/1",    // slash
		"owprov:1",    // colon
		"owprov@1",    // at-sign
	};
	for (const auto &Slot : InvalidSlots) {
		bool threw = false;
		try {
			OpenWifi::InstanceIdentity::ValidateSlotId(Slot);
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		TEST_ASSERT(threw, "Invalid slot '" + Slot + "' must be rejected");
	}
	std::cout << "  PASS: test_invalid_slot_characters_rejected" << std::endl;
}

// M. InstanceIdentity is immutable after first initialization,
// even if a later value would be invalid.
static void test_invalid_slot_after_init_leaves_identity_unchanged() {
	OpenWifi::InstanceIdentity id(kUUID1);
	id.Initialize("owprov-1");

	// Confirm ValidateSlotId rejects the bad value that a reload might supply.
	bool threw = false;
	try {
		OpenWifi::InstanceIdentity::ValidateSlotId("owprov 1/bad");
	} catch (const std::invalid_argument &) {
		threw = true;
	}
	TEST_ASSERT(threw, "ValidateSlotId must reject the bad reload value");

	// Initialize() is a no-op after the first call regardless of the argument.
	bool secondInit = id.Initialize("owprov 1/bad");
	TEST_ASSERT(!secondInit, "Initialize() must be a no-op after first call");

	// Stored identity must be completely unchanged.
	TEST_ASSERT(id.SlotId() == "owprov-1",
		"SlotId must remain owprov-1 after attempted invalid slot change");
	TEST_ASSERT(id.InstanceId() == "owprov-1-" + kUUID1,
		"InstanceId must remain owprov-1-<UUID> after attempted invalid slot change");
	TEST_ASSERT(id.RuntimeIncarnationId() == kUUID1,
		"RuntimeIncarnationId must be unchanged");

	std::cout << "  PASS: test_invalid_slot_after_init_leaves_identity_unchanged" << std::endl;
}

// N. Real generator path generates valid, non-empty, unique UUID strings.
static void test_real_uuid_generation_path() {
	std::string uuid1 = OpenWifi::InstanceIdentity::GenerateUUID();
	std::string uuid2 = OpenWifi::InstanceIdentity::GenerateUUID();

	TEST_ASSERT(!uuid1.empty(), "Generated UUID must not be empty");
	TEST_ASSERT(uuid1.size() == 36, "Generated UUID length must be 36");
	TEST_ASSERT(OpenWifi::InstanceIdentity::IsValidUUID(uuid1),
		"Generated UUID must satisfy strict UUID format");
	TEST_ASSERT(OpenWifi::InstanceIdentity::IsValidUUID(uuid2),
		"Second generated UUID must satisfy strict UUID format");
	TEST_ASSERT(uuid1 != uuid2,
		"Successive UUID generations must produce different IDs");

	// Verify an InstanceIdentity constructed with the generated UUID initializes correctly.
	OpenWifi::InstanceIdentity id(uuid1);
	TEST_ASSERT(id.RuntimeIncarnationId() == uuid1,
		"RuntimeIncarnationId must equal generated UUID");
	bool initOk = id.Initialize("owprov-1");
	TEST_ASSERT(initOk, "Initialize() must succeed with real generated UUID");
	TEST_ASSERT(id.InstanceId() == "owprov-1-" + uuid1,
		"Effective runtime instance ID must be <slot>-<generated-uuid>");

	std::cout << "  PASS: test_real_uuid_generation_path" << std::endl;
}

// O. Configuration reload behavior: changed openwifi.system.slot.id is ignored,
// produces restart-required warning, and leaves identity unchanged.
static void test_config_reload_behavior() {
	OpenWifi::InstanceIdentity id(kUUID1);
	id.Initialize("owprov-1");

	TEST_ASSERT(id.SlotId() == "owprov-1", "Initial SlotId must be owprov-1");
	TEST_ASSERT(id.RuntimeIncarnationId() == kUUID1, "RuntimeIncarnationId must be kUUID1");
	TEST_ASSERT(id.InstanceId() == "owprov-1-" + kUUID1, "InstanceId must be owprov-1-<UUID>");

	// Reload with a different slot: must be ignored and produce restart-required warning.
	std::string warnMsg;
	auto res1 = id.HandleConfigReload("owprov-2", &warnMsg);
	TEST_ASSERT(res1 == OpenWifi::InstanceIdentity::ReloadResult::SlotChangedIgnored,
		"Reload with different slot must be SlotChangedIgnored");
	TEST_ASSERT(warnMsg.find("openwifi.system.slot.id changed from 'owprov-1' to 'owprov-2' in config reload - ignored. Changing slot identity requires a process restart.") != std::string::npos,
		"Warning message must indicate slot change requires process restart");
	TEST_ASSERT(id.SlotId() == "owprov-1", "SlotId must remain owprov-1 after reload");
	TEST_ASSERT(id.RuntimeIncarnationId() == kUUID1, "RuntimeIncarnationId must remain unchanged");
	TEST_ASSERT(id.InstanceId() == "owprov-1-" + kUUID1, "InstanceId must remain unchanged");

	// Reload with an invalid slot: must be ignored and produce invalid warning without throwing.
	warnMsg.clear();
	auto res2 = id.HandleConfigReload("owprov/invalid!slot", &warnMsg);
	TEST_ASSERT(res2 == OpenWifi::InstanceIdentity::ReloadResult::InvalidSlotIgnored,
		"Reload with invalid slot must be InvalidSlotIgnored");
	TEST_ASSERT(warnMsg.find("openwifi.system.slot.id has an invalid value in config reload - ignored") != std::string::npos,
		"Warning message must indicate invalid value was ignored");
	TEST_ASSERT(id.SlotId() == "owprov-1", "SlotId must remain unchanged after invalid reload");
	TEST_ASSERT(id.InstanceId() == "owprov-1-" + kUUID1, "InstanceId must remain unchanged after invalid reload");

	// Reload with the same slot: unchanged.
	warnMsg.clear();
	auto res3 = id.HandleConfigReload("owprov-1", &warnMsg);
	TEST_ASSERT(res3 == OpenWifi::InstanceIdentity::ReloadResult::Unchanged,
		"Reload with same slot must return Unchanged");
	TEST_ASSERT(warnMsg.empty(), "No warning message should be set for unchanged slot");
	TEST_ASSERT(id.SlotId() == "owprov-1", "SlotId must remain owprov-1");
	TEST_ASSERT(id.InstanceId() == "owprov-1-" + kUUID1, "InstanceId must remain owprov-1-<UUID>");

	std::cout << "  PASS: test_config_reload_behavior" << std::endl;
}

// P. Invalid slot at initialization falls back to unslotted identity (no slot) with warning.
static void test_invalid_slot_at_init_falls_back_to_unslotted() {
	OpenWifi::InstanceIdentity id(kUUID1);
	std::string warnMsg;
	bool ok = id.Initialize("owprov:1/bad", &warnMsg);

	TEST_ASSERT(ok, "Initialize() must succeed even with unsupported slot formatting");
	TEST_ASSERT(id.SlotId().empty(), "SlotId must fall back to empty");
	TEST_ASSERT(id.InstanceId() == kUUID1,
		"InstanceId must fall back to unslotted RuntimeIncarnationId");
	TEST_ASSERT(!warnMsg.empty(), "Warning message must be populated on invalid slot formatting");
	TEST_ASSERT(warnMsg.find("openwifi.system.slot.id has an invalid format") != std::string::npos,
		"Warning message must explain invalid format");

	std::cout << "  PASS: test_invalid_slot_at_init_falls_back_to_unslotted" << std::endl;
}

#include "Poco/Util/PropertyFileConfiguration.h"
#include <sstream>

// Q. Poco PropertyFileConfiguration loading and reload simulation (matching MicroService paths).
static void test_poco_property_file_loading_and_reload() {
	std::string config1 = "openwifi.system.slot.id = owprov-1\nopenwifi.system.debug = false\n";
	std::istringstream is1(config1);
	Poco::AutoPtr<Poco::Util::PropertyFileConfiguration> pConfig1(
		new Poco::Util::PropertyFileConfiguration(is1));

	OpenWifi::InstanceIdentity id(kUUID1);
	std::string warnMsg;
	std::string slotFromConfig = pConfig1->getString("openwifi.system.slot.id", "");
	bool initOk = id.Initialize(slotFromConfig, &warnMsg);

	TEST_ASSERT(initOk, "Initialize from Poco config must succeed");
	TEST_ASSERT(warnMsg.empty(), "No warning on valid slot");
	TEST_ASSERT(id.SlotId() == "owprov-1", "SlotId must be owprov-1");
	TEST_ASSERT(id.InstanceId() == "owprov-1-" + kUUID1,
		"InstanceId must be owprov-1-<UUID>");

	// Reload with different slot: identity must remain locked, warning produced
	std::string config2 = "openwifi.system.slot.id = owprov-2\n";
	std::istringstream is2(config2);
	Poco::AutoPtr<Poco::Util::PropertyFileConfiguration> pConfig2(
		new Poco::Util::PropertyFileConfiguration(is2));

	warnMsg.clear();
	std::string reloadedSlot = pConfig2->getString("openwifi.system.slot.id", "");
	auto reloadRes = id.HandleConfigReload(reloadedSlot, &warnMsg);

	TEST_ASSERT(reloadRes == OpenWifi::InstanceIdentity::ReloadResult::SlotChangedIgnored,
		"Reload with different slot must be SlotChangedIgnored");
	TEST_ASSERT(!warnMsg.empty(), "Warning message must be set for slot change");
	TEST_ASSERT(id.SlotId() == "owprov-1", "SlotId must remain owprov-1 across reload");
	TEST_ASSERT(id.InstanceId() == "owprov-1-" + kUUID1,
		"InstanceId must remain owprov-1-<UUID> across reload");

	std::cout << "  PASS: test_poco_property_file_loading_and_reload" << std::endl;
}

// R. Verify safe accessor pattern (matching MicroService accessor contract).
static void test_safe_accessor_fallback_semantics() {
	OpenWifi::InstanceIdentity id(kUUID1);

	// Before init: SlotId() is empty, RuntimeIncarnationId() is valid
	TEST_ASSERT(id.SlotId().empty(), "SlotId must be empty before init");
	TEST_ASSERT(id.RuntimeIncarnationId() == kUUID1, "RuntimeIncarnationId is available before init");

	// Safe read simulation: if uninitialized, safe accessor returns RuntimeIncarnationId
	const std::string &effectivePre = id.IsInitialized() ? id.InstanceId() : id.RuntimeIncarnationId();
	TEST_ASSERT(effectivePre == kUUID1, "Safe accessor fallback must return RuntimeIncarnationId");

	// Initialize
	id.Initialize("owprov-1");
	const std::string &effectivePost = id.IsInitialized() ? id.InstanceId() : id.RuntimeIncarnationId();
	TEST_ASSERT(effectivePost == "owprov-1-" + kUUID1, "Safe accessor must return composite ID post-init");

	std::cout << "  PASS: test_safe_accessor_fallback_semantics" << std::endl;
}

int main() {
	std::cout << "InstanceIdentity tests:" << std::endl;

	test_runtime_id_available_before_init();
	test_empty_slot_id();
	test_configured_slot_id();
	test_read_stability();
	test_different_incarnations_differ();
	test_initialization_is_one_time();
	test_empty_slot_cannot_be_replaced();
	test_runtime_uuid_validation();
	test_slot_id_is_trimmed();
	test_all_whitespace_slot_becomes_empty();
	test_valid_slot_characters();
	test_invalid_slot_characters_rejected();
	test_invalid_slot_after_init_leaves_identity_unchanged();
	test_invalid_slot_at_init_falls_back_to_unslotted();
	test_real_uuid_generation_path();
	test_config_reload_behavior();
	test_poco_property_file_loading_and_reload();
	test_safe_accessor_fallback_semantics();

	std::cout << "All tests passed." << std::endl;
	return 0;
}
