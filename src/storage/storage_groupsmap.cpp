/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
#ifdef CGW_INTEGRATION
#include "storage_groupsmap.h"
#include "Poco/Data/Statement.h"
#include "Poco/Data/Session.h"
#include "Poco/Logger.h"

namespace OpenWifi {
    static ORM::FieldVec GroupsMapDB_Fields{
        ORM::Field{"venueid", ORM::FieldType::FT_TEXT, 64},
        ORM::Field{"groupid", ORM::FieldType::FT_BIGINT, 0, true}
    };
    static ORM::IndexVec GroupsMapDB_Indexes{
        {std::string{"groupsmap_venue_idx"}, ORM::IndexEntryVec{{std::string{"venueid"}, ORM::Indextype::ASC}}}
    };

    GroupsMapDB::GroupsMapDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L)
        : DB(T, "groupsmap", GroupsMapDB_Fields, GroupsMapDB_Indexes, P, L, "gmap") {}

    uint64_t GroupsMapDB::NextId() {
        uint64_t id = 0;
        try {
            Poco::Data::Session Session = Pool_.get();
            Poco::Data::Statement Select(Session);
            Select << "select max(groupid) from " + TableName_, Poco::Data::Keywords::into(id), Poco::Data::Keywords::now;
        } catch (...) {
            poco_error(Logger(), "Exception in GroupsMapDB::NextId");
        }
        return id + 1;
    }

    bool GroupsMapDB::AddVenue(const std::string &venueId, uint64_t &groupId) {
        GroupsMapRecord R;
        R.venueid = venueId;
        R.groupid = NextId();
        groupId = R.groupid;
        bool ok = CreateRecord(R);
        poco_debug(Logger(), fmt::format("Added venue {} with group id {}", venueId, groupId));
        return ok;
    }

    bool GroupsMapDB::GetGroup(const std::string &venueId, uint64_t &groupId) {
        GroupsMapRecord rec;
        if (GetRecord("venueid", venueId, rec)) {
            groupId = rec.groupid;
            return true;
        }
        return false;
    }

    bool GroupsMapDB::DeleteVenue(const std::string &venueId) {
        bool ok = DeleteRecord("venueid", venueId);
        return ok;
    }
} // namespace OpenWifi

template<> void ORM::DB<OpenWifi::GroupsMapDBRecordType, OpenWifi::GroupsMapRecord>::Convert(const OpenWifi::GroupsMapDBRecordType &In, OpenWifi::GroupsMapRecord &Out) {
    Out.venueid = In.get<0>();
    Out.groupid = In.get<1>();
}

template<> void ORM::DB<OpenWifi::GroupsMapDBRecordType, OpenWifi::GroupsMapRecord>::Convert(const OpenWifi::GroupsMapRecord &In, OpenWifi::GroupsMapDBRecordType &Out) {
    Out.set<0>(In.venueid);
    Out.set<1>(In.groupid);
}
#endif