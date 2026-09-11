//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include "storage_management_roles.h"
#include "RESTObjects/RESTAPI_SecurityObjects.h"
#include "framework/OpenWifiTypes.h"
#include "framework/RESTAPI_utils.h"

namespace OpenWifi {

	static ORM::FieldVec RolesDB_Fields{// object info
										ORM::Field{"id", 64, true},
										ORM::Field{"name", ORM::FieldType::FT_TEXT},
										ORM::Field{"description", ORM::FieldType::FT_TEXT},
										ORM::Field{"notes", ORM::FieldType::FT_TEXT},
										ORM::Field{"created", ORM::FieldType::FT_BIGINT},
										ORM::Field{"modified", ORM::FieldType::FT_BIGINT},
										ORM::Field{"managementPolicy", ORM::FieldType::FT_TEXT},
										ORM::Field{"users", ORM::FieldType::FT_TEXT},
										ORM::Field{"inUse", ORM::FieldType::FT_TEXT},
										ORM::Field{"tags", ORM::FieldType::FT_TEXT},
										ORM::Field{"entity", ORM::FieldType::FT_TEXT},
										ORM::Field{"venue", ORM::FieldType::FT_TEXT}};

	static ORM::IndexVec RolesDB_Indexes{
		{std::string("roles_name_index"),
		 ORM::IndexEntryVec{{std::string("name"), ORM::Indextype::ASC}}}};

	ManagementRoleDB::ManagementRoleDB(OpenWifi::DBType T, Poco::Data::SessionPool &P,
									   Poco::Logger &L)
		: DB(T, "roles", RolesDB_Fields, RolesDB_Indexes, P, L, "rol") {}

	bool ManagementRoleDB::Upgrade([[maybe_unused]] uint32_t from, uint32_t &to) {
		std::vector<std::string> Statements{
			"alter table " + TableName_ + " add column entity text;",
			"alter table " + TableName_ + " add column venue text;"};
		RunScript(Statements);
		to = 2;
		return true;
	}

	bool ManagementRoleDB::HasPolicy(const std::string &PolicyId, bool &InUse) {
		try {
			uint64_t Count = 0;
			Poco::Data::Session Session = Pool_.get();
			Poco::Data::Statement Select(Session);

			std::string St = "SELECT COUNT(*) FROM (SELECT 1 FROM " + TableName_ +
							 " WHERE managementPolicy=? LIMIT 1) AS t";
			Select << ConvertParams(St), Poco::Data::Keywords::into(Count),
				Poco::Data::Keywords::use(PolicyId);
			Select.execute();

			InUse = (Count > 0);
			return true;
		} catch (const Poco::Exception &E) {
			Logger_.log(E);
		}
		return false;
	}

} // namespace OpenWifi

template <>
void ORM::DB<OpenWifi::ManagementRoleDBRecordType, OpenWifi::ProvObjects::ManagementRole>::Convert(
	const OpenWifi::ManagementRoleDBRecordType &In, OpenWifi::ProvObjects::ManagementRole &Out) {
	Out.info.id = In.get<0>();
	Out.info.name = In.get<1>();
	Out.info.description = In.get<2>();
	Out.info.notes =
		OpenWifi::RESTAPI_utils::to_object_array<OpenWifi::SecurityObjects::NoteInfo>(In.get<3>());
	Out.info.created = In.get<4>();
	Out.info.modified = In.get<5>();
	Out.managementPolicy = In.get<6>();
	Out.users = OpenWifi::RESTAPI_utils::to_object_array(In.get<7>());
	Out.inUse = OpenWifi::RESTAPI_utils::to_object_array(In.get<8>());
	Out.info.tags = OpenWifi::RESTAPI_utils::to_taglist(In.get<9>());
	Out.entity = In.get<10>();
	Out.venue = In.get<11>();
}

template <>
void ORM::DB<OpenWifi::ManagementRoleDBRecordType, OpenWifi::ProvObjects::ManagementRole>::Convert(
	const OpenWifi::ProvObjects::ManagementRole &In, OpenWifi::ManagementRoleDBRecordType &Out) {
	Out.set<0>(In.info.id);
	Out.set<1>(In.info.name);
	Out.set<2>(In.info.description);
	Out.set<3>(OpenWifi::RESTAPI_utils::to_string(In.info.notes));
	Out.set<4>(In.info.created);
	Out.set<5>(In.info.modified);
	Out.set<6>(In.managementPolicy);
	Out.set<7>(OpenWifi::RESTAPI_utils::to_string(In.users));
	Out.set<8>(OpenWifi::RESTAPI_utils::to_string(In.inUse));
	Out.set<9>(OpenWifi::RESTAPI_utils::to_string(In.info.tags));
	Out.set<10>(In.entity);
	Out.set<11>(In.venue);
}
