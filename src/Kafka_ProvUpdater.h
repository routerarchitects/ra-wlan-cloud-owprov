//
// Created by stephane bourque on 2022-04-01.
//

#pragma once

#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "framework/KafkaManager.h"
#include "framework/KafkaTopics.h"
#include "fmt/format.h"

namespace OpenWifi {

	enum ProvisioningOperation { creation = 0, modification, removal };

	template <typename ObjectType>
	inline bool UpdateKafkaProvisioningObject(ProvisioningOperation op, const ObjectType &obj) {
		static const std::vector<std::string> Ops{"creation", "modification", "removal"};

		if (static_cast<std::size_t>(op) >= Ops.size()) {
			return false;
		}

		std::string OT{"object"};
		if constexpr (std::is_same_v<ObjectType, ProvObjects::Venue>) {
			OT = "Venue";
		}
		if constexpr (std::is_same_v<ObjectType, ProvObjects::Entity>) {
			OT = "Entity";
		}
		if constexpr (std::is_same_v<ObjectType, ProvObjects::InventoryTag>) {
			OT = "InventoryTag";
		}
		if constexpr (std::is_same_v<ObjectType, ProvObjects::Contact>) {
			OT = "Contact";
		}
		if constexpr (std::is_same_v<ObjectType, ProvObjects::Location>) {
			OT = "Location";
		}
		if constexpr (std::is_same_v<ObjectType, ProvObjects::DeviceConfiguration>) {
			OT = "DeviceConfiguration";
		}

		// Key-based partitioning contract requires a valid, stable object ID.
		// Reject publishing before serialization when the object identity is missing or empty
		// to prevent incorrectly keyed events or partition collisions.
		if (obj.info.id.empty()) {
			poco_warning(KafkaManager()->Logger(),
						 fmt::format("Cannot publish Kafka provisioning change for {}: missing object ID", OT));
			return false;
		}

		Poco::JSON::Object Payload;
		obj.to_json(Payload);
		Payload.set("ObjectType", OT);
		Payload.set("operation", Ops[op]);

		std::string Key = fmt::format("{}:{}", OT, obj.info.id);
		KafkaManager()->PostMessage(KafkaTopics::PROVISIONING_CHANGE, Key, Payload, true,
									KAFKA_PARTITION_UNASSIGNED);

		return true;
	}
} // namespace OpenWifi