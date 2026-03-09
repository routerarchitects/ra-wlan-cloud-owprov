//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include <functional>

#include "RESTObjects/RESTAPI_SecurityObjects.h"
#include "StorageService.h"
#include "framework/CIDR.h"
#include "framework/OpenWifiTypes.h"
#include "framework/RESTAPI_utils.h"
#include "storage_venue.h"

namespace OpenWifi {

	static ORM::FieldVec VenueDB_Fields{// object info
										ORM::Field{"id", 64, true},
										ORM::Field{"name", ORM::FieldType::FT_TEXT},
										ORM::Field{"description", ORM::FieldType::FT_TEXT},
										ORM::Field{"notes", ORM::FieldType::FT_TEXT},
										ORM::Field{"created", ORM::FieldType::FT_BIGINT},
										ORM::Field{"modified", ORM::FieldType::FT_BIGINT},
										ORM::Field{"entity", ORM::FieldType::FT_TEXT},
										ORM::Field{"parent", ORM::FieldType::FT_TEXT},
										ORM::Field{"children", ORM::FieldType::FT_TEXT},
										ORM::Field{"devices", ORM::FieldType::FT_TEXT},
										ORM::Field{"managementPolicy", ORM::FieldType::FT_TEXT},
										ORM::Field{"topology", ORM::FieldType::FT_TEXT},
										ORM::Field{"design", ORM::FieldType::FT_TEXT},
										ORM::Field{"contacts", ORM::FieldType::FT_TEXT},
										ORM::Field{"location", ORM::FieldType::FT_TEXT},
										ORM::Field{"deviceRules", ORM::FieldType::FT_TEXT},
										ORM::Field{"tags", ORM::FieldType::FT_TEXT},
										ORM::Field{"deviceConfiguration", ORM::FieldType::FT_TEXT},
										ORM::Field{"sourceIP", ORM::FieldType::FT_TEXT},
										ORM::Field{"variables", ORM::FieldType::FT_TEXT},
										ORM::Field{"configurations", ORM::FieldType::FT_TEXT},
										ORM::Field{"maps", ORM::FieldType::FT_TEXT},
										ORM::Field{"managementPolicies", ORM::FieldType::FT_TEXT},
										ORM::Field{"managementRoles", ORM::FieldType::FT_TEXT},
										ORM::Field{"boards", ORM::FieldType::FT_TEXT},
										ORM::Field{"subscriber", ORM::FieldType::FT_TEXT}};

	static ORM::IndexVec VenueDB_Indexes{
		{std::string("venue_name_index"),
		 ORM::IndexEntryVec{{std::string("name"), ORM::Indextype::ASC}}}};

	VenueDB::VenueDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L)
		: DB(T, "venues", VenueDB_Fields, VenueDB_Indexes, P, L, "ven") {}

	bool VenueDB::Upgrade([[maybe_unused]] uint32_t from, uint32_t &to) {
		to = Version();
		std::vector<std::string> Script{
			"alter table " + TableName_ + " add column variables text",
			"alter table " + TableName_ + " add column configurations text",
			"alter table " + TableName_ + " add column maps text",
			"alter table " + TableName_ + " add column managementRoles text",
			"alter table " + TableName_ + " add column managementPolicies text",
			"alter table " + TableName_ + " add column boards text",
			"alter table " + TableName_ + " rename column contact to contacts",
			"alter table " + TableName_ + " add column deviceRules text",
			"alter table " + TableName_ + " add column subscriber text"};

		for (const auto &i : Script) {
			try {
				auto Session = Pool_.get();
				Session << i, Poco::Data::Keywords::now;
			} catch (...) {
			}
		}
		return true;
	}

	bool VenueDB::GetByIP(const std::string &IP, std::string &uuid) {
		try {
			std::string UUID;
			std::function<bool(const ProvObjects::Venue &E)> Function =
				[&UUID, IP](const ProvObjects::Venue &E) -> bool {
				if (E.sourceIP.empty())
					return true;
				if (CIDR::IpInRanges(IP, E.sourceIP)) {
					UUID = E.info.id;
					return false;
				}
				return true;
			};
			Iterate(Function);
			uuid = UUID;
		} catch (const Poco::Exception &E) {
			Logger().log(E);
		}
		return false;
	}

	bool VenueDB::EvaluateDeviceRules(const std::string &id, ProvObjects::DeviceRules &Rules) {
		ProvObjects::Venue V;
		if (GetRecord("id", id, V)) {
			if (!Storage::ApplyRules(V.deviceRules, Rules))
				return true;

			if (!V.parent.empty()) {
				return EvaluateDeviceRules(V.parent, Rules);
			}

			if (!V.entity.empty()) {
				return StorageService()->EntityDB().EvaluateDeviceRules(V.entity, Rules);
			}
		}
		Storage::ApplyConfigRules(Rules);
		return true;
	}

    bool VenueDB::DoesVenueNameAlreadyExist(const std::string &name, const std::string &entity_uuid, const std::string &parent_uuid) {

        std::string Statement;
        if(!entity_uuid.empty()) {
            Statement = fmt::format("select count(*) from venues where entity='{}' and upper(name)='{}'",
                                    entity_uuid, Poco::toUpper(name));
        } else {
            Statement = fmt::format("select count(*) from venues where parent='{}' and upper(name)='{}'",
                                    parent_uuid, Poco::toUpper(name));
        }

        std::uint64_t RecordCount = 0;
        try {
            Poco::Data::Session Session = Pool_.get();
            Poco::Data::Statement Command(Session);

            Command << Statement,
                    Poco::Data::Keywords::into(RecordCount);
            Command.execute();
        } catch (...) {

        }
        return RecordCount!=0;
    }

} // namespace OpenWifi

template <>
void ORM::DB<OpenWifi::VenueDBRecordType, OpenWifi::ProvObjects::Venue>::Convert(
	const OpenWifi::VenueDBRecordType &In, OpenWifi::ProvObjects::Venue &Out) {
	Out.info.id = In.get<0>();
	Out.info.name = In.get<1>();
	Out.info.description = In.get<2>();
	Out.info.notes =
		OpenWifi::RESTAPI_utils::to_object_array<OpenWifi::SecurityObjects::NoteInfo>(In.get<3>());
	Out.info.created = In.get<4>();
	Out.info.modified = In.get<5>();
	Out.entity = In.get<6>();
	Out.parent = In.get<7>();
	Out.children = OpenWifi::RESTAPI_utils::to_object_array(In.get<8>());
	Out.devices = OpenWifi::RESTAPI_utils::to_object_array(In.get<9>());
	Out.managementPolicy = In.get<10>();
	Out.topology =
		OpenWifi::RESTAPI_utils::to_object_array<OpenWifi::ProvObjects::DiGraphEntry>(In.get<11>());
	Out.design = In.get<12>();
	Out.contacts = OpenWifi::RESTAPI_utils::to_object_array(In.get<13>());
	Out.location = In.get<14>();
	Out.deviceRules =
		OpenWifi::RESTAPI_utils::to_object<OpenWifi::ProvObjects::DeviceRules>(In.get<15>());
	Out.info.tags = OpenWifi::RESTAPI_utils::to_taglist(In.get<16>());
	Out.deviceConfiguration = OpenWifi::RESTAPI_utils::to_object_array(In.get<17>());
	Out.sourceIP = OpenWifi::RESTAPI_utils::to_object_array(In.get<18>());
	Out.variables = OpenWifi::RESTAPI_utils::to_object_array(In.get<19>());
	Out.configurations = OpenWifi::RESTAPI_utils::to_object_array(In.get<20>());
	Out.maps = OpenWifi::RESTAPI_utils::to_object_array(In.get<21>());
	Out.managementPolicies = OpenWifi::RESTAPI_utils::to_object_array(In.get<22>());
	Out.managementRoles = OpenWifi::RESTAPI_utils::to_object_array(In.get<23>());
	Out.boards = OpenWifi::RESTAPI_utils::to_object_array(In.get<24>());
	Out.subscriber = In.get<25>();
}

template <>
void ORM::DB<OpenWifi::VenueDBRecordType, OpenWifi::ProvObjects::Venue>::Convert(
	const OpenWifi::ProvObjects::Venue &In, OpenWifi::VenueDBRecordType &Out) {
	Out.set<0>(In.info.id);
	Out.set<1>(In.info.name);
	Out.set<2>(In.info.description);
	Out.set<3>(OpenWifi::RESTAPI_utils::to_string(In.info.notes));
	Out.set<4>(In.info.created);
	Out.set<5>(In.info.modified);
	Out.set<6>(In.entity);
	Out.set<7>(In.parent);
	Out.set<8>(OpenWifi::RESTAPI_utils::to_string(In.children));
	Out.set<9>(OpenWifi::RESTAPI_utils::to_string(In.devices));
	Out.set<10>(In.managementPolicy);
	Out.set<11>(OpenWifi::RESTAPI_utils::to_string(In.topology));
	Out.set<12>(In.design);
	Out.set<13>(OpenWifi::RESTAPI_utils::to_string(In.contacts));
	Out.set<14>(In.location);
	Out.set<15>(OpenWifi::RESTAPI_utils::to_string(In.deviceRules));
	Out.set<16>(OpenWifi::RESTAPI_utils::to_string(In.info.tags));
	Out.set<17>(OpenWifi::RESTAPI_utils::to_string(In.deviceConfiguration));
	Out.set<18>(OpenWifi::RESTAPI_utils::to_string(In.sourceIP));
	Out.set<19>(OpenWifi::RESTAPI_utils::to_string(In.variables));
	Out.set<20>(OpenWifi::RESTAPI_utils::to_string(In.configurations));
	Out.set<21>(OpenWifi::RESTAPI_utils::to_string(In.maps));
	Out.set<22>(OpenWifi::RESTAPI_utils::to_string(In.managementPolicies));
	Out.set<23>(OpenWifi::RESTAPI_utils::to_string(In.managementRoles));
	Out.set<24>(OpenWifi::RESTAPI_utils::to_string(In.boards));
	Out.set<25>(In.subscriber);
}
