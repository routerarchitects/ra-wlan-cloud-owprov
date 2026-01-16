//
// Created by stephane bourque on 2022-02-21.
//

#include "storage_signup.h"

#include "RESTObjects/RESTAPI_SecurityObjects.h"
#include "StorageService.h"
#include "framework/OpenWifiTypes.h"
#include "framework/RESTAPI_utils.h"
#include "framework/utils.h"

#include "Signup.h"
#include "nlohmann/json.hpp"
#include "sdks/SDK_sec.h"

namespace OpenWifi {

	const static ORM::FieldVec SignupDB_Fields{
		// object info
		ORM::Field{"id", 64, true},
		ORM::Field{"name", ORM::FieldType::FT_TEXT},
		ORM::Field{"description", ORM::FieldType::FT_TEXT},
		ORM::Field{"notes", ORM::FieldType::FT_TEXT},
		ORM::Field{"created", ORM::FieldType::FT_BIGINT},
		ORM::Field{"modified", ORM::FieldType::FT_BIGINT},
		ORM::Field{"email", ORM::FieldType::FT_TEXT},
		ORM::Field{"userId", ORM::FieldType::FT_TEXT},
		ORM::Field{"serialNumber", ORM::FieldType::FT_TEXT},
		ORM::Field{"venueId", ORM::FieldType::FT_TEXT},
		ORM::Field{"submitted", ORM::FieldType::FT_BIGINT},
		ORM::Field{"completed", ORM::FieldType::FT_BIGINT},
		ORM::Field{"status", ORM::FieldType::FT_TEXT},
		ORM::Field{"error", ORM::FieldType::FT_BIGINT},
		ORM::Field{"statusCode", ORM::FieldType::FT_BIGINT},
		ORM::Field{"macAddress", ORM::FieldType::FT_TEXT},
		ORM::Field{"deviceID", ORM::FieldType::FT_TEXT},
		ORM::Field{"registrationId", ORM::FieldType::FT_TEXT},
		ORM::Field{"operatorId", ORM::FieldType::FT_TEXT}};

	const static ORM::IndexVec SignupDB_Indexes{
		{std::string("signup5_email_index"),
		 ORM::IndexEntryVec{{std::string("email"), ORM::Indextype::ASC}}}};

	SignupDB::SignupDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L) noexcept
		: DB(T, "signups5", SignupDB_Fields, SignupDB_Indexes, P, L, "sig") {}

	bool SignupDB::GetIncompleteSignups(SignupDB::RecordVec &Signups) {
		try {
			return GetRecords(0, 10000, Signups, " completed=0 and error=0 ");
		} catch (const Poco::Exception &E) {
			Logger().log(E);
		}
		return false;
	}

	void SignupDB::RemoveIncompleteSignups() {
		try {
			Types::StringVec ToDelete, TimedOut;
			uint64_t now = Utils::Now();
			auto F = [&](const SignupDB::RecordName &R) -> bool {
				if (R.completed != 0)
					return true;

				if ((now - R.submitted) > Signup()->LingerPeriod()) {
					ToDelete.emplace_back(R.info.id);
					return true;
				}

				if ((now - R.submitted) > Signup()->GracePeriod()) {
					TimedOut.push_back(R.info.id);
					//  delete this temporary user
					SDK::Sec::Subscriber::Delete(nullptr, R.userId);
					if (R.statusCode == ProvObjects::SignupStatusCodes::SignupWaitingForDevice) {
						ProvObjects::InventoryTag IT;
						if (StorageService()->InventoryDB().GetRecord("serialNumber",
																	  R.serialNumber, IT)) {
							if (IT.devClass.empty() || IT.devClass == "any" ||
								IT.devClass == "subscriber") {
								try {
									auto DeviceStatus = nlohmann::json::parse(IT.state);
									if (DeviceStatus["method"] == "signup" &&
										DeviceStatus["signupUUID"] == R.info.id) {
										DeviceStatus["claimer"] = "";
										DeviceStatus["claimerId"] = "";
										DeviceStatus["signupUUID"] = "";
										IT.subscriber = "";
										IT.info.modified = Utils::Now();
										IT.info.notes.push_back(SecurityObjects::NoteInfo{
											.created = Utils::Now(),
											.createdBy = "signup-bot",
											.note = "Device release from signup process."});
										StorageService()->InventoryDB().UpdateRecord(
											"serialNumber", R.serialNumber, IT);
									}
								} catch (...) {
								}
							}
						}
					}
					return true;
				}
				return true;
			};

			Iterate(F);

			for (const auto &i : ToDelete)
				DeleteRecord("id", i);

			for (const auto &i : TimedOut) {
				SignupDB::RecordName R;
				if (GetRecord("id", i, R)) {
					R.statusCode = ProvObjects::SignupStatusCodes::SignupTimedOut;
					R.status = "timedOut";
					R.info.modified = Utils::Now();
					UpdateRecord("id", i, R);
				}
			}

		} catch (...) {
		}
	}
} // namespace OpenWifi

template <>
void ORM::DB<OpenWifi::SignupDBRecordType, OpenWifi::ProvObjects::SignupEntry>::Convert(
	const OpenWifi::SignupDBRecordType &In, OpenWifi::ProvObjects::SignupEntry &Out) {
	Out.info.id = In.get<0>();
	Out.info.name = In.get<1>();
	Out.info.description = In.get<2>();
	Out.info.notes =
		OpenWifi::RESTAPI_utils::to_object_array<OpenWifi::SecurityObjects::NoteInfo>(In.get<3>());
	Out.info.created = In.get<4>();
	Out.info.modified = In.get<5>();
	Out.email = In.get<6>();
	Out.userId = In.get<7>();
	Out.serialNumber = In.get<8>();
	Out.venueId = In.get<9>();
	Out.submitted = In.get<10>();
	Out.completed = In.get<11>();
	Out.status = In.get<12>();
	Out.error = In.get<13>();
	Out.statusCode = In.get<14>();
	Out.macAddress = In.get<15>();
	Out.deviceID = In.get<16>();
	Out.registrationId = In.get<17>();
	Out.operatorId = In.get<18>();
}

template <>
void ORM::DB<OpenWifi::SignupDBRecordType, OpenWifi::ProvObjects::SignupEntry>::Convert(
	const OpenWifi::ProvObjects::SignupEntry &In, OpenWifi::SignupDBRecordType &Out) {
	Out.set<0>(In.info.id);
	Out.set<1>(In.info.name);
	Out.set<2>(In.info.description);
	Out.set<3>(OpenWifi::RESTAPI_utils::to_string(In.info.notes));
	Out.set<4>(In.info.created);
	Out.set<5>(In.info.modified);
	Out.set<6>(In.email);
	Out.set<7>(In.userId);
	Out.set<8>(In.serialNumber);
	Out.set<9>(In.venueId);
	Out.set<10>(In.submitted);
	Out.set<11>(In.completed);
	Out.set<12>(In.status);
	Out.set<13>(In.error);
	Out.set<14>(In.statusCode);
	Out.set<15>(In.macAddress);
	Out.set<16>(In.deviceID);
	Out.set<17>(In.registrationId);
	Out.set<18>(In.operatorId);
}
