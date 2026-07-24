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

		if (!operatorId.empty() && subscriberId.empty()) {
			std::string operatorEntityId = operatorId;
			ProvObjects::Entity E;
			if (StorageService()->EntityDB().GetRecord("operatorId", operatorId, E)) {
				operatorEntityId = E.info.id;
			} else if (StorageService()->EntityDB().GetRecord("id", operatorId, E)) {
				operatorEntityId = E.info.id;
			}

			std::set<std::string> SubscriberIds;
			std::set<std::string> VenueDeviceSerials;

			std::string VenueWhere = "subscriber!='' and (entity='" + operatorEntityId + "' or entity='" + operatorId + "')";
			VenueDB::RecordVec SubVenues;
			if (StorageService()->VenueDB().GetRecords(0, 10000, SubVenues, VenueWhere)) {
				for (const auto &v : SubVenues) {
					if (!v.subscriber.empty()) {
						SubscriberIds.insert(v.subscriber);
					}
					for (const auto &dev : v.devices) {
						VenueDeviceSerials.insert(dev);
					}
				}
			}

			std::string AllVenueWhere = "subscriber!=''";
			VenueDB::RecordVec AllSubVenues;
			if (StorageService()->VenueDB().GetRecords(0, 10000, AllSubVenues, AllVenueWhere)) {
				for (const auto &v : AllSubVenues) {
					if (v.entity == operatorEntityId || v.entity == operatorId) {
						if (!v.subscriber.empty()) {
							SubscriberIds.insert(v.subscriber);
						}
						for (const auto &dev : v.devices) {
							VenueDeviceSerials.insert(dev);
						}
					}
				}
			}

			std::vector<std::string> Conditions;
			Conditions.push_back(fmt::format("operatorId='{}'", operatorId));
			Conditions.push_back(fmt::format("operatorId='{}'", operatorEntityId));

			for (const auto &subId : SubscriberIds) {
				Conditions.push_back(fmt::format("subscriberId='{}'", subId));
			}
			for (const auto &devSerial : VenueDeviceSerials) {
				Conditions.push_back(fmt::format("serialNumber='{}'", devSerial));
				Conditions.push_back(fmt::format("id='{}'", devSerial));
			}

			std::string FinalWhere;
			for (size_t i = 0; i < Conditions.size(); ++i) {
				if (i > 0) FinalWhere += " or ";
				FinalWhere += "(" + Conditions[i] + ")";
			}

			if (QB_.CountOnly) {
				auto Count = DB_.Count(FinalWhere);
				return ReturnCountOnly(Count);
			}

			if (!QB_.Select.empty()) {
				return ReturnRecordList<SubscriberDeviceDB, ProvObjects::SubscriberDevice>("subscriberDevices", DB_, *this);
			}

			SubscriberDeviceDB::RecordVec Entries;
			DB_.GetRecords(QB_.Offset, QB_.Limit, Entries, FinalWhere);
			return MakeJSONObjectArray("subscriberDevices", Entries, *this);
		}

		return ListHandlerForOperator<SubscriberDeviceDB>("subscriberDevices", DB_, *this,
														  operatorId, subscriberId);
	}

} // namespace OpenWifi