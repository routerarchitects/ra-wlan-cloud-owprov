/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include "StorageService.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "fmt/format.h"
#include "framework/utils.h"

namespace OpenWifi {

	int Storage::Start() {
		poco_information(Logger(), "Starting...");
		std::lock_guard Guard(Mutex_);

		StorageClass::Start();

		EntityDB_ = std::make_unique<OpenWifi::EntityDB>(dbType_, *Pool_, Logger());
		PolicyDB_ = std::make_unique<OpenWifi::PolicyDB>(dbType_, *Pool_, Logger());
		VenueDB_ = std::make_unique<OpenWifi::VenueDB>(dbType_, *Pool_, Logger());
		LocationDB_ = std::make_unique<OpenWifi::LocationDB>(dbType_, *Pool_, Logger());
		ContactDB_ = std::make_unique<OpenWifi::ContactDB>(dbType_, *Pool_, Logger());
		InventoryDB_ = std::make_unique<OpenWifi::InventoryDB>(dbType_, *Pool_, Logger());
		RolesDB_ = std::make_unique<OpenWifi::ManagementRoleDB>(dbType_, *Pool_, Logger());
		ConfigurationDB_ = std::make_unique<OpenWifi::ConfigurationDB>(dbType_, *Pool_, Logger());
		TagsDictionaryDB_ = std::make_unique<OpenWifi::TagsDictionaryDB>(dbType_, *Pool_, Logger());
		TagsObjectDB_ = std::make_unique<OpenWifi::TagsObjectDB>(dbType_, *Pool_, Logger());
		MapDB_ = std::make_unique<OpenWifi::MapDB>(dbType_, *Pool_, Logger());
		SignupDB_ = std::make_unique<OpenWifi::SignupDB>(dbType_, *Pool_, Logger());
		VariablesDB_ = std::make_unique<OpenWifi::VariablesDB>(dbType_, *Pool_, Logger());
		OperatorDB_ = std::make_unique<OpenWifi::OperatorDB>(dbType_, *Pool_, Logger());
		ServiceClassDB_ = std::make_unique<OpenWifi::ServiceClassDB>(dbType_, *Pool_, Logger());
		SubscriberDeviceDB_ =
			std::make_unique<OpenWifi::SubscriberDeviceDB>(dbType_, *Pool_, Logger());
		OpLocationDB_ = std::make_unique<OpenWifi::OpLocationDB>(dbType_, *Pool_, Logger());
		OpContactDB_ = std::make_unique<OpenWifi::OpContactDB>(dbType_, *Pool_, Logger());
		OverridesDB_ = std::make_unique<OpenWifi::OverridesDB>(dbType_, *Pool_, Logger());
        GLBLRAccountInfoDB_ = std::make_unique<OpenWifi::GLBLRAccountInfoDB>(dbType_, *Pool_, Logger());
        GLBLRCertsDB_ = std::make_unique<OpenWifi::GLBLRCertsDB>(dbType_, *Pool_, Logger());
        OrionAccountsDB_ = std::make_unique<OpenWifi::OrionAccountsDB>(dbType_, *Pool_, Logger());
        RadiusEndpointDB_ = std::make_unique<OpenWifi::RadiusEndpointDB>(dbType_, *Pool_, Logger());

#ifdef CGW_INTEGRATION
        GroupsMapDB_ = std::make_unique<OpenWifi::GroupsMapDB>(dbType_, *Pool_, Logger());
#endif
		EntityDB_->Create();
		PolicyDB_->Create();
		VenueDB_->Create();
		LocationDB_->Create();
		ContactDB_->Create();
		InventoryDB_->Create();
		RolesDB_->Create();
		ConfigurationDB_->Create();
		TagsDictionaryDB_->Create();
		TagsObjectDB_->Create();
		MapDB_->Create();
		SignupDB_->Create();
		VariablesDB_->Create();
		OperatorDB_->Create();
		ServiceClassDB_->Create();
		SubscriberDeviceDB_->Create();
		OpLocationDB_->Create();
		OpContactDB_->Create();
		OverridesDB_->Create();
        GLBLRAccountInfoDB_->Create();
        GLBLRCertsDB_->Create();
        OrionAccountsDB_->Create();
        RadiusEndpointDB_->Create();

#ifdef CGW_INTEGRATION
        GroupsMapDB_->Create();
#endif
		ExistFunc_[EntityDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
			return EntityDB_->Exists(F, V);
		};
		ExistFunc_[PolicyDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
			return PolicyDB_->Exists(F, V);
		};
		ExistFunc_[VenueDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
			return VenueDB_->Exists(F, V);
		};
		ExistFunc_[ContactDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
			return ContactDB_->Exists(F, V);
		};
		ExistFunc_[InventoryDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
			return InventoryDB_->Exists(F, V);
		};
		ExistFunc_[ConfigurationDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
			return ConfigurationDB_->Exists(F, V);
		};
		ExistFunc_[LocationDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
			return LocationDB_->Exists(F, V);
		};
		ExistFunc_[RolesDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
			return RolesDB_->Exists(F, V);
		};
		ExistFunc_[TagsDictionaryDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
			return TagsDictionaryDB_->Exists(F, V);
		};
		ExistFunc_[TagsObjectDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
			return TagsObjectDB_->Exists(F, V);
		};
		ExistFunc_[MapDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
			return MapDB_->Exists(F, V);
		};
		ExistFunc_[SignupDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
			return SignupDB_->Exists(F, V);
		};
		ExistFunc_[VariablesDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
			return VariablesDB_->Exists(F, V);
		};
		ExistFunc_[OperatorDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
			return OperatorDB_->Exists(F, V);
		};
		ExistFunc_[ServiceClassDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
			return ServiceClassDB_->Exists(F, V);
		};
		ExistFunc_[SubscriberDeviceDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
			return SubscriberDeviceDB_->Exists(F, V);
		};
		ExistFunc_[OpLocationDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
			return OpLocationDB_->Exists(F, V);
		};
		ExistFunc_[OpContactDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
			return OpContactDB_->Exists(F, V);
		};
		ExistFunc_[OverridesDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
			return OverridesDB_->Exists(F, V);
		};
        ExistFunc_[GLBLRAccountInfoDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
            return GLBLRAccountInfoDB_->Exists(F, V);
        };
        ExistFunc_[GLBLRCertsDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
            return GLBLRCertsDB_->Exists(F, V);
        };
        ExistFunc_[OrionAccountsDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
            return OrionAccountsDB_->Exists(F, V);
        };
        ExistFunc_[RadiusEndpointDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
            return RadiusEndpointDB_->Exists(F, V);
        };

#ifdef CGW_INTEGRATION
        ExistFunc_[GroupsMapDB_->Prefix()] = [=](const char *F, std::string &V) -> bool {
            return GroupsMapDB_->Exists(F, V);
        };
#endif


        ExpandFunc_[EntityDB_->Prefix()] = [=](const char *F, std::string &V, std::string &Name,
											   std::string &Description) -> bool {
			return EntityDB_->GetNameAndDescription(F, V, Name, Description);
		};
		ExpandFunc_[PolicyDB_->Prefix()] = [=](const char *F, std::string &V, std::string &Name,
											   std::string &Description) -> bool {
			return PolicyDB_->GetNameAndDescription(F, V, Name, Description);
		};
		ExpandFunc_[VenueDB_->Prefix()] = [=](const char *F, std::string &V, std::string &Name,
											  std::string &Description) -> bool {
			return VenueDB_->GetNameAndDescription(F, V, Name, Description);
		};
		ExpandFunc_[ContactDB_->Prefix()] = [=](const char *F, std::string &V, std::string &Name,
												std::string &Description) -> bool {
			return ContactDB_->GetNameAndDescription(F, V, Name, Description);
		};
		ExpandFunc_[InventoryDB_->Prefix()] = [=](const char *F, std::string &V, std::string &Name,
												  std::string &Description) -> bool {
			return InventoryDB_->GetNameAndDescription(F, V, Name, Description);
		};
		ExpandFunc_[ConfigurationDB_->Prefix()] = [=](const char *F, std::string &V,
													  std::string &Name,
													  std::string &Description) -> bool {
			return ConfigurationDB_->GetNameAndDescription(F, V, Name, Description);
		};
		ExpandFunc_[LocationDB_->Prefix()] = [=](const char *F, std::string &V, std::string &Name,
												 std::string &Description) -> bool {
			return LocationDB_->GetNameAndDescription(F, V, Name, Description);
		};
		ExpandFunc_[RolesDB_->Prefix()] = [=](const char *F, std::string &V, std::string &Name,
											  std::string &Description) -> bool {
			return RolesDB_->GetNameAndDescription(F, V, Name, Description);
		};
		ExpandFunc_[TagsDictionaryDB_->Prefix()] =
			[=](const char *F, std::string &V, [[maybe_unused]] std::string &Name,
				[[maybe_unused]] std::string &Description) -> bool {
			return TagsDictionaryDB_->Exists(F, V);
		};
		ExpandFunc_[TagsObjectDB_->Prefix()] =
			[=](const char *F, std::string &V, [[maybe_unused]] std::string &Name,
				[[maybe_unused]] std::string &Description) -> bool {
			return TagsObjectDB_->Exists(F, V);
			;
		};
		ExpandFunc_[MapDB_->Prefix()] = [=](const char *F, std::string &V,
											[[maybe_unused]] std::string &Name,
											[[maybe_unused]] std::string &Description) -> bool {
			return MapDB_->Exists(F, V);
			;
		};
		ExpandFunc_[SignupDB_->Prefix()] = [=](const char *F, std::string &V,
											   [[maybe_unused]] std::string &Name,
											   [[maybe_unused]] std::string &Description) -> bool {
			return SignupDB_->Exists(F, V);
			;
		};
		ExpandFunc_[VariablesDB_->Prefix()] =
			[=](const char *F, std::string &V, [[maybe_unused]] std::string &Name,
				[[maybe_unused]] std::string &Description) -> bool {
			return VariablesDB_->Exists(F, V);
			;
		};
		ExpandFunc_[OperatorDB_->Prefix()] = [=](const char *F, std::string &V, std::string &Name,
												 std::string &Description) -> bool {
			return OperatorDB_->GetNameAndDescription(F, V, Name, Description);
		};
		ExpandFunc_[ServiceClassDB_->Prefix()] = [=](const char *F, std::string &V,
													 std::string &Name,
													 std::string &Description) -> bool {
			return ServiceClassDB_->GetNameAndDescription(F, V, Name, Description);
		};
		ExpandFunc_[SubscriberDeviceDB_->Prefix()] = [=](const char *F, std::string &V,
														 std::string &Name,
														 std::string &Description) -> bool {
			return SubscriberDeviceDB_->GetNameAndDescription(F, V, Name, Description);
		};
		ExpandFunc_[OpLocationDB_->Prefix()] = [=](const char *F, std::string &V, std::string &Name,
												   std::string &Description) -> bool {
			return OpLocationDB_->GetNameAndDescription(F, V, Name, Description);
		};
		ExpandFunc_[OpContactDB_->Prefix()] = [=](const char *F, std::string &V, std::string &Name,
												  std::string &Description) -> bool {
			return OpContactDB_->GetNameAndDescription(F, V, Name, Description);
		};
		ExpandFunc_[OverridesDB_->Prefix()] =
			[=]([[maybe_unused]] const char *F, [[maybe_unused]] std::string &V,
				[[maybe_unused]] std::string &Name,
				[[maybe_unused]] std::string &Description) -> bool { return false; };
        ExpandFunc_[GLBLRAccountInfoDB_->Prefix()] =
                [=]([[maybe_unused]] const char *F, [[maybe_unused]] std::string &V,
                    [[maybe_unused]] std::string &Name,
                    [[maybe_unused]] std::string &Description) -> bool { return false; };
        ExpandFunc_[OverridesDB_->Prefix()] =
                [=]([[maybe_unused]] const char *F, [[maybe_unused]] std::string &V,
                    [[maybe_unused]] std::string &Name,
                    [[maybe_unused]] std::string &Description) -> bool { return false; };
        ExpandFunc_[GLBLRCertsDB_->Prefix()] =
                [=]([[maybe_unused]] const char *F, [[maybe_unused]] std::string &V,
                    [[maybe_unused]] std::string &Name,
                    [[maybe_unused]] std::string &Description) -> bool { return false; };
        ExpandFunc_[OrionAccountsDB_->Prefix()] =
                [=]([[maybe_unused]] const char *F, [[maybe_unused]] std::string &V,
                    [[maybe_unused]] std::string &Name,
                    [[maybe_unused]] std::string &Description) -> bool { return false; };

        ExpandFunc_[RadiusEndpointDB_->Prefix()] =
                [=]([[maybe_unused]] const char *F, [[maybe_unused]] std::string &V,
                    [[maybe_unused]] std::string &Name,
                    [[maybe_unused]] std::string &Description) -> bool { return false; };

        InventoryDB_->InitializeSerialCache();
		ConsistencyCheck();
		InitializeSystemDBs();

		TimerCallback_ = std::make_unique<Poco::TimerCallback<Storage>>(*this, &Storage::onTimer);
		Timer_.setStartInterval(20 * 1000);				// first run in 20 seconds
		Timer_.setPeriodicInterval(1 * 60 * 60 * 1000); // 1 hours
		Timer_.start(*TimerCallback_);
		return 0;
	}

	void Storage::onTimer([[maybe_unused]] Poco::Timer &timer) {
		Utils::SetThreadName("strg-janitor");
	}

	void Storage::Stop() {
		poco_information(Logger(), "Stopping...");
		Timer_.stop();
		poco_information(Logger(), "Stopped...");
	}

	bool Storage::Validate(const Poco::URI::QueryParameters &P, RESTAPI::Errors::msg &Error) {
		for (const auto &i : P) {
			auto uuid_parts = Utils::Split(i.second, ':');
			if (uuid_parts.size() == 2) {
				auto F = ExistFunc_.find(uuid_parts[0]);
				if (F != ExistFunc_.end()) {
					if (!F->second("id", uuid_parts[1])) {
						Error = RESTAPI::Errors::UnknownId;
						return false;
					}
				}
			}
		}
		return true;
	}

	bool Storage::ValidateSingle(const std::string &P, std::string &Error) {
		auto uuid_parts = Utils::Split(P, ':');
		if (uuid_parts.size() == 2) {
			auto F = ExistFunc_.find(uuid_parts[0]);
			if (F != ExistFunc_.end()) {
				if (!F->second("id", uuid_parts[1])) {
					Error = "Unknown " + F->first + " UUID:" + uuid_parts[1];
					return false;
				} else {
					return true;
				}
			} else {
				return false;
			}
		} else {
			return false;
		}
	}

	bool Storage::Validate(const Types::StringVec &P, std::string &Error) {
		for (const auto &i : P) {
			if (!ValidateSingle(i, Error))
				return false;
		}
		if (Error.empty())
			return true;
		return false;
	}

	bool Storage::Validate(const std::string &P) {
		std::string Error;
		return ValidateSingle(P, Error);
	}

	bool Storage::ExpandInUse(const Types::StringVec &UUIDs, ExpandedListMap &Map,
							  std::vector<std::string> &Errors) {
		for (const auto &i : UUIDs) {
			auto uuid_parts = Utils::Split(i, ':');
			if (uuid_parts.size() == 2) {
				auto F = ExpandFunc_.find(uuid_parts[0]);
				if (F != ExpandFunc_.end()) {
					std::string Name, Description;
					if (!F->second("id", uuid_parts[1], Name, Description)) {
						Errors.push_back(i);
					} else {
						auto Hint = Map.find(uuid_parts[0]);
						ProvObjects::ExpandedUseEntry X{
							.uuid = uuid_parts[1], .name = Name, .description = Description};
						if (Hint == Map.end()) {
							ProvObjects::ExpandedUseEntryList L;
							L.type = uuid_parts[0];
							L.entries.push_back(X);
							Map[uuid_parts[0]] = L;
						} else {
							Hint->second.entries.push_back(X);
						}
					}
				}
			}
		}
		return true;
	}

	void Storage::ConsistencyCheck() {

		// check that all inventory in venues and entities actually exists, if not, fix it.
		auto FixVenueDevices = [&](const ProvObjects::Venue &V) -> bool {
			Types::UUIDvec_t NewDevices;
			bool modified = false;
			for (const auto &device : V.devices) {
				ProvObjects::InventoryTag T;
				if (InventoryDB().GetRecord("id", device, T)) {
					NewDevices.emplace_back(device);
				} else {
					modified = true;
				}
			}

			ProvObjects::Venue NewVenue = V;
			if (V.deviceRules.rrm == "yes") {
				NewVenue.deviceRules.rrm = "inherit";
				modified = true;
			}

			if (modified) {
				poco_warning(Logger(), fmt::format("  fixing venue: {}", V.info.name));
				NewVenue.devices = NewDevices;
				VenueDB().UpdateRecord("id", V.info.id, NewVenue);
			}

			return true;
		};

		auto FixEntity = [&](const ProvObjects::Entity &E) -> bool {
			Types::UUIDvec_t NewDevices;
			bool Modified = false;
			for (const auto &device : E.devices) {
				ProvObjects::InventoryTag T;
				if (InventoryDB().GetRecord("id", device, T)) {
					NewDevices.emplace_back(device);
				} else {
					Modified = true;
				}
			}

			Types::UUIDvec_t NewContacts;
			for (const auto &contact : E.contacts) {
				ProvObjects::Contact C;
				if (ContactDB().GetRecord("id", contact, C)) {
					NewContacts.emplace_back(contact);
				} else {
					Modified = true;
				}
			}

			Types::UUIDvec_t NewLocations;
			for (const auto &location : E.locations) {
				ProvObjects::Location L;
				if (LocationDB().GetRecord("id", location, L)) {
					NewLocations.emplace_back(location);
				} else {
					Modified = true;
				}
			}

			Types::UUIDvec_t NewVenues;
			for (const auto &venue : E.venues) {
				ProvObjects::Venue V;
				if (VenueDB().GetRecord("id", venue, V)) {
					NewVenues.emplace_back(venue);
				} else {
					Modified = true;
				}
			}

			Types::UUIDvec_t NewVariables;
			for (const auto &variable : E.variables) {
				ProvObjects::VariableBlock V;
				if (VariablesDB().GetRecord("id", variable, V)) {
					NewVariables.emplace_back(variable);
				} else {
					Modified = true;
				}
			}

			ProvObjects::Entity NewEntity = E;

			if (E.deviceRules.rrm == "yes") {
				NewEntity.deviceRules.rrm = "inherit";
				Modified = true;
			}

			if (Modified) {
				poco_warning(Logger(), fmt::format("  fixing entity: {}", E.info.name));
				NewEntity.devices = NewDevices;
				NewEntity.contacts = NewContacts;
				NewEntity.locations = NewLocations;
				NewEntity.venues = NewVenues;
				NewEntity.variables = NewVariables;
				EntityDB().UpdateRecord("id", E.info.id, NewEntity);
			}
			return true;
		};

		auto FixInventory = [&](const ProvObjects::InventoryTag &T) -> bool {
			// check the venue/entity for this device.
			ProvObjects::InventoryTag NewTag{T};
			bool modified = false;
			if (!T.venue.empty() && !VenueDB().Exists("id", T.venue)) {
				NewTag.venue.clear();
				modified = true;
			}

			if (!T.entity.empty() && !EntityDB().Exists("id", T.entity)) {
				NewTag.entity.clear();
				modified = true;
			}

			if (!T.location.empty() && !LocationDB().Exists("id", T.location)) {
				NewTag.location.clear();
				modified = true;
			}

			if (!T.contact.empty() && !ContactDB().Exists("id", T.contact)) {
				NewTag.contact.clear();
				modified = true;
			}

			if (T.deviceRules.rrm == "yes") {
				NewTag.deviceRules.rrm = "inherit";
				modified = true;
			}

			if (modified) {
				poco_warning(Logger(), fmt::format("  fixing entity: {}", T.info.name));
				InventoryDB().UpdateRecord("id", T.info.id, NewTag);
			}
			return true;
		};

		auto FixConfiguration = [&](const ProvObjects::DeviceConfiguration &C) -> bool {
			ProvObjects::DeviceConfiguration NewConfig{C};

			bool modified = false;

			if (C.deviceRules.rrm == "yes") {
				NewConfig.deviceRules.rrm = "inherit";
				modified = true;
			}

			if (modified) {
				poco_warning(Logger(), fmt::format("  fixing configuration: {}", C.info.name));
				ConfigurationDB().UpdateRecord("id", C.info.id, NewConfig);
			}
			return true;
		};

		auto FixOperator = [&](const ProvObjects::Operator &O) -> bool {
			ProvObjects::Operator NewOp{O};
			bool modified = false;

			if (O.deviceRules.rrm == "yes") {
				NewOp.deviceRules.rrm = "inherit";
				modified = true;
			}

			if (modified) {
				poco_warning(Logger(), fmt::format("  fixing operator: {}", O.info.name));
				OperatorDB().UpdateRecord("id", O.info.id, NewOp);
			}
			return true;
		};

		auto FixSubscriber = [&](const ProvObjects::SubscriberDevice &O) -> bool {
			ProvObjects::SubscriberDevice NewSub{O};
			bool modified = false;

			if (O.deviceRules.rrm == "yes") {
				NewSub.deviceRules.rrm = "inherit";
				modified = true;
			}

			if (modified) {
				poco_warning(Logger(), fmt::format("  fixing subscriber: {}", O.info.name));
				SubscriberDeviceDB().UpdateRecord("id", O.info.id, NewSub);
			}
			return true;
		};

		poco_information(Logger(), "Checking DB consistency: venues");
		VenueDB().Iterate(FixVenueDevices);
		poco_information(Logger(), "Checking DB consistency: entities");
		EntityDB().Iterate(FixEntity);
		poco_information(Logger(), "Checking DB consistency: inventory");
		InventoryDB().Iterate(FixInventory);
		poco_information(Logger(), "Checking DB consistency: configurations");
		ConfigurationDB().Iterate(FixConfiguration);
		poco_information(Logger(), "Checking DB consistency: operators");
		OperatorDB().Iterate(FixOperator);
		poco_information(Logger(), "Checking DB consistency: subscribers");
		SubscriberDeviceDB().Iterate(FixSubscriber);
	}

	void Storage::InitializeSystemDBs() {
		if (!EntityDB().Exists("id", EntityDB::RootUUID())) {
			ProvObjects::Entity Root;

			Root.info.id = EntityDB::RootUUID();
			Root.info.name = "Top Entity";
			Root.info.created = Root.info.modified = Utils::Now();
			Root.deviceRules.rrm = "off";
			EntityDB().CreateRecord(Root);
		}

		auto OperatorCount = OperatorDB().Count();
		if (OperatorCount == 0) {
			ProvObjects::Operator DefOp;
			DefOp.info.id = MicroServiceCreateUUID();
			DefOp.info.name = "Default Operator";
			DefOp.defaultOperator = true;
			DefOp.info.created = DefOp.info.modified = Utils::Now();
			DefOp.deviceRules.rrm = "inherit";
			OperatorDB_->CreateRecord(DefOp);

			ProvObjects::ServiceClass DefSer;
			DefSer.info.id = MicroServiceCreateUUID();
			DefSer.info.name = "Default Service Class";
			DefSer.defaultService = true;
			DefSer.info.created = DefSer.info.modified = Utils::Now();
			DefSer.operatorId = DefOp.info.id;
			DefSer.period = "monthly";
			DefSer.billingCode = "basic";
			DefSer.currency = "USD";
			DefSer.cost = 0.0;
			ServiceClassDB_->CreateRecord(DefSer);
		}
	}
} // namespace OpenWifi

// namespace