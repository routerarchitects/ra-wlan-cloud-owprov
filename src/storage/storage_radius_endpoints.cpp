//
// Created by stephane bourque on 2023-09-27.
//

#include "storage_radius_endpoints.h"
#include <framework/RESTAPI_utils.h>
namespace OpenWifi {

    static ORM::FieldVec RadiusEndpointDB_Fields{// object info
            ORM::Field{"id", 64, true},
            ORM::Field{"name", ORM::FieldType::FT_TEXT},
            ORM::Field{"description", ORM::FieldType::FT_TEXT},
            ORM::Field{"notes", ORM::FieldType::FT_TEXT},
            ORM::Field{"created", ORM::FieldType::FT_BIGINT},
            ORM::Field{"modified", ORM::FieldType::FT_BIGINT},
            ORM::Field{"Type", ORM::FieldType::FT_TEXT},
            ORM::Field{"RadsecServers", ORM::FieldType::FT_TEXT},
            ORM::Field{"RadiusServers", ORM::FieldType::FT_TEXT},
            ORM::Field{"PoolStrategy", ORM::FieldType::FT_TEXT},
            ORM::Field{"radiusIndex", ORM::FieldType::FT_TEXT},
            ORM::Field{"UsedBy", ORM::FieldType::FT_TEXT},
            ORM::Field{"UseGWProxy", ORM::FieldType::FT_BOOLEAN},
            ORM::Field{"NasIdentifier", ORM::FieldType::FT_TEXT},
            ORM::Field{"AccountingInterval", ORM::FieldType::FT_BIGINT}
    };

    static ORM::IndexVec RadiusEndpointDB_Indexes{
            {std::string("radius_ep_name_index"),
             ORM::IndexEntryVec{{std::string("name"), ORM::Indextype::ASC}}}};

    RadiusEndpointDB::RadiusEndpointDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L)
            : DB(T, "radius_endpoints", RadiusEndpointDB_Fields, RadiusEndpointDB_Indexes, P, L, "rep") {}

    bool RadiusEndpointDB::Upgrade([[maybe_unused]] uint32_t from, uint32_t &to) {
        to = Version();
        std::vector<std::string> Script{
            "alter table " + TableName_ + " add column NasIdentifier TEXT;",
            "alter table " + TableName_ + " add column AccountingInterval BIGINT;"
        };

        for (const auto &i : Script) {
            try {
                auto Session = Pool_.get();
                Session << i, Poco::Data::Keywords::now;
            } catch (...) {
            }
        }
        return true;
    }

} // namespace OpenWifi

template <>
void ORM::DB<OpenWifi::RadiusEndpointDbRecordType, OpenWifi::ProvObjects::RADIUSEndPoint>::Convert(
        const OpenWifi::RadiusEndpointDbRecordType &In, OpenWifi::ProvObjects::RADIUSEndPoint &Out) {
    Out.info.id = In.get<0>();
    Out.info.name = In.get<1>();
    Out.info.description = In.get<2>();
    Out.info.notes =
            OpenWifi::RESTAPI_utils::to_object_array<OpenWifi::SecurityObjects::NoteInfo>(In.get<3>());
    Out.info.created = In.get<4>();
    Out.info.modified = In.get<5>();
    Out.Type = In.get<6>();
    Out.RadsecServers = OpenWifi::RESTAPI_utils::to_object_array<OpenWifi::ProvObjects::RADIUSEndPointRadsecType>(In.get<7>());
    Out.RadiusServers = OpenWifi::RESTAPI_utils::to_object_array<OpenWifi::ProvObjects::RADIUSEndPointRadiusType>(In.get<8>());
    Out.PoolStrategy = In.get<9>();
    Out.Index = In.get<10>();
    Out.UsedBy = OpenWifi::RESTAPI_utils::to_object_array(In.get<11>());
    Out.UseGWProxy = In.get<12>();
    Out.NasIdentifier = In.get<13>();
    Out.AccountingInterval = In.get<14>();
}

template <>
void ORM::DB<OpenWifi::RadiusEndpointDbRecordType, OpenWifi::ProvObjects::RADIUSEndPoint>::Convert(
        const OpenWifi::ProvObjects::RADIUSEndPoint &In, OpenWifi::RadiusEndpointDbRecordType &Out) {
    Out.set<0>(In.info.id);
    Out.set<1>(In.info.name);
    Out.set<2>(In.info.description);
    Out.set<3>(OpenWifi::RESTAPI_utils::to_string(In.info.notes));
    Out.set<4>(In.info.created);
    Out.set<5>(In.info.modified);
    Out.set<6>(In.Type);
    Out.set<7>(OpenWifi::RESTAPI_utils::to_string(In.RadsecServers));
    Out.set<8>(OpenWifi::RESTAPI_utils::to_string(In.RadiusServers));
    Out.set<9>(In.PoolStrategy);
    Out.set<10>(In.Index);
    Out.set<11>(OpenWifi::RESTAPI_utils::to_string(In.UsedBy));
    Out.set<12>(In.UseGWProxy);
    Out.set<13>(In.NasIdentifier);
    Out.set<14>(In.AccountingInterval);
}
