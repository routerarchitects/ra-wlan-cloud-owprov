/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#pragma once

#ifdef CGW_INTEGRATION

#include <cstdint>

#include "framework/KafkaManager.h"
#include "framework/KafkaTopics.h"
#include "framework/MicroServiceFuncs.h"
#include "framework/OpenWifiTypes.h"

#include "Poco/JSON/Array.h"
#include "Poco/JSON/Object.h"

namespace OpenWifi {

	inline void PrepareInfraGroupEventJson(Poco::JSON::Object &Payload,
										   const std::string &EventType,
										   std::uint64_t GroupId,
										   const Types::StringVec &Infras = {}) {
		Payload.set("type", EventType);
		Payload.set("infra_group_id", std::to_string(GroupId));
		Payload.set("uuid", MicroServiceCreateUUID());

		if (!Infras.empty()) {
			Poco::JSON::Array InfraList;
			for (const auto &infra : Infras) {
				InfraList.add(infra);
			}
			Payload.set("infra_group_infras", InfraList);
		}
	}

	inline void PublishInfraGroupEvent(const std::string &EventType,
									   std::uint64_t GroupId,
									   const Types::StringVec &Infras = {}) {
		Poco::JSON::Object Payload;
		PrepareInfraGroupEventJson(Payload, EventType, GroupId, Infras);
		KafkaManager()->PostMessage(KafkaTopics::CNC, std::to_string(GroupId), Payload,false);
	}

} // namespace OpenWifi

#endif // CGW_INTEGRATION
