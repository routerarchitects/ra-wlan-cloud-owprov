//
// Created by Router Architects on 2026-06-19.
//

#include "RESTAPI_operator_entity_list_handler.h"

#include "RESTAPI/RESTAPI_rbac_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"

namespace OpenWifi {
	namespace {
		void AddOperatorEntity(Poco::JSON::Array &entries, const ProvObjects::Entity &entity,
							   const ProvObjects::Operator &op) {
			Poco::JSON::Object entry;
			entry.set("id", entity.info.id);
			entry.set("entityId", entity.info.id);
			entry.set("entityName", entity.info.name);
			entry.set("parentEntityId", entity.parent);
			entry.set("operatorId", op.info.id);
			entry.set("operatorName", op.info.name);
			entries.add(entry);
		}
	} // namespace

	void RESTAPI_operator_entity_list_handler::DoGet() {
		Poco::JSON::Array operatorEntities;

		StorageService()->EntityDB().Iterate([&](const ProvObjects::Entity &entity) {
			if (entity.operatorId.empty()) {
				return true;
			}

			ProvObjects::Operator op;
			if (!StorageService()->OperatorDB().GetRecord("id", entity.operatorId, op)) {
				return true;
			}

			if (!RBAC::IsRootUser(*this) &&
				!RBAC::HasAccess(*this, "operator", "CREATE",
								 RBAC::TargetScope{entity.info.id, ""})) {
				return true;
			}

			AddOperatorEntity(operatorEntities, entity, op);
			return true;
		});

		if (QB_.CountOnly) {
			return ReturnCountOnly(operatorEntities.size());
		}

		Poco::JSON::Object answer;
		answer.set("operatorEntities", operatorEntities);
		return ReturnObject(answer);
	}
} // namespace OpenWifi
