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

	bool ManagementRoleDB::Create() {
		try {
			Poco::Data::Session Session = Pool_.get();
			if (Type_ == OpenWifi::DBType::mysql) {
				std::string Statement =
					"CREATE TABLE IF NOT EXISTS " + TableName_ + " ("
					"id VARCHAR(64) UNIQUE PRIMARY KEY, "
					"name TEXT, "
					"description TEXT, "
					"notes TEXT, "
					"created BIGINT, "
					"modified BIGINT, "
					"managementPolicy VARCHAR(64) NOT NULL, "
					"users TEXT, "
					"inUse TEXT, "
					"tags TEXT, "
					"entity TEXT, "
					"venue TEXT, "
					"CONSTRAINT fk_roles_management_policy FOREIGN KEY (managementPolicy) REFERENCES policies(id) ON DELETE RESTRICT, "
					"INDEX roles_name_index (name(255)), "
					"INDEX roles_management_policy_idx (managementPolicy)"
					");";
				Session << Statement, Poco::Data::Keywords::now;

				uint64_t HasIndex = 0;
				std::string CheckIndex =
					"SELECT COUNT(*) FROM information_schema.statistics "
					"WHERE table_schema = DATABASE() AND table_name = '" + TableName_ + "' AND index_name = 'roles_name_index'";
				Session << CheckIndex, Poco::Data::Keywords::into(HasIndex), Poco::Data::Keywords::now;
				if (HasIndex == 0) {
					Session << "CREATE INDEX roles_name_index ON " + TableName_ + " (name(255));", Poco::Data::Keywords::now;
				}

				uint64_t HasPolicyIndex = 0;
				std::string CheckPolicyIndex =
					"SELECT COUNT(*) FROM information_schema.statistics "
					"WHERE table_schema = DATABASE() AND table_name = '" + TableName_ + "' AND index_name = 'roles_management_policy_idx'";
				Session << CheckPolicyIndex, Poco::Data::Keywords::into(HasPolicyIndex), Poco::Data::Keywords::now;
				if (HasPolicyIndex == 0) {
					Session << "CREATE INDEX roles_management_policy_idx ON " + TableName_ + " (managementPolicy);", Poco::Data::Keywords::now;
				}
			} else {
				std::string Statement =
					"CREATE TABLE IF NOT EXISTS " + TableName_ + " ("
					"id VARCHAR(64) UNIQUE PRIMARY KEY, "
					"name TEXT, "
					"description TEXT, "
					"notes TEXT, "
					"created BIGINT, "
					"modified BIGINT, "
					"managementPolicy VARCHAR(64) NOT NULL, "
					"users TEXT, "
					"inUse TEXT, "
					"tags TEXT, "
					"entity TEXT, "
					"venue TEXT, "
					"CONSTRAINT fk_roles_management_policy FOREIGN KEY (managementPolicy) REFERENCES policies(id) ON DELETE RESTRICT"
					");";
				Session << Statement, Poco::Data::Keywords::now;

				std::string IndexStatement =
					"CREATE INDEX IF NOT EXISTS roles_name_index ON " + TableName_ + " (name);";
				Session << IndexStatement, Poco::Data::Keywords::now;

				std::string PolicyIndexStatement =
					"CREATE INDEX IF NOT EXISTS roles_management_policy_idx ON " + TableName_ + " (managementPolicy);";
				Session << PolicyIndexStatement, Poco::Data::Keywords::now;
			}
		} catch (const Poco::Exception &E) {
			Logger_.error("Failure to create ManagementRoleDB table resources.");
			Logger_.log(E);
			return false;
		}
		return DB::Upgrade();
	}

	bool ManagementRoleDB::Upgrade(uint32_t from, uint32_t &to) {
		to = from;

		// Step 1: Version 1 -> 2 (Add entity & venue columns)
		if (from < 2) {
			std::vector<std::string> v2Statements;
			if (Type_ == OpenWifi::DBType::pgsql) {
				v2Statements = {
					"alter table " + TableName_ + " add column if not exists entity text;",
					"alter table " + TableName_ + " add column if not exists venue text;"};
			} else if (Type_ == OpenWifi::DBType::mysql) {
				auto HasCol = [this](const std::string &col) -> bool {
					try {
						std::size_t count = 0;
						Poco::Data::Session Session = Pool_.get();
						std::string Q = "SELECT COUNT(*) FROM information_schema.columns "
										"WHERE table_schema = database() AND lower(table_name) = '" +
										Poco::toLower(TableName_) + "' AND lower(column_name) = '" +
										Poco::toLower(col) + "';";
						Session << Q, Poco::Data::Keywords::into(count), Poco::Data::Keywords::now;
						return count > 0;
					} catch (...) {
						return false;
					}
				};
				if (!HasCol("entity")) {
					v2Statements.push_back("alter table " + TableName_ + " add column entity text;");
				}
				if (!HasCol("venue")) {
					v2Statements.push_back("alter table " + TableName_ + " add column venue text;");
				}
			} else {
				v2Statements = {
					"alter table " + TableName_ + " add column entity text;",
					"alter table " + TableName_ + " add column venue text;"};
			}
			for (const auto &st : v2Statements) {
				try {
					auto Session = Pool_.get();
					Session << st, Poco::Data::Keywords::now;
				} catch (const Poco::Exception &E) {
					Logger_.log(E);
				}
			}
			to = 2;
		}

		// Step 2: Version 2 -> 3 (Auto-purge corrupt roles & add FK constraint)
		if (from < 3) {
			std::string PurgeCorruptRoles =
				"DELETE FROM " + TableName_ +
				" WHERE managementPolicy IS NULL OR managementPolicy = '' OR "
				"managementPolicy NOT IN (SELECT id FROM policies);";

			try {
				auto Session = Pool_.get();
				Session << PurgeCorruptRoles, Poco::Data::Keywords::now;
			} catch (const Poco::Exception &E) {
				Logger_.error(Poco::format("ManagementRoleDB::Upgrade: Auto-purge failed on table %s: %s",
										   TableName_, E.displayText()));
				return false;
			}

			if (Type_ == OpenWifi::DBType::pgsql || Type_ == OpenWifi::DBType::mysql) {
				// 1. Set NOT NULL on managementPolicy
				try {
					auto Session = Pool_.get();
					std::string NotNullQuery = (Type_ == OpenWifi::DBType::pgsql)
						? "alter table " + TableName_ + " alter column managementPolicy set not null;"
						: "alter table " + TableName_ + " modify managementPolicy varchar(64) not null;";
					Session << NotNullQuery, Poco::Data::Keywords::now;
				} catch (const Poco::Exception &E) {
					Logger_.error(Poco::format("ManagementRoleDB::Upgrade: Failed to set NOT NULL on %s.managementPolicy: %s",
											   TableName_, E.displayText()));
					return false;
				}

				// 2. Check if constraint already exists in catalog for active schema/database
				auto HasConstraint = [this](const std::string &ConstraintName) -> bool {
					try {
						std::size_t count = 0;
						Poco::Data::Session Session = Pool_.get();
						std::string SchemaFilter;
						if (Type_ == OpenWifi::DBType::pgsql) {
							SchemaFilter = " AND table_schema = current_schema()";
						} else if (Type_ == OpenWifi::DBType::mysql) {
							SchemaFilter = " AND table_schema = DATABASE()";
						}
						std::string CheckQ =
							"SELECT COUNT(*) FROM information_schema.table_constraints "
							"WHERE lower(table_name) = '" + Poco::toLower(TableName_) +
							"' AND lower(constraint_name) = '" + Poco::toLower(ConstraintName) + "'" +
							SchemaFilter + ";";
						Session << CheckQ, Poco::Data::Keywords::into(count), Poco::Data::Keywords::now;
						return count > 0;
					} catch (...) {
						return false;
					}
				};

				// 3. Add constraint if not already present
				if (!HasConstraint("fk_roles_management_policy")) {
					try {
						auto Session = Pool_.get();
						std::string AddFkQuery =
							"alter table " + TableName_ +
							" add constraint fk_roles_management_policy foreign key (managementPolicy) references policies(id) on delete restrict;";
						Session << AddFkQuery, Poco::Data::Keywords::now;
					} catch (const Poco::Exception &E) {
						Logger_.error(Poco::format("ManagementRoleDB::Upgrade: Failed to add foreign key constraint on table %s: %s",
												   TableName_, E.displayText()));
						return false;
					}
				}

				// 4. Post-Verification: Confirm constraint is registered in DB catalog
				if (!HasConstraint("fk_roles_management_policy")) {
					Logger_.error(Poco::format("ManagementRoleDB::Upgrade: Constraint fk_roles_management_policy is missing after migration on table %s",
											   TableName_));
					return false;
				}
			}

			// 5. Ensure index on managementPolicy exists for fast FK checks and HasPolicy lookups
			try {
				auto Session = Pool_.get();
				if (Type_ == OpenWifi::DBType::mysql) {
					uint64_t HasPolicyIndex = 0;
					std::string CheckPolicyIndex =
						"SELECT COUNT(*) FROM information_schema.statistics "
						"WHERE table_schema = DATABASE() AND table_name = '" + TableName_ + "' AND index_name = 'roles_management_policy_idx'";
					Session << CheckPolicyIndex, Poco::Data::Keywords::into(HasPolicyIndex), Poco::Data::Keywords::now;
					if (HasPolicyIndex == 0) {
						Session << "CREATE INDEX roles_management_policy_idx ON " + TableName_ + " (managementPolicy);", Poco::Data::Keywords::now;
					}
				} else {
					std::string PolicyIndexStatement =
						"CREATE INDEX IF NOT EXISTS roles_management_policy_idx ON " + TableName_ + " (managementPolicy);";
					Session << PolicyIndexStatement, Poco::Data::Keywords::now;
				}
			} catch (const Poco::Exception &E) {
				Logger_.error(Poco::format("ManagementRoleDB::Upgrade: Failed to create index roles_management_policy_idx on table %s: %s",
										   TableName_, E.displayText()));
				return false;
			}

			to = 3;
		}

		return true;
	}

	bool ManagementRoleDB::HasPolicy(const std::string &PolicyId, bool &InUse) {
		try {
			uint64_t Count = 0;
			Poco::Data::Session Session = Pool_.get();
			Poco::Data::Statement Select(Session);

			std::string St = "SELECT COUNT(*) FROM (SELECT 1 FROM " + TableName_ +
							 " WHERE managementPolicy=? LIMIT 1) AS t";
			auto tPolicyId{PolicyId};
			Select << ConvertParams(St), Poco::Data::Keywords::into(Count),
				Poco::Data::Keywords::use(tPolicyId);
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
