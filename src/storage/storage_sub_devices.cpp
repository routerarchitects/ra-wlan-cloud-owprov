//
// Created by stephane bourque on 2022-04-06.
//

#include "storage_sub_devices.h"
#include "RESTObjects/RESTAPI_SecurityObjects.h"
#include "StorageService.h"
#include "Poco/String.h"
#include "framework/OpenWifiTypes.h"
#include "framework/RESTAPI_utils.h"

namespace OpenWifi {

	static ORM::FieldVec SubscriberDeviceDB_Fields{
		// object info
		ORM::Field{"id", 64, true},
		ORM::Field{"name", ORM::FieldType::FT_TEXT},
		ORM::Field{"description", ORM::FieldType::FT_TEXT},
		ORM::Field{"notes", ORM::FieldType::FT_TEXT},
		ORM::Field{"created", ORM::FieldType::FT_BIGINT},
		ORM::Field{"modified", ORM::FieldType::FT_BIGINT},
		ORM::Field{"serialNumber", ORM::FieldType::FT_TEXT},
		ORM::Field{"deviceType", ORM::FieldType::FT_TEXT},
		ORM::Field{"operatorId", ORM::FieldType::FT_TEXT},
		ORM::Field{"subscriberId", ORM::FieldType::FT_TEXT},
		ORM::Field{"location", ORM::FieldType::FT_TEXT},
		ORM::Field{"contact", ORM::FieldType::FT_TEXT},
		ORM::Field{"managementPolicy", ORM::FieldType::FT_TEXT},
		ORM::Field{"serviceClass", ORM::FieldType::FT_TEXT},
		ORM::Field{"qrCode", ORM::FieldType::FT_TEXT},
		ORM::Field{"geoCode", ORM::FieldType::FT_TEXT},
		ORM::Field{"deviceRules", ORM::FieldType::FT_TEXT},
		ORM::Field{"state", ORM::FieldType::FT_TEXT},
		ORM::Field{"locale", ORM::FieldType::FT_TEXT},
		ORM::Field{"billingCode", ORM::FieldType::FT_TEXT},
		ORM::Field{"configuration", ORM::FieldType::FT_TEXT},
		ORM::Field{"suspended", ORM::FieldType::FT_BOOLEAN},
		ORM::Field{"realMacAddress", ORM::FieldType::FT_TEXT},
		ORM::Field{"deviceGroup", ORM::FieldType::FT_TEXT}};

	static ORM::IndexVec SubscriberDeviceDB_Indexes{
		{std::string("subscriber_device_name_index4"),
		 ORM::IndexEntryVec{{std::string("name"), ORM::Indextype::ASC}}},
		{std::string("subscriber_device_serialNumber_index4"),
		 ORM::IndexEntryVec{{std::string("serialNumber"), ORM::Indextype::ASC}}},
		{std::string("subscriber_device_realMacAddress_index4"),
		 ORM::IndexEntryVec{{std::string("realMacAddress"), ORM::Indextype::ASC}}}};

	SubscriberDeviceDB::SubscriberDeviceDB(OpenWifi::DBType T, Poco::Data::SessionPool &P,
										   Poco::Logger &L)
		: DB(T, "sub_devices4", SubscriberDeviceDB_Fields, SubscriberDeviceDB_Indexes, P, L,
			 "sdv") {}

	bool SubscriberDeviceDB::Upgrade([[maybe_unused]] uint32_t from, uint32_t &to) {
		to = Version();
		std::vector<std::string> Script{
			"alter table " + TableName_ + " add column deviceGroup text default 'ap'",
			"alter table " + TableName_ + " drop column if exists gateway"};
		RunScript(Script);
		return true;
	}
} // namespace OpenWifi

template <>
void ORM::DB<OpenWifi::SubDeviceDBRecordType, OpenWifi::ProvObjects::SubscriberDevice>::Convert(
	const OpenWifi::SubDeviceDBRecordType &In, OpenWifi::ProvObjects::SubscriberDevice &Out) {
	Out.info.id = In.get<0>();
	Out.info.name = In.get<1>();
	Out.info.description = In.get<2>();
	Out.info.notes =
		OpenWifi::RESTAPI_utils::to_object_array<OpenWifi::SecurityObjects::NoteInfo>(In.get<3>());
	Out.info.created = In.get<4>();
	Out.info.modified = In.get<5>();
	Out.serialNumber = In.get<6>();
	Out.deviceType = In.get<7>();
	Out.operatorId = In.get<8>();
	Out.subscriberId = In.get<9>();
	Out.location =
		OpenWifi::RESTAPI_utils::to_object<OpenWifi::ProvObjects::SubLocation>(In.get<10>());
	Out.contact =
		OpenWifi::RESTAPI_utils::to_object<OpenWifi::ProvObjects::SubContact>(In.get<11>());
	Out.managementPolicy = In.get<12>();
	Out.serviceClass = In.get<13>();
	Out.qrCode = In.get<14>();
	Out.geoCode = In.get<15>();
	Out.deviceRules =
		OpenWifi::RESTAPI_utils::to_object<OpenWifi::ProvObjects::DeviceRules>(In.get<16>());
	Out.state = In.get<17>();
	Out.locale = In.get<18>();
	Out.billingCode = In.get<19>();
	Out.configurationId = In.get<20>();
	Out.suspended = In.get<21>();
	Out.realMacAddress = In.get<22>();
	Out.deviceGroup = In.get<23>();
	if (Out.deviceGroup.empty()) {
		Out.deviceGroup = "ap";
	} else {
		Poco::toLowerInPlace(Out.deviceGroup);
	}
}

template <>
void ORM::DB<OpenWifi::SubDeviceDBRecordType, OpenWifi::ProvObjects::SubscriberDevice>::Convert(
	const OpenWifi::ProvObjects::SubscriberDevice &In, OpenWifi::SubDeviceDBRecordType &Out) {
	Out.set<0>(In.info.id);
	Out.set<1>(In.info.name);
	Out.set<2>(In.info.description);
	Out.set<3>(OpenWifi::RESTAPI_utils::to_string(In.info.notes));
	Out.set<4>(In.info.created);
	Out.set<5>(In.info.modified);
	Out.set<6>(In.serialNumber);
	Out.set<7>(In.deviceType);
	Out.set<8>(In.operatorId);
	Out.set<9>(In.subscriberId);
	Out.set<10>(OpenWifi::RESTAPI_utils::to_string(In.location));
	Out.set<11>(OpenWifi::RESTAPI_utils::to_string(In.contact));
	Out.set<12>(In.managementPolicy);
	Out.set<13>(In.serviceClass);
	Out.set<14>(In.qrCode);
	Out.set<15>(In.geoCode);
	Out.set<16>(OpenWifi::RESTAPI_utils::to_string(In.deviceRules));
	Out.set<17>(In.state);
	Out.set<18>(In.locale);
	Out.set<19>(In.billingCode);
	Out.set<20>(In.configurationId);
	Out.set<21>(In.suspended);
	Out.set<22>(In.realMacAddress);
	Out.set<23>(Poco::toLower(In.deviceGroup.empty() ? std::string("ap") : In.deviceGroup));
}
