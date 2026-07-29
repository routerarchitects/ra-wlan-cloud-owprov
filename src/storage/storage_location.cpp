//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include "storage_location.h"
#include "framework/OpenWifiTypes.h"
#include "framework/RESTAPI_utils.h"

#include "RESTObjects/RESTAPI_SecurityObjects.h"

namespace OpenWifi {

	static ORM::FieldVec LocationDB_Fields{// object info
										   ORM::Field{"id", 64, true},
										   ORM::Field{"name", ORM::FieldType::FT_TEXT},
										   ORM::Field{"description", ORM::FieldType::FT_TEXT},
										   ORM::Field{"notes", ORM::FieldType::FT_TEXT},
										   ORM::Field{"created", ORM::FieldType::FT_BIGINT},
										   ORM::Field{"modified", ORM::FieldType::FT_BIGINT},
										   ORM::Field{"type", ORM::FieldType::FT_TEXT},
										   ORM::Field{"buildingName", ORM::FieldType::FT_TEXT},
										   ORM::Field{"addressLines", ORM::FieldType::FT_TEXT},
										   ORM::Field{"city", ORM::FieldType::FT_TEXT},
										   ORM::Field{"state", ORM::FieldType::FT_TEXT},
										   ORM::Field{"postal", ORM::FieldType::FT_TEXT},
										   ORM::Field{"country", ORM::FieldType::FT_TEXT},
										   ORM::Field{"phones", ORM::FieldType::FT_TEXT},
										   ORM::Field{"mobiles", ORM::FieldType::FT_TEXT},
										   ORM::Field{"geoCode", ORM::FieldType::FT_TEXT},
										   ORM::Field{"inUse", ORM::FieldType::FT_TEXT},
										   ORM::Field{"tags", ORM::FieldType::FT_TEXT},
										   ORM::Field{"managementPolicy", ORM::FieldType::FT_TEXT},
										   ORM::Field{"entity", ORM::FieldType::FT_TEXT},
										   ORM::Field{"timezone", ORM::FieldType::FT_TEXT}};

	static ORM::IndexVec LocationDB_Indexes{
		{std::string("location_name_index"),
		 ORM::IndexEntryVec{{std::string("name"), ORM::Indextype::ASC}}}};

	LocationDB::LocationDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L)
		: DB(T, "locations", LocationDB_Fields, LocationDB_Indexes, P, L, "loc") {}

	// Upgrades the locations table schema by adding the timezone column.
	bool LocationDB::Upgrade([[maybe_unused]] uint32_t from, uint32_t &to) {
		to = Version();

		std::string CheckQuery;
		if (Type_ == OpenWifi::DBType::sqlite) {
			CheckQuery = "select count(*) from pragma_table_info('" + TableName_ + "') where lower(name) = 'timezone'";
		} else if (Type_ == OpenWifi::DBType::pgsql) {
			CheckQuery = "select count(*) from information_schema.columns where lower(table_name) = '" + Poco::toLower(TableName_) + "' and lower(column_name) = 'timezone' and table_schema = current_schema()";
		} else {
			CheckQuery = "select count(*) from information_schema.columns where lower(table_name) = '" + Poco::toLower(TableName_) + "' and lower(column_name) = 'timezone' and table_schema = database()";
		}

		std::size_t count = 0;
		try {
			auto Session = Pool_.get();
			Session << CheckQuery, Poco::Data::Keywords::into(count), Poco::Data::Keywords::now;
		} catch (const Poco::Exception &E) {
			Logger().error(fmt::format("LocationDB::Upgrade schema pre-check failed on table {}: {}", TableName_, E.displayText()));
			return false;
		} catch (const std::exception &E) {
			Logger().error(fmt::format("LocationDB::Upgrade schema pre-check failed on table {}: {}", TableName_, E.what()));
			return false;
		} catch (...) {
			Logger().error(fmt::format("LocationDB::Upgrade schema pre-check failed on table {}: unknown exception", TableName_));
			return false;
		}

		if (count > 0) {
			Logger().information(fmt::format("LocationDB::Upgrade: Column 'timezone' already exists in table {}", TableName_));
			return true;
		}

		try {
			auto Session = Pool_.get();
			std::string Query = "alter table " + TableName_ + " add column timezone text";
			Session << Query, Poco::Data::Keywords::now;
			Logger().information(fmt::format("LocationDB::Upgrade: Successfully added column 'timezone' to table {}", TableName_));
			return true;
		} catch (const Poco::Exception &E) {
			Logger().error(fmt::format("LocationDB::Upgrade failed on table {}: {}", TableName_, E.displayText()));
			return false;
		} catch (const std::exception &E) {
			Logger().error(fmt::format("LocationDB::Upgrade failed on table {}: {}", TableName_, E.what()));
			return false;
		} catch (...) {
			Logger().error(fmt::format("LocationDB::Upgrade failed on table {}: unknown exception", TableName_));
			return false;
		}
	}

} // namespace OpenWifi

template <>
void ORM::DB<OpenWifi::LocationDBRecordType, OpenWifi::ProvObjects::Location>::Convert(
	const OpenWifi::LocationDBRecordType &In, OpenWifi::ProvObjects::Location &Out) {
	Out.info.id = In.get<0>();
	Out.info.name = In.get<1>();
	Out.info.description = In.get<2>();
	Out.info.notes =
		OpenWifi::RESTAPI_utils::to_object_array<OpenWifi::SecurityObjects::NoteInfo>(In.get<3>());
	Out.info.created = In.get<4>();
	Out.info.modified = In.get<5>();
	Out.type = OpenWifi::ProvObjects::location_from_string(In.get<6>());
	Out.buildingName = In.get<7>();
	Out.addressLines = OpenWifi::RESTAPI_utils::to_object_array(In.get<8>());
	Out.city = In.get<9>();
	Out.state = In.get<10>();
	Out.postal = In.get<11>();
	Out.country = In.get<12>();
	Out.phones = OpenWifi::RESTAPI_utils::to_object_array(In.get<13>());
	Out.mobiles = OpenWifi::RESTAPI_utils::to_object_array(In.get<14>());
	Out.geoCode = In.get<15>();
	Out.inUse = OpenWifi::RESTAPI_utils::to_object_array(In.get<16>());
	Out.info.tags = OpenWifi::RESTAPI_utils::to_taglist(In.get<17>());
	Out.managementPolicy = In.get<18>();
	Out.entity = In.get<19>();
	Out.timezone = In.get<20>();
}

template <>
void ORM::DB<OpenWifi::LocationDBRecordType, OpenWifi::ProvObjects::Location>::Convert(
	const OpenWifi::ProvObjects::Location &In, OpenWifi::LocationDBRecordType &Out) {
	Out.set<0>(In.info.id);
	Out.set<1>(In.info.name);
	Out.set<2>(In.info.description);
	Out.set<3>(OpenWifi::RESTAPI_utils::to_string(In.info.notes));
	Out.set<4>(In.info.created);
	Out.set<5>(In.info.modified);
	Out.set<6>(OpenWifi::ProvObjects::to_string(In.type));
	Out.set<7>(In.buildingName);
	Out.set<8>(OpenWifi::RESTAPI_utils::to_string(In.addressLines));
	Out.set<9>(In.city);
	Out.set<10>(In.state);
	Out.set<11>(In.postal);
	Out.set<12>(In.country);
	Out.set<13>(OpenWifi::RESTAPI_utils::to_string(In.phones));
	Out.set<14>(OpenWifi::RESTAPI_utils::to_string(In.mobiles));
	Out.set<15>(In.geoCode);
	Out.set<16>(OpenWifi::RESTAPI_utils::to_string(In.inUse));
	Out.set<17>(OpenWifi::RESTAPI_utils::to_string(In.info.tags));
	Out.set<18>(In.managementPolicy);
	Out.set<19>(In.entity);
	Out.set<20>(In.timezone);
}
