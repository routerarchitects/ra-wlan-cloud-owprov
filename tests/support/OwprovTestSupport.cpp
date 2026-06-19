#include "support/OwprovTestSupport.h"
#include "framework/EventBusManager.h"
#include "framework/RESTAPI_Handler.h"
#include "framework/AuthClient.h"
#include "Signup.h"
#include "sdks/SDK_sec.h"
#include "sdks/SDK_gw.h"
#include "Poco/UUIDGenerator.h"
#include <ctime>

namespace OpenWifi {

	std::string g_TestDataDir = "/tmp";

	// MicroServiceFuncs stubs
	const std::string &MicroServiceDataDirectory() {
		return g_TestDataDir;
	}
	std::string MicroServiceConfigGetString(const std::string &Key,
											const std::string &DefaultValue) {
		if (Key == "storage.type") return "sqlite";
		if (Key == "storage.type.sqlite.db") return "owprov_test_db";
		if (Key == "firmware.updater.upgrade") return "yes";
		if (Key == "firmware.updater.releaseonly") return "yes";
		if (Key == "rrm.default") return "no";
		return DefaultValue;
	}
	bool MicroServiceConfigGetBool(const std::string &, bool DefaultValue) {
		return DefaultValue;
	}
	std::uint64_t MicroServiceConfigGetInt(const std::string &Key, std::uint64_t DefaultValue) {
		if (Key == "storage.type.sqlite.maxsessions") return 4;
		if (Key == "storage.type.sqlite.idletime") return 60;
		return DefaultValue;
	}
	std::string MicroServiceCreateUUID() {
		return Poco::UUIDGenerator::defaultGenerator().createRandom().toString();
	}
	std::string MicroServicePrivateEndPoint() { return ""; }
	std::string MicroServicePublicEndPoint() { return ""; }
	std::uint64_t MicroServiceID() { return 1; }
	bool MicroServiceIsValidAPIKEY(const Poco::Net::HTTPServerRequest &) { return false; }
	bool MicroServiceNoAPISecurity() { return false; }
	void MicroServiceLoadConfigurationFile() {}
	void MicroServiceReload() {}
	void MicroServiceReload(const std::string &) {}
	Types::StringVec MicroServiceGetLogLevelNames() { return {}; }
	Types::StringVec MicroServiceGetSubSystems() { return {}; }
	Types::StringPairVec MicroServiceGetLogLevels() { return {}; }
	bool MicroServiceSetSubsystemLogLevel(const std::string &, const std::string &) { return false; }
	void MicroServiceGetExtraConfiguration(Poco::JSON::Object &) {}
	std::string MicroServiceVersion() { return "test"; }
	std::uint64_t MicroServiceUptimeTotalSeconds() { return 0; }
	std::uint64_t MicroServiceStartTimeEpochTime() { return 0; }
	std::string MicroServiceGetUIURI() { return ""; }
	SubSystemVec MicroServiceGetFullSubSystems() { return {}; }
	std::uint64_t MicroServiceDaemonBusTimer() { return 0; }
	std::string MicroServiceMakeSystemEventMessage(const char *) { return ""; }
	std::string MicroServiceConfigPath(const std::string &, const std::string &DefaultValue) { return DefaultValue; }
	std::string MicroServiceWWWAssetsDir() { return ""; }
	std::uint64_t MicroServiceRandom(std::uint64_t, std::uint64_t End) { return End; }
	std::uint64_t MicroServiceRandom(std::uint64_t Range) { return Range; }
	std::string MicroServiceSign(Poco::JWT::Token &, const std::string &) { return ""; }
	std::string MicroServiceGetPublicAPIEndPoint() { return ""; }
	void MicroServiceDeleteOverrideConfiguration() {}
	bool AllowExternalMicroServices() { return false; }
	void MicroServiceALBCallback(std::string (*)()) {}
	Types::MicroServiceMetaVec MicroServiceGetServices(const std::string &) { return {}; }
	Types::MicroServiceMetaVec MicroServiceGetServices() { return {}; }
	std::string MicroServiceAccessKey() { return ""; }
	std::optional<OpenWifi::Types::MicroServiceMeta> MicroServicePrivateAccessKey(const std::string &) { return std::nullopt; }

	static Poco::ThreadPool *g_TestTimerPool = nullptr;
	Poco::ThreadPool &MicroServiceTimerPool() {
		if (!g_TestTimerPool) {
			g_TestTimerPool = new Poco::ThreadPool(1, 4);
		}
		return *g_TestTimerPool;
	}

	// EventBusManager stubs
	void EventBusManager::run() {}

	// SDK_sec stubs
	namespace SDK::Sec::Subscriber {
		bool Get(RESTAPIHandler *, const std::string &, SecurityObjects::UserInfo &) {
			return false;
		}
		bool Delete(RESTAPIHandler *, const std::string &) {
			return false;
		}
	}

	// SDK::GW stubs
	namespace SDK::GW::Device {
		bool SetVenue([[maybe_unused]] RESTAPIHandler *client, [[maybe_unused]] const std::string &SerialNumber, [[maybe_unused]] const std::string &uuid) {
			return true;
		}
		bool SetOwnerShip([[maybe_unused]] RESTAPIHandler *client, [[maybe_unused]] const std::string &SerialNumber, [[maybe_unused]] const std::string &owner, [[maybe_unused]] const std::string &venue, [[maybe_unused]] const std::string &subscriber) {
			return true;
		}
	}

	// Signup stubs
	int Signup::Start() { return 0; }
	void Signup::Stop() {}
	void Signup::run() {}
	void Signup::onTimer(Poco::Timer &) {}

	// Utility function
	uint64_t Now() {
		return static_cast<uint64_t>(std::time(nullptr));
	}

	// AuthClient stubs
	bool AuthClient::IsAuthorized(const std::string &,
					  SecurityObjects::UserInfoAndPolicy &, std::uint64_t,
					  bool &, bool &, bool) {
		return true;
	}

	bool AuthClient::IsValidApiKey(const std::string &,
					   SecurityObjects::UserInfoAndPolicy &, std::uint64_t,
					   bool &, bool &, bool &) {
		return true;
	}

	bool AuthClient::RetrieveTokenInformation(const std::string &,
								  SecurityObjects::UserInfoAndPolicy &, std::uint64_t,
								  bool &, bool &, bool) {
		return true;
	}

	bool AuthClient::RetrieveApiKeyInformation(const std::string &,
								   SecurityObjects::UserInfoAndPolicy &, std::uint64_t,
								   bool &, bool &, bool &) {
		return true;
	}

} // namespace OpenWifi
