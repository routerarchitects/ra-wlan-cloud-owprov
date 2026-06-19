#pragma once

#include <string>
#include <vector>
#include <optional>
#include "Poco/ThreadPool.h"
#include "Poco/JSON/Object.h"
#include "Poco/Net/HTTPServerRequest.h"
#include "Poco/JWT/Token.h"
#include "framework/OpenWifiTypes.h"
#include "RESTObjects/RESTAPI_SecurityObjects.h"

namespace OpenWifi {
	class SubSystemServer;
	using SubSystemVec = std::vector<SubSystemServer *>;

	extern std::string g_TestDataDir;

	const std::string &MicroServiceDataDirectory();
	std::string MicroServiceConfigGetString(const std::string &Key, const std::string &DefaultValue);
	bool MicroServiceConfigGetBool(const std::string &Key, bool DefaultValue);
	std::uint64_t MicroServiceConfigGetInt(const std::string &Key, std::uint64_t DefaultValue);
	std::string MicroServiceCreateUUID();
	std::string MicroServicePrivateEndPoint();
	std::string MicroServicePublicEndPoint();
	std::uint64_t MicroServiceID();
	bool MicroServiceIsValidAPIKEY(const Poco::Net::HTTPServerRequest &);
	bool MicroServiceNoAPISecurity();
	void MicroServiceLoadConfigurationFile();
	void MicroServiceReload();
	void MicroServiceReload(const std::string &);
	Types::StringVec MicroServiceGetLogLevelNames();
	Types::StringVec MicroServiceGetSubSystems();
	Types::StringPairVec MicroServiceGetLogLevels();
	bool MicroServiceSetSubsystemLogLevel(const std::string &, const std::string &);
	void MicroServiceGetExtraConfiguration(Poco::JSON::Object &);
	std::string MicroServiceVersion();
	std::uint64_t MicroServiceUptimeTotalSeconds();
	std::uint64_t MicroServiceStartTimeEpochTime();
	std::string MicroServiceGetUIURI();
	SubSystemVec MicroServiceGetFullSubSystems();
	std::uint64_t MicroServiceDaemonBusTimer();
	std::string MicroServiceMakeSystemEventMessage(const char *);
	std::string MicroServiceConfigPath(const std::string &, const std::string &DefaultValue);
	std::string MicroServiceWWWAssetsDir();
	std::uint64_t MicroServiceRandom(std::uint64_t, std::uint64_t End);
	std::uint64_t MicroServiceRandom(std::uint64_t Range);
	std::string MicroServiceSign(Poco::JWT::Token &, const std::string &);
	std::string MicroServiceGetPublicAPIEndPoint();
	void MicroServiceDeleteOverrideConfiguration();
	bool AllowExternalMicroServices();
	void MicroServiceALBCallback(std::string (*)());
	Types::MicroServiceMetaVec MicroServiceGetServices(const std::string &);
	Types::MicroServiceMetaVec MicroServiceGetServices();
	std::string MicroServiceAccessKey();
	std::optional<OpenWifi::Types::MicroServiceMeta> MicroServicePrivateAccessKey(const std::string &);

	Poco::ThreadPool &MicroServiceTimerPool();

	uint64_t Now();
}
