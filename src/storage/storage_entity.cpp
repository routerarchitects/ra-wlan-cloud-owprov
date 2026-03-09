//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include "storage_entity.h"
#include "RESTObjects/RESTAPI_SecurityObjects.h"
#include "StorageService.h"
#include "framework/CIDR.h"
#include "framework/MicroServiceFuncs.h"
#include "framework/OpenWifiTypes.h"
#include "framework/RESTAPI_utils.h"
#include "framework/utils.h"

namespace OpenWifi {

	static ORM::FieldVec EntityDB_Fields{// object info
										 ORM::Field{"id", 64, true},
										 ORM::Field{"name", ORM::FieldType::FT_TEXT},
										 ORM::Field{"description", ORM::FieldType::FT_TEXT},
										 ORM::Field{"notes", ORM::FieldType::FT_TEXT},
										 ORM::Field{"created", ORM::FieldType::FT_BIGINT},
										 ORM::Field{"modified", ORM::FieldType::FT_BIGINT},
										 ORM::Field{"parent", ORM::FieldType::FT_TEXT},
										 ORM::Field{"children", ORM::FieldType::FT_TEXT},
										 ORM::Field{"contacts", ORM::FieldType::FT_TEXT},
										 ORM::Field{"locations", ORM::FieldType::FT_TEXT},
										 ORM::Field{"managementPolicy", ORM::FieldType::FT_TEXT},
										 ORM::Field{"venues", ORM::FieldType::FT_TEXT},
										 ORM::Field{"deviceConfiguration", ORM::FieldType::FT_TEXT},
										 ORM::Field{"devices", ORM::FieldType::FT_TEXT},
										 ORM::Field{"deviceRules", ORM::FieldType::FT_TEXT},
										 ORM::Field{"tags", ORM::FieldType::FT_TEXT},
										 ORM::Field{"sourceIP", ORM::FieldType::FT_TEXT},
										 ORM::Field{"variables", ORM::FieldType::FT_TEXT},
										 ORM::Field{"managementPolicies", ORM::FieldType::FT_TEXT},
										 ORM::Field{"managementRoles", ORM::FieldType::FT_TEXT},
										 ORM::Field{"maps", ORM::FieldType::FT_TEXT},
										 ORM::Field{"configurations", ORM::FieldType::FT_TEXT},
										 ORM::Field{"operatorId", ORM::FieldType::FT_TEXT}};

	static ORM::IndexVec EntityDB_Indexes{
		{std::string("entity_name_index"),
		 ORM::IndexEntryVec{{std::string("name"), ORM::Indextype::ASC}}}};

	EntityDB::EntityDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L)
		: DB(T, "entities", EntityDB_Fields, EntityDB_Indexes, P, L, "ent") {}

	bool EntityDB::Upgrade([[maybe_unused]] uint32_t from, uint32_t &to) {
		to = Version();
		std::vector<std::string> Script{
			"alter table " + TableName_ + " add column variables text",
			"alter table " + TableName_ + " add column managementPolicies text",
			"alter table " + TableName_ + " add column maps text",
			"alter table " + TableName_ + " add column configurations text",
			"alter table " + TableName_ + " add column managementRoles text",
			"alter table " + TableName_ + " add column deviceRules text",
			"alter table " + TableName_ + " add column operatorId text"};

		for (const auto &i : Script) {
			try {
				auto Session = Pool_.get();
				Session << i, Poco::Data::Keywords::now;
			} catch (...) {
			}
		}
		return true;
	}

	bool EntityDB::GetByIP(const std::string &IP, std::string &uuid) {
		try {
			std::string UUID;
			std::function<bool(const ProvObjects::Entity &E)> Function =
				[&UUID, IP](const ProvObjects::Entity &E) -> bool {
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

	void EntityDB::AddVenues(Poco::JSON::Object &Tree, const std::string &Node) {
		ProvObjects::Venue E;
		// std::cout << "Adding venue:" << Node << std::endl;
		if (StorageService()->VenueDB().GetRecord("id", Node, E)) {
			Poco::JSON::Array Venues;
			for (const auto &i : E.children) {
				Poco::JSON::Object Venue;
				AddVenues(Venue, i);
				Venues.add(Venue);
			}
			Tree.set("type", "venue");
			Tree.set("name", E.info.name);
			Tree.set("uuid", E.info.id);
			Tree.set("children", Venues);
		}
	}

	void EntityDB::BuildTree(Poco::JSON::Object &Tree, const std::string &Node) {
		ProvObjects::Entity E;
		// std::cout << "Adding node:" << Node << std::endl;
		if (StorageService()->EntityDB().GetRecord("id", Node, E)) {
			Poco::JSON::Array Children;
			for (const auto &i : E.children) {
				Poco::JSON::Object Child;
				BuildTree(Child, i);
				Children.add(Child);
			}
			Poco::JSON::Array Venues;
			for (const auto &i : E.venues) {
				Poco::JSON::Object Venue;
				AddVenues(Venue, i);
				Venues.add(Venue);
			}
			Tree.set("type", "entity");
			Tree.set("name", E.info.name);
			Tree.set("uuid", E.info.id);
			Tree.set("children", Children);
			Tree.set("venues", Venues);
		}
	}

	void EntityDB::ImportVenues(const Poco::JSON::Object::Ptr &O, const std::string &Parent) {
		std::string Type, Name, UUID;
		Poco::JSON::Array::Ptr Children, Venues;

		Type = O->get("type").toString();
		Name = O->get("name").toString();

		std::cout << "Name :" << Name << "  Venue Parent UUID: " << Parent << std::endl;
		Venues = O->getArray("venues");
		for (const auto &i : *Venues) {
			const auto &Child = i.extract<Poco::JSON::Object::Ptr>();
			ProvObjects::Venue V;
			V.info.name = Child->get("name").toString();
			V.info.id = MicroServiceCreateUUID();
			V.parent = Parent;
			V.info.created = V.info.modified = Utils::Now();
			// StorageService()->VenueDB().CreateShortCut(V);
			ImportVenues(Child, V.info.id);
		}
	}

	void EntityDB::ImportTree(const Poco::JSON::Object::Ptr &O, const std::string &Parent) {

		std::string Type, Name;

		Type = O->get("type").toString();
		Name = O->get("name").toString();

		if (Parent == EntityDB::RootUUID()) {
			ProvObjects::Entity E;
			E.info.name = Name;
			E.info.id = EntityDB::RootUUID();
			E.info.created = E.info.modified = Utils::Now();
			// StorageService()->EntityDB().CreateShortCut(E);
		}

		auto Children = O->getArray("children");
		for (const auto &i : *Children) {
			const auto &Child = i.extract<Poco::JSON::Object::Ptr>();
			ProvObjects::Entity E;

			E.info.name = Child->get("name").toString();
			E.info.id = MicroServiceCreateUUID();
			E.parent = Parent;
			E.info.created = E.info.modified = Utils::Now();
			// StorageService()->EntityDB().CreateShortCut(E);
			ImportTree(Child, E.info.id);
		}

		auto Venues = O->getArray("venues");
		for (const auto &i : *Venues) {
			const auto &Child = i.extract<Poco::JSON::Object::Ptr>();
			ProvObjects::Venue V;
			V.info.name = Child->get("name").toString();
			V.info.id = MicroServiceCreateUUID();
			V.entity = Parent;
			V.info.created = V.info.modified = Utils::Now();
			// StorageService()->VenueDB().CreateShortCut(V);
			ImportVenues(Child, V.info.id);
		}
	}

	bool EntityDB::EvaluateDeviceRules(const std::string &id, ProvObjects::DeviceRules &Rules) {
		ProvObjects::Entity E;
		if (GetRecord("id", id, E)) {
			if (!Storage::ApplyRules(E.deviceRules, Rules))
				return true;

			if (!E.parent.empty()) {
				return EvaluateDeviceRules(E.parent, Rules);
			}
		}
		Storage::ApplyConfigRules(Rules);
		return true;
	}

} // namespace OpenWifi

template <>
void ORM::DB<OpenWifi::EntityDBRecordType, OpenWifi::ProvObjects::Entity>::Convert(
	const OpenWifi::EntityDBRecordType &In, OpenWifi::ProvObjects::Entity &Out) {
	Out.info.id = In.get<0>();
	Out.info.name = In.get<1>();
	Out.info.description = In.get<2>();
	Out.info.notes =
		OpenWifi::RESTAPI_utils::to_object_array<OpenWifi::SecurityObjects::NoteInfo>(In.get<3>());
	Out.info.created = In.get<4>();
	Out.info.modified = In.get<5>();
	Out.parent = In.get<6>();
	Out.children = OpenWifi::RESTAPI_utils::to_object_array(In.get<7>());
	Out.contacts = OpenWifi::RESTAPI_utils::to_object_array(In.get<8>());
	Out.locations = OpenWifi::RESTAPI_utils::to_object_array(In.get<9>());
	Out.managementPolicy = In.get<10>();
	Out.venues = OpenWifi::RESTAPI_utils::to_object_array(In.get<11>());
	Out.deviceConfiguration = OpenWifi::RESTAPI_utils::to_object_array(In.get<12>());
	Out.devices = OpenWifi::RESTAPI_utils::to_object_array(In.get<13>());
	Out.deviceRules =
		OpenWifi::RESTAPI_utils::to_object<OpenWifi::ProvObjects::DeviceRules>(In.get<14>());
	Out.info.tags = OpenWifi::RESTAPI_utils::to_taglist(In.get<15>());
	Out.sourceIP = OpenWifi::RESTAPI_utils::to_object_array(In.get<16>());
	Out.variables = OpenWifi::RESTAPI_utils::to_object_array(In.get<17>());
	Out.managementPolicies = OpenWifi::RESTAPI_utils::to_object_array(In.get<18>());
	Out.managementRoles = OpenWifi::RESTAPI_utils::to_object_array(In.get<19>());
	Out.maps = OpenWifi::RESTAPI_utils::to_object_array(In.get<20>());
	Out.configurations = OpenWifi::RESTAPI_utils::to_object_array(In.get<21>());
	Out.operatorId = In.get<22>();
}

template <>
void ORM::DB<OpenWifi::EntityDBRecordType, OpenWifi::ProvObjects::Entity>::Convert(
	const OpenWifi::ProvObjects::Entity &In, OpenWifi::EntityDBRecordType &Out) {
	Out.set<0>(In.info.id);
	Out.set<1>(In.info.name);
	Out.set<2>(In.info.description);
	Out.set<3>(OpenWifi::RESTAPI_utils::to_string(In.info.notes));
	Out.set<4>(In.info.created);
	Out.set<5>(In.info.modified);
	Out.set<6>(In.parent);
	Out.set<7>(OpenWifi::RESTAPI_utils::to_string(In.children));
	Out.set<8>(OpenWifi::RESTAPI_utils::to_string(In.contacts));
	Out.set<9>(OpenWifi::RESTAPI_utils::to_string(In.locations));
	Out.set<10>(In.managementPolicy);
	Out.set<11>(OpenWifi::RESTAPI_utils::to_string(In.venues));
	Out.set<12>(OpenWifi::RESTAPI_utils::to_string(In.deviceConfiguration));
	Out.set<13>(OpenWifi::RESTAPI_utils::to_string(In.devices));
	Out.set<14>(OpenWifi::RESTAPI_utils::to_string(In.deviceRules));
	Out.set<15>(OpenWifi::RESTAPI_utils::to_string(In.info.tags));
	Out.set<16>(OpenWifi::RESTAPI_utils::to_string(In.sourceIP));
	Out.set<17>(OpenWifi::RESTAPI_utils::to_string(In.variables));
	Out.set<18>(OpenWifi::RESTAPI_utils::to_string(In.managementPolicies));
	Out.set<19>(OpenWifi::RESTAPI_utils::to_string(In.managementRoles));
	Out.set<20>(OpenWifi::RESTAPI_utils::to_string(In.maps));
	Out.set<21>(OpenWifi::RESTAPI_utils::to_string(In.configurations));
	Out.set<22>(In.operatorId);
}
