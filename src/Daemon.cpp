/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include "Poco/Environment.h"
#include "Poco/Net/SSLManager.h"
#include "Poco/Util/Application.h"
#include "Poco/Util/Option.h"

#include "AutoDiscovery.h"
#ifdef CGW_INTEGRATION
#include "SubscriberEvents.h"
#endif
#include "Daemon.h"
#include "DeviceTypeCache.h"
#include "FileDownloader.h"
#include "FindCountry.h"
#include "JobController.h"
#include "SerialNumberCache.h"
#include "Signup.h"
#include "StorageService.h"
#include "UI_Prov_WebSocketNotifications.h"
#include "framework/ConfigurationValidator.h"
#include "framework/UI_WebSocketClientServer.h"
#include <RadiusEndpointTypes/GlobalReach.h>
#include <RadiusEndpointTypes/OrionWifi.h>
#include <RadiusEndpointTypes/Radsec.h>
#include <RadiusEndpointTypes/GenericRadius.h>

namespace OpenWifi {
	class Daemon *Daemon::instance_ = nullptr;

	class Daemon *Daemon::instance() {
		if (instance_ == nullptr) {
			instance_ = new Daemon(vDAEMON_PROPERTIES_FILENAME, vDAEMON_ROOT_ENV_VAR,
								   vDAEMON_CONFIG_ENV_VAR, vDAEMON_APP_NAME, vDAEMON_BUS_TIMER,
								   SubSystemVec{OpenWifi::StorageService(), DeviceTypeCache(),
												ConfigurationValidator(), SerialNumberCache(),
												AutoDiscovery(), JobController(),
												UI_WebSocketClientServer(), FindCountryFromIP(),
												Signup(), FileDownloader(),
                                                OpenRoaming_GlobalReach(),
                                                OpenRoaming_Orion(), OpenRoaming_Radsec(),
                                                OpenRoaming_GenericRadius()
// This subsystem service is initialized when the microservice starts.
// It listens to Kafka topic (subscriber_event) and processes messages related to subscriber creation and deletion.
#ifdef CGW_INTEGRATION
                                                , SubscriberEventsProcessor()
#endif
            });
		}
		return instance_;
	}

	void Daemon::PostInitialization([[maybe_unused]] Poco::Util::Application &self) {
		if (MicroService::instance().ConfigGetBool("firmware.updater.upgrade", false)) {
			if (MicroService::instance().ConfigGetBool("firmware.updater.releaseonly", false)) {
				FWRules_ = ProvObjects::upgrade_release_only;
			} else {
				FWRules_ = ProvObjects::upgrade_latest;
			}
		} else {
			FWRules_ = ProvObjects::dont_upgrade;
		}

		WebSocketProcessor_ = std::make_unique<ProvWebSocketClient>(logger());

		AssetDir_ = MicroService::instance().DataDir() + "/wwwassets";
		Poco::File DataDir(AssetDir_);
		if (!DataDir.exists()) {
			try {
				DataDir.createDirectory();
			} catch (const Poco::Exception &E) {
				logger().log(E);
			}
		}
	}

	void DaemonPostInitialization(Poco::Util::Application &self) {
		Daemon()->PostInitialization(self);
		ProvWebSocketNotifications::Register();
	}

} // namespace OpenWifi

int main(int argc, char **argv) {
	int ExitCode;
	try {
		Poco::Net::SSLManager::instance().initializeServer(nullptr, nullptr, nullptr);
		auto App = OpenWifi::Daemon::instance();
		ExitCode = App->run(argc, argv);
		Poco::Net::SSLManager::instance().shutdown();
	} catch (Poco::Exception &exc) {
		ExitCode = Poco::Util::Application::EXIT_SOFTWARE;
		std::cout << exc.displayText() << std::endl;
	} catch (std::exception &exc) {
		ExitCode = Poco::Util::Application::EXIT_TEMPFAIL;
		std::cout << exc.what() << std::endl;
	} catch (...) {
		ExitCode = Poco::Util::Application::EXIT_TEMPFAIL;
		std::cout << "Exception on closure" << std::endl;
	}

	std::cout << "Exitcode: " << ExitCode << std::endl;
	return ExitCode;
}

// end of namespace