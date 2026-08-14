/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
#ifdef CGW_INTEGRATION
#include "storage_groupsmap.h"
#include <limits>

#include "Poco/Data/Session.h"
#include "Poco/Data/Statement.h"
#include "Poco/Logger.h"

namespace OpenWifi {
	static ORM::FieldVec GroupsMapDB_Fields{ORM::Field{"venueid", ORM::FieldType::FT_TEXT, 64},
											ORM::Field{"groupid", ORM::FieldType::FT_INT, 0, true}};
	static ORM::IndexVec GroupsMapDB_Indexes{};

	GroupsMapDB::GroupsMapDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L)
		: DB(T, "groupsmap", GroupsMapDB_Fields, GroupsMapDB_Indexes, P, L, "gmap") {}

	bool GroupsMapDB::Create() {
		try {
			Poco::Data::Session Session = Pool_.get();
			if (Type_ != OpenWifi::DBType::pgsql) {
				poco_error(Logger(), "GroupsMapDB::Create requires PostgreSQL.");
				return false;
			}

			std::string statement = "create table if not exists " + TableName_ +
									" ( venueid TEXT, groupid SERIAL PRIMARY KEY )";
			Session << statement, Poco::Data::Keywords::now;
			Session << "create unique index if not exists groupsmap_venue_uidx on " + TableName_ +
						   " ( venueid ASC )",
				Poco::Data::Keywords::now;
		} catch (const std::exception &E) {
			poco_error(Logger(), fmt::format("Exception in GroupsMapDB::Create {}", E.what()));
			return false;
		}
		return Upgrade(); // Returns its result (so final success/failure comes from Upgrade).
	}

	bool GroupsMapDB::AddVenue(const std::string &venueId, std::uint32_t &groupId) {
		try {
			if (Type_ != OpenWifi::DBType::pgsql) {
				poco_error(Logger(), "GroupsMapDB::AddVenue requires PostgreSQL.");
				return false;
			}

			Poco::Data::Session Session = Pool_.get();
			Poco::Data::Statement Insert(Session);
			std::string venueIdParam = venueId;
			groupId = 0;
			std::string statement = "insert into " + TableName_ +
									" (venueid) values (?) "
									"on conflict (venueid) do update set venueid=excluded.venueid "
									"returning groupid";

			Insert << ConvertParams(statement), Poco::Data::Keywords::use(venueIdParam),
				Poco::Data::Keywords::into(groupId), Poco::Data::Keywords::now;

			poco_debug(Logger(),
					   fmt::format("Upserted venue {} with group id {}", venueId, groupId));
			return true;
		} catch (const Poco::Exception &E) {
			Logger().log(E);
		}
		return false;
	}


	bool GroupsMapDB::GetGroup(const std::string &venueId, std::uint32_t &groupId) {
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

template <>
void ORM::DB<OpenWifi::GroupsMapDBRecordType, OpenWifi::GroupsMapRecord>::Convert(
	const OpenWifi::GroupsMapDBRecordType &In, OpenWifi::GroupsMapRecord &Out) {
	Out.venueid = In.get<0>();
	Out.groupid = In.get<1>();
}

template <>
void ORM::DB<OpenWifi::GroupsMapDBRecordType, OpenWifi::GroupsMapRecord>::Convert(
	const OpenWifi::GroupsMapRecord &In, OpenWifi::GroupsMapDBRecordType &Out) {
	Out.set<0>(In.venueid);
	Out.set<1>(In.groupid);
}
#endif
