//
// Created by stephane bourque on 2022-04-05.
//

#include "storage_operataor.h"
#include "RESTObjects/RESTAPI_SecurityObjects.h"
#include "StorageService.h"
#include "framework/CIDR.h"
#include "framework/OpenWifiTypes.h"
#include "framework/RESTAPI_utils.h"

namespace OpenWifi {

	static ORM::FieldVec OperatorDB_Fields{
		// object info
		ORM::Field{"id", 64, true},
		ORM::Field{"name", ORM::FieldType::FT_TEXT},
		ORM::Field{"description", ORM::FieldType::FT_TEXT},
		ORM::Field{"notes", ORM::FieldType::FT_TEXT},
		ORM::Field{"created", ORM::FieldType::FT_BIGINT},
		ORM::Field{"modified", ORM::FieldType::FT_BIGINT},
		ORM::Field{"managementPolicy", ORM::FieldType::FT_TEXT},
		ORM::Field{"managementRoles", ORM::FieldType::FT_TEXT},
		ORM::Field{"deviceRules", ORM::FieldType::FT_TEXT},
		ORM::Field{"variables", ORM::FieldType::FT_TEXT},
		ORM::Field{"defaultOperator", ORM::FieldType::FT_BOOLEAN},
		ORM::Field{"sourceIP", ORM::FieldType::FT_TEXT},
		ORM::Field{"registrationId", ORM::FieldType::FT_TEXT},
		ORM::Field{"entityId", ORM::FieldType::FT_TEXT}};

	static ORM::IndexVec OperatorDB_Indexes{
		{std::string("operator2_name_index"),
		 ORM::IndexEntryVec{{std::string("name"), ORM::Indextype::ASC}}}};

	OperatorDB::OperatorDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L)
		: DB(T, "operators2", OperatorDB_Fields, OperatorDB_Indexes, P, L, "opr") {}

	bool OperatorDB::Upgrade([[maybe_unused]] uint32_t from, uint32_t &to) {
		to = Version();
		std::vector<std::string> Script{"alter table " + TableName_ +
										" add column deviceRules text"};

		RunScript(Script);
		return true;
	}

	bool OperatorDB::GetByIP(const std::string &IP, std::string &uuid) {
		try {
			std::string UUID;
			std::function<bool(const ProvObjects::Operator &E)> Function =
				[&UUID, IP](const ProvObjects::Operator &E) -> bool {
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
} // namespace OpenWifi

template <>
void ORM::DB<OpenWifi::OperatorDBRecordType, OpenWifi::ProvObjects::Operator>::Convert(
	const OpenWifi::OperatorDBRecordType &In, OpenWifi::ProvObjects::Operator &Out) {
	Out.info.id = In.get<0>();
	Out.info.name = In.get<1>();
	Out.info.description = In.get<2>();
	Out.info.notes =
		OpenWifi::RESTAPI_utils::to_object_array<OpenWifi::SecurityObjects::NoteInfo>(In.get<3>());
	Out.info.created = In.get<4>();
	Out.info.modified = In.get<5>();
	Out.managementPolicy = In.get<6>();
	Out.managementRoles = OpenWifi::RESTAPI_utils::to_object_array(In.get<7>());
	Out.deviceRules =
		OpenWifi::RESTAPI_utils::to_object<OpenWifi::ProvObjects::DeviceRules>(In.get<8>());
	Out.variables =
		OpenWifi::RESTAPI_utils::to_object_array<OpenWifi::ProvObjects::Variable>(In.get<9>());
	Out.defaultOperator = In.get<10>();
	Out.sourceIP = OpenWifi::RESTAPI_utils::to_object_array(In.get<11>());
	Out.registrationId = In.get<12>();
	Out.entityId = In.get<13>();
}

template <>
void ORM::DB<OpenWifi::OperatorDBRecordType, OpenWifi::ProvObjects::Operator>::Convert(
	const OpenWifi::ProvObjects::Operator &In, OpenWifi::OperatorDBRecordType &Out) {
	Out.set<0>(In.info.id);
	Out.set<1>(In.info.name);
	Out.set<2>(In.info.description);
	Out.set<3>(OpenWifi::RESTAPI_utils::to_string(In.info.notes));
	Out.set<4>(In.info.created);
	Out.set<5>(In.info.modified);
	Out.set<6>(In.managementPolicy);
	Out.set<7>(OpenWifi::RESTAPI_utils::to_string(In.managementRoles));
	Out.set<8>(OpenWifi::RESTAPI_utils::to_string(In.deviceRules));
	Out.set<9>(OpenWifi::RESTAPI_utils::to_string(In.variables));
	Out.set<10>(In.defaultOperator);
	Out.set<11>(OpenWifi::RESTAPI_utils::to_string(In.sourceIP));
	Out.set<12>(In.registrationId);
	Out.set<13>(In.entityId);
}
