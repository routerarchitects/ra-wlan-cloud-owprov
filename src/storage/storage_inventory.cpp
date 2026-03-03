//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include "storage_inventory.h"
#include "RESTObjects/RESTAPI_SecurityObjects.h"
#include "SerialNumberCache.h"
#include "StorageService.h"
#include "framework/MicroServiceFuncs.h"
#include "framework/OpenWifiTypes.h"
#include "framework/RESTAPI_utils.h"
#include "framework/utils.h"
#include "nlohmann/json.hpp"
#include "sdks/SDK_gw.h"

namespace OpenWifi {

	static ORM::FieldVec InventoryDB_Fields{
		// object info
		ORM::Field{"id", 64, true},
		ORM::Field{"name", ORM::FieldType::FT_TEXT},
		ORM::Field{"description", ORM::FieldType::FT_TEXT},
		ORM::Field{"notes", ORM::FieldType::FT_TEXT},
		ORM::Field{"created", ORM::FieldType::FT_BIGINT},
		ORM::Field{"modified", ORM::FieldType::FT_BIGINT},
		ORM::Field{"serialNumber", ORM::FieldType::FT_TEXT},
		ORM::Field{"venue", ORM::FieldType::FT_TEXT},
		ORM::Field{"entity", ORM::FieldType::FT_TEXT},
		ORM::Field{"subscriber", ORM::FieldType::FT_TEXT},
		ORM::Field{"deviceType", ORM::FieldType::FT_TEXT},
		ORM::Field{"qrcode", ORM::FieldType::FT_TEXT},
		ORM::Field{"geocode", ORM::FieldType::FT_TEXT},
		ORM::Field{"location", ORM::FieldType::FT_TEXT},
		ORM::Field{"contact", ORM::FieldType::FT_TEXT},
		ORM::Field{"deviceConfiguration", ORM::FieldType::FT_TEXT},
		ORM::Field{"deviceRules", ORM::FieldType::FT_TEXT},
		ORM::Field{"tags", ORM::FieldType::FT_TEXT},
		ORM::Field{"managementPolicy", ORM::FieldType::FT_TEXT},
		ORM::Field{"state", ORM::FieldType::FT_TEXT},
		ORM::Field{"devClass", ORM::FieldType::FT_TEXT},
		ORM::Field{"locale", ORM::FieldType::FT_TEXT},
		ORM::Field{"realMacAddress", ORM::FieldType::FT_TEXT},
		ORM::Field{"doNotAllowOverrides", ORM::FieldType::FT_BOOLEAN},
        ORM::Field{"imported", ORM::FieldType::FT_BIGINT},
        ORM::Field{"connected", ORM::FieldType::FT_BIGINT},
        ORM::Field{"platform", ORM::FieldType::FT_TEXT}};

	static ORM::IndexVec InventoryDB_Indexes{
		{std::string("inventory_name_index"),
		 ORM::IndexEntryVec{{std::string("name"), ORM::Indextype::ASC}}},
		{std::string("inventory_serial_index"),
		 ORM::IndexEntryVec{{std::string("serialNumber"), ORM::Indextype::ASC}}}};

	bool InventoryDB::Upgrade([[maybe_unused]] uint32_t from, uint32_t &to) {
		to = Version();
		std::vector<std::string> Script{
			"alter table " + TableName_ + " add column state text",
			"alter table " + TableName_ + " add column locale varchar(16)",
			"alter table " + TableName_ + " add column realMacAddress text",
			"alter table " + TableName_ + " add column devClass text",
			"alter table " + TableName_ + " add column deviceRules text",
            "alter table " + TableName_ + " add column platform text default 'AP'",
            "alter table " + TableName_ + " add column imported bigint",
            "alter table " + TableName_ + " add column connected bigint",
			"alter table " + TableName_ + " add column doNotAllowOverrides boolean"};

		for (const auto &i : Script) {
			try {
				auto Session = Pool_.get();
				Session << i, Poco::Data::Keywords::now;
			} catch (...) {
			}
		}
		return true;
	}

#define __DBG__ std::cout << __FILE__ << ": " << __LINE__ << std::endl;

	InventoryDB::InventoryDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L)
		: DB(T, "inventory", InventoryDB_Fields, InventoryDB_Indexes, P, L, "inv") {}

	bool InventoryDB::CreateFromConnection(const std::string &SerialNumberRaw,
										   const std::string &ConnectionInfo,
										   const std::string &DeviceType,
										   const std::string &Locale,
										   const bool isConnection) {

		ProvObjects::InventoryTag ExistingDevice;
		auto SerialNumber = Poco::toLower(SerialNumberRaw);
		if (!GetRecord("serialNumber", SerialNumber, ExistingDevice)) {
            ProvObjects::InventoryTag NewDevice;
			uint64_t Now = Utils::Now();

			auto Tokens = Poco::StringTokenizer(ConnectionInfo, "@:");
			std::string IP;
			if (Tokens.count() == 3) {
				IP = Tokens[1];
			}
			NewDevice.info.id = MicroServiceCreateUUID();
			NewDevice.info.name = SerialNumber;
			NewDevice.info.created = NewDevice.info.modified = Now;
			NewDevice.info.notes.push_back(SecurityObjects::NoteInfo{
				.created = Now, .createdBy = "*system", .note = "Auto discovered"});
			NewDevice.serialNumber = SerialNumber;
			NewDevice.deviceType = DeviceType;
			NewDevice.locale = Locale;
			nlohmann::json StateDoc;
			StateDoc["method"] = "auto-discovery";
			StateDoc["date"] = Utils::Now();
			NewDevice.state = to_string(StateDoc);
			NewDevice.devClass = "any";
            NewDevice.connected = Now;
            NewDevice.imported = 0;
			if (!IP.empty()) {
				StorageService()->VenueDB().GetByIP(IP, NewDevice.venue);
				if (NewDevice.venue.empty()) {
					StorageService()->EntityDB().GetByIP(IP, NewDevice.entity);
				}
			}

			if (CreateRecord(NewDevice)) {
				SerialNumberCache()->AddSerialNumber(SerialNumber, DeviceType);
				std::string FullUUID;
				if (!NewDevice.entity.empty()) {
					StorageService()->EntityDB().AddDevice("id", NewDevice.entity,
														   NewDevice.info.id);
					FullUUID = StorageService()->EntityDB().Prefix() + ":" + NewDevice.entity;
				} else if (!NewDevice.venue.empty()) {
					StorageService()->VenueDB().AddDevice("id", NewDevice.venue, NewDevice.info.id);
					FullUUID = StorageService()->VenueDB().Prefix() + ":" + NewDevice.venue;
				}

				if (!FullUUID.empty()) {
					if (SDK::GW::Device::SetVenue(nullptr, NewDevice.serialNumber, FullUUID)) {
						Logger().information(Poco::format("%s: GW set entity/venue property.",
														  NewDevice.serialNumber));
					} else {
						Logger().information(Poco::format(
							"%s: could not set GW entity/venue property.", NewDevice.serialNumber));
					}
				}
				Logger().information(Poco::format("Adding %s to inventory.", SerialNumber));
				return true;
			} else {
				Logger().information(Poco::format("Could not add %s to inventory.", SerialNumber));
			}
		} else {
			//  Device already exists, do we need to modify anything?
			bool modified = false;
			if (ExistingDevice.deviceType != DeviceType) {
				ExistingDevice.deviceType = DeviceType;
				modified = true;
			}

			//  if this device is being claimed, not it is claimed.
			if (!ExistingDevice.state.empty()) {
				auto State = nlohmann::json::parse(ExistingDevice.state);
				if (State["method"] == "claiming") {
					uint64_t Date = State["date"];
					uint64_t Now = Utils::Now();

					if ((Now - Date) < (24 * 60 * 60)) {
						State["method"] = "claimed";
						State["date"] = Utils::Now();
						ExistingDevice.state = to_string(State);
						modified = true;
					} else {
						ExistingDevice.state = "";
						modified = true;
					}
				}
			} else {
				ExistingDevice.devClass = "any";
				modified = true;
			}

			if (Locale != ExistingDevice.locale) {
				ExistingDevice.locale = Locale;
				modified = true;
			}

			if (modified) {
				ExistingDevice.info.modified = Utils::Now();
                ExistingDevice.connected = Utils::Now();
				StorageService()->InventoryDB().UpdateRecord("id", ExistingDevice.info.id,
															 ExistingDevice);
			}

			// Push entity and venue down to GW but only on connect (not ping)
			if (isConnection && !ExistingDevice.venue.empty()) {
				if (SDK::GW::Device::SetVenue(nullptr, ExistingDevice.serialNumber, ExistingDevice.venue)) {
						Logger().information(Poco::format("%s: GW set venue property.",
														  ExistingDevice.serialNumber));
				} else {
					Logger().information(Poco::format(
						"%s: could not set GW venue property.", ExistingDevice.serialNumber));
				}
			}

			if (isConnection && !ExistingDevice.entity.empty()) {
				if (SDK::GW::Device::SetEntity(nullptr, ExistingDevice.serialNumber, ExistingDevice.entity)) {
						Logger().information(Poco::format("%s: GW set entity property.",
														  ExistingDevice.serialNumber));
				} else {
					Logger().information(Poco::format(
						"%s: could not set GW entity property.", ExistingDevice.serialNumber));
				}
			}

		}
		return false;
	}

	bool InventoryDB::EvaluateDeviceIDRules(const std::string &id,
											ProvObjects::DeviceRules &Rules) {
		ProvObjects::InventoryTag T;
		if (GetRecord("id", id, T))
			return EvaluateDeviceRules(T, Rules);
		return false;
	}

	bool InventoryDB::EvaluateDeviceSerialNumberRules(const std::string &serialNumber,
													  ProvObjects::DeviceRules &Rules) {
		ProvObjects::InventoryTag T;
		if (GetRecord("serialNumber", serialNumber, T))
			return EvaluateDeviceRules(T, Rules);
		return false;
	}

	bool InventoryDB::EvaluateDeviceRules(const ProvObjects::InventoryTag &T,
										  ProvObjects::DeviceRules &Rules) {
		Rules = T.deviceRules;
		if (!T.venue.empty())
			return StorageService()->VenueDB().EvaluateDeviceRules(T.venue, Rules);
		if (!T.entity.empty())
			return StorageService()->EntityDB().EvaluateDeviceRules(T.venue, Rules);
		return Storage::ApplyConfigRules(Rules);
	}

	void InventoryDB::InitializeSerialCache() {
		auto F = [](const ProvObjects::InventoryTag &T) -> bool {
			SerialNumberCache()->AddSerialNumber(T.serialNumber, T.deviceType);
			return true;
		};
		Iterate(F);
	}

	bool InventoryDB::GetRRMDeviceList(Types::UUIDvec_t &DeviceList) {
		// get a local copy of the cache - this could be expensive.
		auto C = SerialNumberCache()->GetCacheCopy();

		for (const auto &i : C) {
			ProvObjects::InventoryTag Tag;
			ProvObjects::DeviceRules Rules;
			std::string SerialNumber = Utils::IntToSerialNumber(i);
			if (EvaluateDeviceSerialNumberRules(SerialNumber, Rules)) {
				if (Rules.rrm != "no" && Rules.rrm != "inherit")
					DeviceList.push_back(SerialNumber);
			}
		}
		return true;
	}

    bool InventoryDB::GetDevicesForVenue(const std::string &venue_uuid, std::vector<std::string> &devices) {
        try {
            std::vector<ProvObjects::InventoryTag> device_list;
            if(GetRecords(0, 1000, device_list, fmt::format(" venue='{}' ", venue_uuid))) {
                for(auto &i:device_list) {
                    devices.push_back(i.serialNumber);
                }
                return true;
            }
        } catch(const Poco::Exception &E) {
            Logger().log(E);
            return false;
        } catch(const std::exception &E) {
            Logger().error(fmt::format("std::exception: {}",E.what()));
            return false;
        } catch(...) {
            Logger().error("Unknown exception");
            return false;
        }
        return false;
    }

    bool InventoryDB::GetDevicesUUIDForVenue(const std::string &venue_uuid, std::vector<std::string> &devices) {
        try {
            std::vector<ProvObjects::InventoryTag> device_list;
            if(GetRecords(0, 1000, device_list, fmt::format(" venue='{}' ", venue_uuid))) {
                for(auto &i:device_list) {
                    devices.push_back(i.info.id);
                }
                return true;
            }
        } catch(const Poco::Exception &E) {
            Logger().log(E);
            return false;
        } catch(const std::exception &E) {
            Logger().error(fmt::format("std::exception: {}",E.what()));
            return false;
        } catch(...) {
            Logger().error("Unknown exception");
            return false;
        }
        return false;
    }

    bool InventoryDB::GetDevicesForVenue(const std::string &venue_uuid, std::vector<ProvObjects::InventoryTag> &devices) {
        try {
            return GetRecords(0, 1000, devices, fmt::format(" venue='{}' ", venue_uuid));
        } catch(const Poco::Exception &E) {
            Logger().log(E);
            return false;
        } catch(const std::exception &E) {
            Logger().error(fmt::format("std::exception: {}",E.what()));
            return false;
        } catch(...) {
            Logger().error("Unknown exception");
            return false;
        }

        return false;
    }


} // namespace OpenWifi

template <>
void ORM::DB<OpenWifi::InventoryDBRecordType, OpenWifi::ProvObjects::InventoryTag>::Convert(
	const OpenWifi::InventoryDBRecordType &In, OpenWifi::ProvObjects::InventoryTag &Out) {
	Out.info.id = In.get<0>();
	Out.info.name = In.get<1>();
	Out.info.description = In.get<2>();
	Out.info.notes =
		OpenWifi::RESTAPI_utils::to_object_array<OpenWifi::SecurityObjects::NoteInfo>(In.get<3>());
	Out.info.created = In.get<4>();
	Out.info.modified = In.get<5>();
	Out.serialNumber = In.get<6>();
	Out.venue = In.get<7>();
	Out.entity = In.get<8>();
	Out.subscriber = In.get<9>();
	Out.deviceType = In.get<10>();
	Out.qrCode = In.get<11>();
	Out.geoCode = In.get<12>();
	Out.location = In.get<13>();
	Out.contact = In.get<14>();
	Out.deviceConfiguration = In.get<15>();
	Out.deviceRules =
		OpenWifi::RESTAPI_utils::to_object<OpenWifi::ProvObjects::DeviceRules>(In.get<16>());
	Out.info.tags = OpenWifi::RESTAPI_utils::to_taglist(In.get<17>());
	Out.managementPolicy = In.get<18>();
	Out.state = In.get<19>();
	Out.devClass = In.get<20>();
	Out.locale = In.get<21>();
	Out.realMacAddress = In.get<22>();
	Out.doNotAllowOverrides = In.get<23>();
    Out.imported = In.get<24>();
    Out.connected = In.get<25>();
    Out.platform = In.get<26>();
}

template <>
void ORM::DB<OpenWifi::InventoryDBRecordType, OpenWifi::ProvObjects::InventoryTag>::Convert(
	const OpenWifi::ProvObjects::InventoryTag &In, OpenWifi::InventoryDBRecordType &Out) {
	Out.set<0>(In.info.id);
	Out.set<1>(In.info.name);
	Out.set<2>(In.info.description);
	Out.set<3>(OpenWifi::RESTAPI_utils::to_string(In.info.notes));
	Out.set<4>(In.info.created);
	Out.set<5>(In.info.modified);
	Out.set<6>(In.serialNumber);
	Out.set<7>(In.venue);
	Out.set<8>(In.entity);
	Out.set<9>(In.subscriber);
	Out.set<10>(In.deviceType);
	Out.set<11>(In.qrCode);
	Out.set<12>(In.geoCode);
	Out.set<13>(In.location);
	Out.set<14>(In.contact);
	Out.set<15>(In.deviceConfiguration);
	Out.set<16>(OpenWifi::RESTAPI_utils::to_string(In.deviceRules));
	Out.set<17>(OpenWifi::RESTAPI_utils::to_string(In.info.tags));
	Out.set<18>(In.managementPolicy);
	Out.set<19>(In.state);
	Out.set<20>(In.devClass);
	Out.set<21>(In.locale);
	Out.set<22>(In.realMacAddress);
	Out.set<23>(In.doNotAllowOverrides);
    Out.set<24>(In.imported);
    Out.set<25>(In.connected);
    Out.set<26>(In.platform);
}
