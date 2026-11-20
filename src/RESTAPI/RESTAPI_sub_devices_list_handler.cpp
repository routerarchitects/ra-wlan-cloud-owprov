#include "RESTAPI_sub_devices_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "StorageService.h"
#include "fmt/format.h"
#include <set>
#include <vector>

namespace OpenWifi {

	void RESTAPI_sub_devices_list_handler::DoGet() {
		auto operatorId = ORM::Escape(GetParameter("operatorId"));
		auto subscriberId = ORM::Escape(GetParameter("subscriberId"));

		if (!operatorId.empty() && !StorageService()->OperatorDB().Exists("id", operatorId)) {
			return BadRequest(RESTAPI::Errors::OperatorIdMustExist);
		}

		if (!QB_.Select.empty()) {
			std::vector<ProvObjects::SubscriberDevice> AuthorizedDevices;
			const bool Privileged = UserInfo_.userinfo.userRole == SecurityObjects::ROOT ||
								IsInternalServiceOnly();

			// A non-ROOT collection selection without a resolvable Operator scope must
			// fail closed. RoleIsAuthorized normally rejects it before DoGet(), but keep
			// this invariant local to the data-return path as well.
			if (!Privileged && operatorId.empty()) {
				if (QB_.CountOnly) {
					return ReturnCountOnly(0);
				}
				return MakeJSONObjectArray("subscriberDevices", AuthorizedDevices, *this);
			}

			for (const auto &Id : SelectedRecords()) {
				ProvObjects::SubscriberDevice Device;
				if (!DB_.GetRecord("id", Id, Device)) {
					// Do not distinguish an unknown UUID from an unauthorized one.
					continue;
				}

				// Explicit collection filters narrow the already-authorized RBAC scope;
				// they are never a replacement for the per-record authorization below.
				if ((!operatorId.empty() && Device.operatorId != operatorId) ||
					(!subscriberId.empty() && Device.subscriberId != subscriberId)) {
					continue;
				}

				if (!Privileged) {
					std::string EntityId;
					std::string VenueId;

					// Prefer inventory placement when the subscriber Device is provisioned.
					// This preserves exact Venue-role shadowing for Devices assigned to a Venue.
					ProvObjects::InventoryTag Inventory;
					if (!Device.serialNumber.empty() &&
						StorageService()->InventoryDB().GetRecord(
							RESTAPI::Protocol::SERIALNUMBER, Device.serialNumber, Inventory)) {
						EntityId = Inventory.entity;
						VenueId = Inventory.venue;
						if (EntityId.empty() && !VenueId.empty()) {
							ProvObjects::Venue Venue;
							if (StorageService()->VenueDB().GetRecord("id", VenueId, Venue)) {
								EntityId = Venue.entity.empty() ? FindParentEntity(Venue) : Venue.entity;
							}
						}
					}

					// Unprovisioned subscriber Devices inherit their scope from their Operator.
					if (EntityId.empty()) {
						ProvObjects::Operator Operator;
						ProvObjects::Entity Entity;
						if (StorageService()->OperatorDB().GetRecord("id", Device.operatorId,
															 Operator)) {
							EntityId = Operator.entityId;
						}
						if (EntityId.empty() &&
							(StorageService()->EntityDB().GetRecord("operatorId", Device.operatorId,
															  Entity) ||
							 StorageService()->EntityDB().GetRecord("id", Device.operatorId, Entity))) {
							EntityId = Entity.info.id;
						}
					}

					if (EntityId.empty() ||
						!IsScopeAuthorized(EntityId, VenueId, "subscriberDevice",
										   Poco::Net::HTTPRequest::HTTP_GET)) {
						continue;
					}
				}

				AuthorizedDevices.emplace_back(std::move(Device));
			}

			if (QB_.CountOnly) {
				return ReturnCountOnly(AuthorizedDevices.size());
			}
			return MakeJSONObjectArray("subscriberDevices", AuthorizedDevices, *this);
		}

		return ListHandlerForOperator<SubscriberDeviceDB>("subscriberDevices", DB_, *this,
														  operatorId, subscriberId);
	}

} // namespace OpenWifi
