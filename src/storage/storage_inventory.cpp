//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include "storage_inventory.h"
#include <algorithm>
#include "RESTObjects/RESTAPI_SecurityObjects.h"
#include "SerialNumberCache.h"
#include "StorageService.h"
#include "fmt/format.h"
#include "framework/MicroServiceFuncs.h"
#include "framework/OpenWifiTypes.h"
#include "framework/RESTAPI_utils.h"
#include "framework/RetryableTransaction.h"
#include "framework/utils.h"
#include "nlohmann/json.hpp"
#include "sdks/SDK_gw.h"

namespace OpenWifi {

	namespace {
		constexpr const char *kSerialCanonicalUniqueIndex = "inventory_serial_canonical_unique_idx";
		constexpr const char *kCanonicalSerialExprSQL = "lower(serialnumber)";
	} // namespace

	static ORM::FieldVec InventoryDB_Fields{
		// object info
		ORM::Field{"id", 64, true},
		ORM::Field{"name", ORM::FieldType::FT_TEXT},
		ORM::Field{"description", ORM::FieldType::FT_TEXT},
		ORM::Field{"notes", ORM::FieldType::FT_TEXT},
		ORM::Field{"created", ORM::FieldType::FT_BIGINT},
		ORM::Field{"modified", ORM::FieldType::FT_BIGINT},
		ORM::Field{"serialNumber", ORM::FieldType::FT_TEXT},
		ORM::Field{"venue", ORM::FieldType::FT_TEXT},
		ORM::Field{"entity", ORM::FieldType::FT_TEXT},
		ORM::Field{"subscriber", ORM::FieldType::FT_TEXT},
		ORM::Field{"deviceType", ORM::FieldType::FT_TEXT},
		ORM::Field{"qrcode", ORM::FieldType::FT_TEXT},
		ORM::Field{"geocode", ORM::FieldType::FT_TEXT},
		ORM::Field{"location", ORM::FieldType::FT_TEXT},
		ORM::Field{"contact", ORM::FieldType::FT_TEXT},
		ORM::Field{"deviceConfiguration", ORM::FieldType::FT_TEXT},
		ORM::Field{"deviceRules", ORM::FieldType::FT_TEXT},
		ORM::Field{"tags", ORM::FieldType::FT_TEXT},
		ORM::Field{"managementPolicy", ORM::FieldType::FT_TEXT},
		ORM::Field{"state", ORM::FieldType::FT_TEXT},
		ORM::Field{"devClass", ORM::FieldType::FT_TEXT},
		ORM::Field{"locale", ORM::FieldType::FT_TEXT},
		ORM::Field{"realMacAddress", ORM::FieldType::FT_TEXT},
		ORM::Field{"doNotAllowOverrides", ORM::FieldType::FT_BOOLEAN},
        ORM::Field{"imported", ORM::FieldType::FT_BIGINT},
        ORM::Field{"connected", ORM::FieldType::FT_BIGINT},
        ORM::Field{"platform", ORM::FieldType::FT_TEXT}};

	static ORM::IndexVec InventoryDB_Indexes{
		{std::string("inventory_name_index"),
		 ORM::IndexEntryVec{{std::string("name"), ORM::Indextype::ASC}}},
		{std::string("inventory_serial_index"),
		 ORM::IndexEntryVec{{std::string("serialNumber"), ORM::Indextype::ASC}}}};

	bool InventoryDB::Upgrade([[maybe_unused]] uint32_t from, uint32_t &to) {
		to = Version();
		std::vector<std::string> Script{
			"alter table " + TableName_ + " add column state text",
			"alter table " + TableName_ + " add column locale varchar(16)",
			"alter table " + TableName_ + " add column realMacAddress text",
			"alter table " + TableName_ + " add column devClass text",
			"alter table " + TableName_ + " add column deviceRules text",
            "alter table " + TableName_ + " add column platform text default 'AP'",
            "alter table " + TableName_ + " add column imported bigint",
            "alter table " + TableName_ + " add column connected bigint",
			"alter table " + TableName_ + " add column doNotAllowOverrides boolean"};

		for (const auto &i : Script) {
			try {
				auto Session = Pool_.get();
				Session << i, Poco::Data::Keywords::now;
			} catch (...) {
			}
		}
		return EnsureSerialUniqueIndex();
	}

	bool InventoryDB::EnsureSerialUniqueIndex() {
		if (Type_ != OpenWifi::DBType::pgsql) {
			return true;
		}

		try {
			Poco::Data::Session Session = Pool_.get();

			// Create separately named unique index on lower(serialnumber), keeping inventory_serial_index intact.
			std::string CreateIndexSQL = fmt::format(
				"CREATE UNIQUE INDEX IF NOT EXISTS {} ON {} ({})",
				kSerialCanonicalUniqueIndex, TableName_, kCanonicalSerialExprSQL);
			Session << CreateIndexSQL, Poco::Data::Keywords::now;

			// Verify via PostgreSQL catalogs that the required unique index exists, is valid,
			// is not partial, has one key attribute, and covers lower(serialnumber).
			bool Verified = false;
			std::string VerifySQL = ConvertParams(
				"SELECT true "
				"FROM pg_index i "
				"JOIN pg_class idx_cls ON idx_cls.oid = i.indexrelid "
				"JOIN pg_class tbl_cls ON tbl_cls.oid = i.indrelid "
				"JOIN pg_namespace ns ON ns.oid = tbl_cls.relnamespace "
				"WHERE tbl_cls.oid = to_regclass(?) "
				"  AND idx_cls.oid = to_regclass(quote_ident(ns.nspname) || '.' || ?) "
				"  AND idx_cls.relnamespace = tbl_cls.relnamespace "
				"  AND i.indisunique AND i.indisvalid AND i.indpred IS NULL AND i.indnkeyatts = 1 "
				"  AND lower(replace(replace(pg_get_expr(i.indexprs, i.indrelid), ' ', ''), '\"', '')) "
				"      IN ('lower(serialnumber)', 'lower((serialnumber)::text)')");
			std::string UseTable = TableName_;
			std::string UseIndex = kSerialCanonicalUniqueIndex;

			Session << VerifySQL,
				Poco::Data::Keywords::into(Verified),
				Poco::Data::Keywords::use(UseTable),
				Poco::Data::Keywords::use(UseIndex),
				Poco::Data::Keywords::now;

			if (!Verified) {
				Logger().critical(fmt::format(
					"InventoryDB: PostgreSQL unique index '{}' on '{}' failed verification.",
					kSerialCanonicalUniqueIndex, TableName_));
				return false;
			}

			return true;
		} catch (const std::exception &E) {
			Logger().critical(fmt::format(
				"InventoryDB: Failed to create or verify unique index '{}' on '{}': {}",
				kSerialCanonicalUniqueIndex, TableName_, E.what()));
			return false;
		} catch (...) {
			Logger().critical(fmt::format(
				"InventoryDB: Failed to create or verify unique index '{}' on '{}': unknown exception",
				kSerialCanonicalUniqueIndex, TableName_));
			return false;
		}
	}

	namespace {
		template <typename ParentDBType, typename ParentRecordType>
		bool AddDeviceToParent(OpenWifi::DbTransaction &tx,
		                       OpenWifi::DBType dbType,
		                       ParentDBType &parentDb,
		                       const std::string &parentId,
		                       const std::string &deviceId,
		                       Poco::Logger &logger) {
			ParentRecordType parent;
			const bool loaded = (dbType == OpenWifi::DBType::pgsql)
				? parentDb.GetRecordForUpdate(tx, "id", parentId, parent)
				: parentDb.GetRecord(tx, "id", parentId, parent);
			if (!loaded) {
				logger.error(fmt::format(
					"CreateFromConnection could not load parent '{}' to add device '{}'.",
					parentId, deviceId));
				return false;
			}
			if (std::find(parent.devices.begin(), parent.devices.end(), deviceId) == parent.devices.end()) {
				parent.devices.push_back(deviceId);
				std::sort(parent.devices.begin(), parent.devices.end());
				if (!parentDb.UpdateRecord(tx, "id", parentId, parent)) {
					logger.error(fmt::format(
						"CreateFromConnection could not update parent '{}' membership for device '{}'.",
						parentId, deviceId));
					return false;
				}
			}
			return true;
		}
	} // namespace

	InventoryDB::InventoryDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L)
		: DB(T, "inventory", InventoryDB_Fields, InventoryDB_Indexes, P, L, "inv") {}

	bool InventoryDB::CreateFromConnection(const std::string &SerialNumberRaw,
										   const std::string &ConnectionInfo,
										   const std::string &DeviceType,
										   const std::string &Locale,
										   const bool isConnection) {

		const std::string SerialNumber = Poco::toLower(SerialNumberRaw);
		if (SerialNumber.empty()) {
			Logger().error("CreateFromConnection called with empty serial number.");
			return false;
		}

		// Check inventory existence before entering transaction to keep venue/entity
		// discovery outside the active PostgreSQL transaction and avoid holding locks
		// or exhausting session-pool connections during IP scans.
		bool PreflightFound = false;
		try {
			Poco::Data::Session PreflightSession = Pool_.get();
			std::string PreflightId;
			std::string key = SerialNumber;
			Poco::Data::Statement select(PreflightSession);
			select << ConvertParams(fmt::format(
				"SELECT id FROM {} WHERE {} = ? LIMIT 1",
				TableName_, kCanonicalSerialExprSQL)),
				Poco::Data::Keywords::into(PreflightId),
				Poco::Data::Keywords::use(key);
			PreflightFound = (select.execute() == 1 && !PreflightId.empty());
		} catch (const Poco::Exception &E) {
			Logger().error(fmt::format(
				"CreateFromConnection preflight lookup database error for '{}': {}",
				SerialNumber, E.displayText()));
			return false;
		} catch (const std::exception &E) {
			Logger().error(fmt::format(
				"CreateFromConnection preflight lookup database error for '{}': {}",
				SerialNumber, E.what()));
			return false;
		} catch (...) {
			Logger().error(fmt::format(
				"CreateFromConnection preflight lookup database error for '{}': unknown exception",
				SerialNumber));
			return false;
		}

		std::string MatchedVenue;
		std::string MatchedEntity;
		if (!PreflightFound) {
			auto Tokens = Poco::StringTokenizer(ConnectionInfo, "@:");
			if (Tokens.count() == 3 && !Tokens[1].empty()) {
				const std::string &IP = Tokens[1];
				StorageService()->VenueDB().GetByIP(IP, MatchedVenue);
				if (MatchedVenue.empty()) {
					StorageService()->EntityDB().GetByIP(IP, MatchedEntity);
				}
			}
		}

		bool CreatedNew = false;
		ProvObjects::InventoryTag CommittedDevice;

		RetryableTransactionOptions TxOpts;
		TxOpts.logContext = "inventory_create_from_connection";

		for (int Pass = 0; Pass < 2; ++Pass) {
			auto Res = RunRetryableTransaction(
				Logger(),
				TxOpts,
				[&]() { return StorageService()->BeginTransaction(); },
				[&](DbTransaction &tx) -> bool {
					CreatedNew = false;
					CommittedDevice = ProvObjects::InventoryTag{};

					std::string ExistingId;
					try {
						Poco::Data::Statement select(tx.Session());
						std::string key = SerialNumber;
						select << ConvertParams(fmt::format(
							"SELECT id FROM {} WHERE {} = ? LIMIT 1",
							TableName_, kCanonicalSerialExprSQL)),
							Poco::Data::Keywords::into(ExistingId),
							Poco::Data::Keywords::use(key);
						select.execute();
					} catch (const Poco::Exception &E) {
						tx.MarkFailed(E);
						return false;
					} catch (const std::exception &E) {
						tx.MarkFailed("", E.what());
						return false;
					} catch (...) {
						tx.MarkFailed();
						return false;
					}

					if (ExistingId.empty()) {
						// If preflight found a record that was deleted before this transaction,
						// fail this attempt so Kafka retries with full IP discovery.
						if (PreflightFound) {
							Logger().warning(fmt::format(
								"CreateFromConnection: device '{}' was present during preflight but deleted before transaction; "
								"failing for retry with discovery.",
								SerialNumber));
							return false;
						}

						ProvObjects::InventoryTag NewDevice;
						uint64_t Now = Utils::Now();
						NewDevice.info.id = MicroServiceCreateUUID();
						NewDevice.info.name = SerialNumber;
						NewDevice.info.created = NewDevice.info.modified = Now;
						NewDevice.info.notes.push_back(SecurityObjects::NoteInfo{
							.created = Now, .createdBy = "*system", .note = "Auto discovered"});
						NewDevice.serialNumber = SerialNumber;
						NewDevice.deviceType = DeviceType;
						NewDevice.locale = Locale;
						nlohmann::json StateDoc;
						StateDoc["method"] = "auto-discovery";
						StateDoc["date"] = Now;
						NewDevice.state = to_string(StateDoc);
						NewDevice.devClass = "any";
						NewDevice.connected = Now;
						NewDevice.imported = 0;
						NewDevice.venue = MatchedVenue;
						NewDevice.entity = MatchedEntity;

						if (!CreateRecord(tx, NewDevice)) {
							return false;
						}

						if (!NewDevice.entity.empty()) {
							if (!AddDeviceToParent<EntityDB, ProvObjects::Entity>(
									tx, Type_, StorageService()->EntityDB(), NewDevice.entity, NewDevice.info.id, Logger())) {
								return false;
							}
						} else if (!NewDevice.venue.empty()) {
							if (!AddDeviceToParent<VenueDB, ProvObjects::Venue>(
									tx, Type_, StorageService()->VenueDB(), NewDevice.venue, NewDevice.info.id, Logger())) {
								return false;
							}
						}

						CreatedNew = true;
						CommittedDevice = std::move(NewDevice);
						return true;
					}

					ProvObjects::InventoryTag ExistingDevice;
					const bool Loaded = (Type_ == OpenWifi::DBType::pgsql)
						? GetRecordForUpdate(tx, "id", ExistingId, ExistingDevice)
						: GetRecord(tx, "id", ExistingId, ExistingDevice);
					if (!Loaded) {
						return false;
					}

					// Device already exists, do we need to modify anything?
					bool modified = false;
					if (ExistingDevice.deviceType != DeviceType) {
						ExistingDevice.deviceType = DeviceType;
						modified = true;
					}

					// if this device is being claimed, now it is claimed.
					if (!ExistingDevice.state.empty()) {
						auto State = nlohmann::json::parse(ExistingDevice.state);
						if (State["method"] == "claiming") {
							uint64_t Date = State["date"];
							uint64_t Now = Utils::Now();

							if ((Now - Date) < (24 * 60 * 60)) {
								State["method"] = "claimed";
								State["date"] = Utils::Now();
								ExistingDevice.state = to_string(State);
								modified = true;
							} else {
								ExistingDevice.state = "";
								modified = true;
							}
						}
					} else {
						ExistingDevice.devClass = "any";
						modified = true;
					}

					if (Locale != ExistingDevice.locale) {
						ExistingDevice.locale = Locale;
						modified = true;
					}

					if (modified) {
						ExistingDevice.info.modified = Utils::Now();
						ExistingDevice.connected = Utils::Now();
						if (!UpdateRecord(tx, "id", ExistingDevice.info.id, ExistingDevice)) {
							Logger().error(Poco::format("Could not update %s in inventory.", SerialNumber));
							return false;
						}
					}

					CreatedNew = false;
					CommittedDevice = std::move(ExistingDevice);
					return true;
				});

			if (Res.Succeeded()) {
				break;
			}

			if (Res.status == RetryableTransactionStatus::UniqueViolation && Pass == 0) {
				Logger().information(fmt::format(
					"CreateFromConnection concurrent insert conflict for '{}' (SQLSTATE 23505); "
					"re-reading existing record in fresh transaction.",
					SerialNumber));
				continue;
			}

			Logger().error(fmt::format("CreateFromConnection failed for '{}': {}",
				SerialNumber, Res.lastFailureMessage));
			return false;
		}

		if (CreatedNew) {
			SerialNumberCache()->AddSerialNumber(CommittedDevice.serialNumber, CommittedDevice.deviceType);

			std::string FullUUID;
			if (!CommittedDevice.entity.empty()) {
				FullUUID = StorageService()->EntityDB().Prefix() + ":" + CommittedDevice.entity;
			} else if (!CommittedDevice.venue.empty()) {
				FullUUID = StorageService()->VenueDB().Prefix() + ":" + CommittedDevice.venue;
			}

			if (!FullUUID.empty()) {
				if (SDK::GW::Device::SetVenue(nullptr, CommittedDevice.serialNumber, FullUUID)) {
					Logger().information(Poco::format("%s: GW set entity/venue property.",
													  CommittedDevice.serialNumber));
				} else {
					Logger().information(Poco::format(
						"%s: could not set GW entity/venue property.", CommittedDevice.serialNumber));
				}
			}
			Logger().information(Poco::format("Adding %s to inventory.", SerialNumber));
			return true;
		}

		// Push ownership properties in a single GW PUT, only on connect (not ping).
		if (isConnection && (!CommittedDevice.venue.empty() || !CommittedDevice.entity.empty() ||
							 !CommittedDevice.subscriber.empty())) {
			if (SDK::GW::Device::SetOwnerShip(nullptr, CommittedDevice.serialNumber,
											  CommittedDevice.entity, CommittedDevice.venue,
											  CommittedDevice.subscriber)) {
				Logger().information(
					Poco::format("%s: GW set ownership properties (entity/venue/subscriber).",
								 CommittedDevice.serialNumber));
			} else {
				Logger().information(Poco::format(
					"%s: could not set GW ownership properties (entity/venue/subscriber).",
					CommittedDevice.serialNumber));
			}
		}

		return true;
	}

	bool InventoryDB::EvaluateDeviceIDRules(const std::string &id,
											ProvObjects::DeviceRules &Rules) {
		ProvObjects::InventoryTag T;
		if (GetRecord("id", id, T))
			return EvaluateDeviceRules(T, Rules);
		return false;
	}

	bool InventoryDB::EvaluateDeviceSerialNumberRules(const std::string &serialNumber,
													  ProvObjects::DeviceRules &Rules) {
		ProvObjects::InventoryTag T;
		if (GetRecord("serialNumber", serialNumber, T))
			return EvaluateDeviceRules(T, Rules);
		return false;
	}

	bool InventoryDB::EvaluateDeviceRules(const ProvObjects::InventoryTag &T,
										  ProvObjects::DeviceRules &Rules) {
		Rules = T.deviceRules;
		if (!T.venue.empty())
			return StorageService()->VenueDB().EvaluateDeviceRules(T.venue, Rules);
		if (!T.entity.empty())
			return StorageService()->EntityDB().EvaluateDeviceRules(T.venue, Rules);
		return Storage::ApplyConfigRules(Rules);
	}

	void InventoryDB::InitializeSerialCache() {
		auto F = [](const ProvObjects::InventoryTag &T) -> bool {
			SerialNumberCache()->AddSerialNumber(T.serialNumber, T.deviceType);
			return true;
		};
		Iterate(F);
	}

	bool InventoryDB::GetRRMDeviceList(Types::UUIDvec_t &DeviceList) {
		// get a local copy of the cache - this could be expensive.
		auto C = SerialNumberCache()->GetCacheCopy();

		for (const auto &i : C) {
			ProvObjects::InventoryTag Tag;
			ProvObjects::DeviceRules Rules;
			std::string SerialNumber = Utils::IntToSerialNumber(i);
			if (EvaluateDeviceSerialNumberRules(SerialNumber, Rules)) {
				if (Rules.rrm != "no" && Rules.rrm != "inherit")
					DeviceList.push_back(SerialNumber);
			}
		}
		return true;
	}

    bool InventoryDB::GetDevicesForVenue(const std::string &venue_uuid, std::vector<std::string> &devices) {
        try {
            Iterate([&](const ProvObjects::InventoryTag &tag) {
                devices.push_back(tag.serialNumber);
                return true;
            }, fmt::format(" venue='{}' ", venue_uuid));
            return true;
        } catch(const Poco::Exception &E) {
            Logger().log(E);
            return false;
        } catch(const std::exception &E) {
            Logger().error(fmt::format("std::exception: {}",E.what()));
            return false;
        } catch(...) {
            Logger().error("Unknown exception");
            return false;
        }
        return false;
    }

    bool InventoryDB::GetDevicesUUIDForVenue(const std::string &venue_uuid, std::vector<std::string> &devices) {
        try {
            Iterate([&](const ProvObjects::InventoryTag &tag) {
                devices.push_back(tag.info.id);
                return true;
            }, fmt::format(" venue='{}' ", venue_uuid));
            return true;
        } catch(const Poco::Exception &E) {
            Logger().log(E);
            return false;
        } catch(const std::exception &E) {
            Logger().error(fmt::format("std::exception: {}",E.what()));
            return false;
        } catch(...) {
            Logger().error("Unknown exception");
            return false;
        }
        return false;
    }

    bool InventoryDB::GetDevicesForVenue(const std::string &venue_uuid, std::vector<ProvObjects::InventoryTag> &devices) {
        try {
            Iterate([&](const ProvObjects::InventoryTag &tag) {
                devices.push_back(tag);
                return true;
            }, fmt::format(" venue='{}' ", venue_uuid));
            return true;
        } catch(const Poco::Exception &E) {
            Logger().log(E);
            return false;
        } catch(const std::exception &E) {
            Logger().error(fmt::format("std::exception: {}",E.what()));
            return false;
        } catch(...) {
            Logger().error("Unknown exception");
            return false;
        }

        return false;
    }


} // namespace OpenWifi

template <>
void ORM::DB<OpenWifi::InventoryDBRecordType, OpenWifi::ProvObjects::InventoryTag>::Convert(
	const OpenWifi::InventoryDBRecordType &In, OpenWifi::ProvObjects::InventoryTag &Out) {
	Out.info.id = In.get<0>();
	Out.info.name = In.get<1>();
	Out.info.description = In.get<2>();
	Out.info.notes =
		OpenWifi::RESTAPI_utils::to_object_array<OpenWifi::SecurityObjects::NoteInfo>(In.get<3>());
	Out.info.created = In.get<4>();
	Out.info.modified = In.get<5>();
	Out.serialNumber = In.get<6>();
	Out.venue = In.get<7>();
	Out.entity = In.get<8>();
	Out.subscriber = In.get<9>();
	Out.deviceType = In.get<10>();
	Out.qrCode = In.get<11>();
	Out.geoCode = In.get<12>();
	Out.location = In.get<13>();
	Out.contact = In.get<14>();
	Out.deviceConfiguration = In.get<15>();
	Out.deviceRules =
		OpenWifi::RESTAPI_utils::to_object<OpenWifi::ProvObjects::DeviceRules>(In.get<16>());
	Out.info.tags = OpenWifi::RESTAPI_utils::to_taglist(In.get<17>());
	Out.managementPolicy = In.get<18>();
	Out.state = In.get<19>();
	Out.devClass = In.get<20>();
	Out.locale = In.get<21>();
	Out.realMacAddress = In.get<22>();
	Out.doNotAllowOverrides = In.get<23>();
    Out.imported = In.get<24>();
    Out.connected = In.get<25>();
    Out.platform = In.get<26>();
}

template <>
void ORM::DB<OpenWifi::InventoryDBRecordType, OpenWifi::ProvObjects::InventoryTag>::Convert(
	const OpenWifi::ProvObjects::InventoryTag &In, OpenWifi::InventoryDBRecordType &Out) {
	Out.set<0>(In.info.id);
	Out.set<1>(In.info.name);
	Out.set<2>(In.info.description);
	Out.set<3>(OpenWifi::RESTAPI_utils::to_string(In.info.notes));
	Out.set<4>(In.info.created);
	Out.set<5>(In.info.modified);
	Out.set<6>(In.serialNumber);
	Out.set<7>(In.venue);
	Out.set<8>(In.entity);
	Out.set<9>(In.subscriber);
	Out.set<10>(In.deviceType);
	Out.set<11>(In.qrCode);
	Out.set<12>(In.geoCode);
	Out.set<13>(In.location);
	Out.set<14>(In.contact);
	Out.set<15>(In.deviceConfiguration);
	Out.set<16>(OpenWifi::RESTAPI_utils::to_string(In.deviceRules));
	Out.set<17>(OpenWifi::RESTAPI_utils::to_string(In.info.tags));
	Out.set<18>(In.managementPolicy);
	Out.set<19>(In.state);
	Out.set<20>(In.devClass);
	Out.set<21>(In.locale);
	Out.set<22>(In.realMacAddress);
	Out.set<23>(In.doNotAllowOverrides);
    Out.set<24>(In.imported);
    Out.set<25>(In.connected);
    Out.set<26>(In.platform);
}
