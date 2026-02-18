//
// Created by stephane bourque on 2021-10-18.
//

#pragma once

#include <utility>

#include "Poco/StringTokenizer.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"
#include "framework/ConfigurationValidator.h"
#include "libs/croncpp.h"
#include "sdks/SDK_sec.h"

namespace OpenWifi {

	inline static void AddInfoBlock(const ProvObjects::ObjectInfo &O, Poco::JSON::Object &J) {
		J.set("name", O.name);
		J.set("description", O.description);
		J.set("id", O.id);
	}

	template <typename R, typename Q = decltype(R{}.entity)>
	void Extend_entity(const R &T, Poco::JSON::Object &EI) {
		if constexpr (std::is_same_v<Q, std::string>) {
			if (!T.entity.empty()) {
				Poco::JSON::Object EntObj;
				ProvObjects::Entity Entity;
				if (StorageService()->EntityDB().GetRecord("id", T.entity, Entity)) {
					AddInfoBlock(Entity.info, EntObj);
				}
				EI.set("entity", EntObj);
			}
		}
	}
	template <typename... Ts> void Extend_entity(Ts... args) {
		static_assert(sizeof...(args) == 2);
	}

	template <typename R, typename Q = decltype(R{}.managementPolicy)>
	void Extend_managementPolicy(const R &T, Poco::JSON::Object &EI) {
		if constexpr (std::is_same_v<Q, std::string>) {
			if (!T.managementPolicy.empty()) {
				Poco::JSON::Object PolObj;
				ProvObjects::ManagementPolicy Policy;
				if (StorageService()->PolicyDB().GetRecord("id", T.managementPolicy, Policy)) {
					AddInfoBlock(Policy.info, PolObj);
				}
				EI.set("managementPolicy", PolObj);
			}
		}
	}
	template <typename... Ts> void Extend_managementPolicy(Ts... args) {
		static_assert(sizeof...(args) == 2);
	}

	template <typename R, typename Q = decltype(R{}.venue)>
	void Extend_venue(const R &T, Poco::JSON::Object &EI) {
		if constexpr (std::is_same_v<Q, std::string>) {
			if (!T.venue.empty()) {
				Poco::JSON::Object VenObj;
				ProvObjects::Venue Venue;
				if (StorageService()->VenueDB().GetRecord("id", T.venue, Venue)) {
					AddInfoBlock(Venue.info, VenObj);
				}
				EI.set("venue", VenObj);
			}
		}
	}
	template <typename... Ts> void Extend_venue(Ts... args) { static_assert(sizeof...(args) == 2); }

	template <typename R, typename Q = decltype(std::declval<R>().subscriber)>
	void Extend_subscriber(const R &T, Poco::JSON::Object &EI) {
		if constexpr (std::is_same_v<Q, std::string>) {
			if (!T.subscriber.empty()) {
				Poco::JSON::Object SubObj;
				ProvObjects::SignupEntry Signup;
				if (StorageService()->SignupDB().GetRecord("userid", T.subscriber, Signup)) {
					SubObj.set("email", Signup.email);
					SubObj.set("description", Signup.info.description);
				}
				SubObj.set("id", T.subscriber);
				EI.set("subscriber", SubObj);
			}
		}
	}
	template <typename... Ts> void Extend_subscriber(Ts... args) {
		static_assert(sizeof...(args) == 2);
	}

	template <typename R, typename Q = decltype(R{}.contact)>
	void Extend_contact(const R &T, Poco::JSON::Object &EI) {
		if constexpr (std::is_same_v<Q, std::string>) {
			if (!T.contact.empty()) {
				Poco::JSON::Object ConObj;
				ProvObjects::Contact Contact;
				if (StorageService()->ContactDB().GetRecord("id", T.contact, Contact)) {
					AddInfoBlock(Contact.info, ConObj);
				}
				EI.set("contact", ConObj);
			}
		}
	}
	template <typename... Ts> void Extend_contact(Ts... args) {
		static_assert(sizeof...(args) == 2);
	}

	template <typename R, typename Q = decltype(R{}.location)>
	void Extend_location(const R &T, Poco::JSON::Object &EI) {
		if constexpr (std::is_same_v<Q, std::string>) {
			if (!T.location.empty()) {
				Poco::JSON::Object LocObj;
				ProvObjects::Location Location;
				if (StorageService()->LocationDB().GetRecord("id", T.location, Location)) {
					AddInfoBlock(Location.info, LocObj);
				}
				EI.set("location", LocObj);
			}
		}
	}
	template <typename... Ts> void Extend_location(Ts... args) {
		static_assert(sizeof...(args) == 2);
	}

	template <typename R, typename Q = decltype(R{}.deviceConfiguration)>
	void Extend_deviceConfiguration(const R &T, Poco::JSON::Object &EI) {
		if constexpr (std::is_same_v<Q, std::string>) {
			if (!T.deviceConfiguration.empty()) {
				Poco::JSON::Object DevObj;
				ProvObjects::DeviceConfiguration DevConf;
				if (StorageService()->ConfigurationDB().GetRecord("id", T.deviceConfiguration,
																  DevConf)) {
					AddInfoBlock(DevConf.info, DevObj);
				}
				EI.set("deviceConfiguration", DevObj);
			}
		}
		if constexpr (std::is_same_v<Q, Types::UUIDvec_t>) {
			if (!T.deviceConfiguration.empty()) {
				Poco::JSON::Array ObjArr;
				ProvObjects::DeviceConfiguration DevConf;
				for (const auto &i : T.deviceConfiguration) {
					if (StorageService()->ConfigurationDB().GetRecord("id", i, DevConf)) {
						Poco::JSON::Object InnerObj;
						AddInfoBlock(DevConf.info, InnerObj);
						ObjArr.add(InnerObj);
					}
				}
				EI.set("deviceConfiguration", ObjArr);
			}
		}
	}

	template <typename... Ts> void Extend_deviceConfiguration(Ts... args) {
		static_assert(sizeof...(args) == 2);
	}

	template <typename R> bool AddExtendedInfo(const R &T, Poco::JSON::Object &O) {
		Poco::JSON::Object EI;
		Extend_entity(T, EI);
		Extend_deviceConfiguration(T, EI);
		Extend_location(T, EI);
		Extend_contact(T, EI);
		Extend_venue(T, EI);
		Extend_subscriber(T, EI);
		Extend_managementPolicy(T, EI);
		O.set("extendedInfo", EI);
		return true;
	}

	template <typename T>
	void MakeJSONObjectArray(const char *ArrayName, const std::vector<T> &V, RESTAPIHandler &R) {
		Poco::JSON::Array ObjArray;
		for (const auto &i : V) {
			Poco::JSON::Object Obj;
			i.to_json(Obj);
			if (R.NeedAdditionalInfo())
				AddExtendedInfo(i, Obj);
			ObjArray.add(Obj);
		}
		Poco::JSON::Object Answer;
		Answer.set(ArrayName, ObjArray);
		return R.ReturnObject(Answer);
	}

	inline static bool is_uuid(const std::string &u) { return u.find('-') != std::string::npos; }

	template <typename DB>
	void ReturnRecordList(const char *ArrayName, DB &DBInstance, RESTAPIHandler &R) {
		Poco::JSON::Array ObjArr;
		for (const auto &i : R.SelectedRecords()) {
			ProvObjects::InventoryTag E;
			if (DBInstance.GetRecord(is_uuid(i) ? "id" : "serialNumber", i, E)) {
				Poco::JSON::Object Obj;
				E.to_json(Obj);
				if (R.NeedAdditionalInfo())
					AddExtendedInfo(E, Obj);
				ObjArr.add(Obj);
			} else {
				return R.BadRequest(RESTAPI::Errors::UnknownId);
			}
		}
		Poco::JSON::Object Answer;
		Answer.set(ArrayName, ObjArr);
		return R.ReturnObject(Answer);
	}

	template <typename DB, typename Record>
	void ReturnRecordList(const char *ArrayName, DB &DBInstance, RESTAPIHandler &R) {
		Poco::JSON::Array ObjArr;
		for (const auto &i : R.SelectedRecords()) {
			Record E;
			if (DBInstance.GetRecord("id", i, E)) {
				Poco::JSON::Object Obj;
				E.to_json(Obj);
				if (R.NeedAdditionalInfo())
					AddExtendedInfo(E, Obj);
				ObjArr.add(Obj);
			} else {
				return R.BadRequest(RESTAPI::Errors::UnknownId);
			}
		}
		Poco::JSON::Object Answer;
		Answer.set(ArrayName, ObjArr);
		return R.ReturnObject(Answer);
	}

	inline bool NormalizeMac(std::string &Mac) {
		Poco::replaceInPlace(Mac, ":", "");
		Poco::replaceInPlace(Mac, "-", "");
		if (Mac.size() != 12)
			return false;
		for (const auto &i : Mac) {
			if (!std::isxdigit(i))
				return false;
		}
		Poco::toLowerInPlace(Mac);
		return true;
	}

	typedef std::tuple<std::string, std::string, std::string> triplet_t;

	inline void AddLocationTriplet(const std::string &id, std::vector<triplet_t> &IDs) {
		ProvObjects::Location L;
		if (StorageService()->LocationDB().GetRecord("id", id, L)) {
			IDs.emplace_back(std::make_tuple(L.info.name, L.info.description, L.info.id));
		}
	}

	inline void AddLocationTriplet(const std::vector<std::string> &id,
								   std::vector<triplet_t> &IDs) {
		for (const auto &i : id)
			AddLocationTriplet(i, IDs);
	}

	inline void GetLocationsForEntity(const std::string &ID, std::vector<triplet_t> &IDs) {
		ProvObjects::Entity Existing;
		if (StorageService()->EntityDB().template GetRecord("id", ID, Existing)) {
			if (!Existing.locations.empty()) {
				AddLocationTriplet(Existing.locations, IDs);
			}
			if (!Existing.parent.empty()) {
				GetLocationsForEntity(Existing.parent, IDs);
			}
			if (ID == EntityDB::RootUUID())
				return;
		}
	}

	inline void GetLocationsForVenue(const std::string &ID, std::vector<triplet_t> &IDs) {
		ProvObjects::Venue Existing;
		if (StorageService()->VenueDB().template GetRecord("id", ID, Existing)) {
			if (!Existing.parent.empty()) {
				GetLocationsForVenue(Existing.parent, IDs);
			}
			ProvObjects::Entity E;
			if (StorageService()->EntityDB().GetRecord("id", Existing.entity, E)) {
				AddLocationTriplet(E.locations, IDs);
			}
			return;
		}
	}

	template <typename DB>
	void ListHandler(const char *BlockName, DB &DBInstance, RESTAPIHandler &R) {
		auto Entity = R.GetParameter("entity", "");
		auto Venue = R.GetParameter("venue", "");

		typedef typename DB::RecordVec RecVec;
		typedef typename DB::RecordName RecType;

		if constexpr (std::is_same_v<RecType, ProvObjects::Venue>) {
			auto LocationsForVenue = R.GetParameter("locationsForVenue", "");
			if (!LocationsForVenue.empty()) {
				std::vector<triplet_t> IDs;
				GetLocationsForVenue(LocationsForVenue, IDs);
				Poco::JSON::Array A;
				for (const auto &[name, description, uuid] : IDs) {
					Poco::JSON::Object O;
					O.set("name", name);
					O.set("description", description);
					O.set("uuid", uuid);
					A.add(O);
				}
				Poco::JSON::Object Answer;
				Answer.set("locations", A);
				return R.ReturnObject(Answer);
			}
		}

		if (!R.QB_.Select.empty()) {
			return ReturnRecordList<decltype(DBInstance), RecType>(BlockName, DBInstance, R);
		}
		if (!Entity.empty()) {
			RecVec Entries;
			DBInstance.GetRecords(R.QB_.Offset, R.QB_.Limit, Entries, " entity=' " + Entity + "'");
			if (R.QB_.CountOnly)
				return R.ReturnCountOnly(Entries.size());
			return MakeJSONObjectArray(BlockName, Entries, R);
		}
		if (!Venue.empty()) {
			RecVec Entries;
			DBInstance.GetRecords(R.QB_.Offset, R.QB_.Limit, Entries, " venue=' " + Venue + "'");
			if (R.QB_.CountOnly)
				return R.ReturnCountOnly(Entries.size());
			return MakeJSONObjectArray(BlockName, Entries, R);
		} else if (R.QB_.CountOnly) {
			Poco::JSON::Object Answer;
			auto C = DBInstance.Count();
			return R.ReturnCountOnly(C);
		} else {
			RecVec Entries;
			DBInstance.GetRecords(R.QB_.Offset, R.QB_.Limit, Entries);
			return MakeJSONObjectArray(BlockName, Entries, R);
		}
	}

	template <typename db_type>
	void ListHandlerForOperator(const char *BlockName, db_type &DB, RESTAPIHandler &R,
								const Types::UUID_t &OperatorId,
								const Types::UUID_t &subscriberId = "") {
		typedef typename db_type::RecordVec RecVec;
		typedef typename db_type::RecordName RecType;

		auto whereClause =
			subscriberId.empty()
				? fmt::format(" operatorId='{}'", OperatorId)
				: fmt::format(" operatorId='{}' and subscriberId='{}' ", OperatorId, subscriberId);

		if (R.QB_.CountOnly) {
			auto Count = DB.Count(whereClause);
			return R.ReturnCountOnly(Count);
		}

		if (!R.QB_.Select.empty()) {
			return ReturnRecordList<decltype(DB), RecType>(BlockName, DB, R);
		}

		RecVec Entries;
		DB.GetRecords(R.QB_.Offset, R.QB_.Limit, Entries, whereClause);
		return MakeJSONObjectArray(BlockName, Entries, R);
	}

	template <typename db_type, typename ObjectDB>
	void MoveUsage(db_type &DB_InUse, ObjectDB &DB, const std::string &From, const std::string &To,
				   const std::string &Id) {
		if (From != To) {
			if (!From.empty())
				DB_InUse.DeleteInUse("id", From, DB.Prefix(), Id);
			if (!To.empty())
				DB_InUse.AddInUse("id", To, DB.Prefix(), Id);
		}
	}

	template <typename db_type, typename ObjectDB>
	void MoveUsage(db_type &DB_InUse, ObjectDB &DB, const Types::UUIDvec_t &From,
				   const Types::UUIDvec_t &To, const std::string &Id) {
		if (From != To) {
			if (!From.empty()) {
				for (const auto &i : From)
					DB_InUse.DeleteInUse("id", i, DB.Prefix(), Id);
			}
			if (!To.empty()) {
				for (const auto &i : To)
					DB_InUse.AddInUse("id", i, DB.Prefix(), Id);
			}
		}
	}

	template <typename db_type>
	void MoveChild(db_type &DB, const std::string &Parent, const std::string &Child,
				   const std::string &Id) {
		if (Parent != Child) {
			if (!Parent.empty())
				DB.InUse.DeleteInUse("id", Parent, Id);
			if (!Child.empty())
				DB.AddInUse("id", Child, Id);
		}
	}

	template <typename db_type, typename Member>
	void RemoveMembership(db_type &DB, Member T, const std::string &Obj, const std::string &Id) {
		if (!Obj.empty())
			DB.ManipulateVectorMember(T, "id", Obj, Id, false);
	}

	template <typename db_type, typename Member>
	void AddMembership(db_type &DB, Member T, const std::string &Obj, const std::string &Id) {
		if (!Obj.empty())
			DB.ManipulateVectorMember(T, "id", Obj, Id, true);
	}

	template <typename db_type, typename Member>
	void AddMembership(db_type &DB, Member T, const Types::UUIDvec_t &Obj, const std::string &Id) {
		for (const auto &i : Obj) {
			AddMembership(DB, T, i, Id);
		}
	}

	template <typename db_type, typename Member>
	void ManageMembership(db_type &DB, Member T, const std::string &From, const std::string &To,
						  const std::string &Id) {
		RemoveMembership(DB, T, From, Id);
		AddMembership(DB, T, To, Id);
	}

	template <typename db_type, typename Member>
	void ManageMembership(db_type &DB, Member T, const Types::UUIDvec_t &From,
						  const Types::UUIDvec_t &To, const std::string &Id) {
		if (From != To) {
			for (const auto &i : From) {
				RemoveMembership(DB, T, i, Id);
			}
			for (const auto &i : To) {
				AddMembership(DB, T, i, Id);
			}
		}
	}

	template <typename Member, typename Rec, typename db_type>
	bool CreateMove(const Poco::JSON::Object::Ptr &RawObj, const char *fieldname, Member T,
					Rec &Existing, std::string &From, std::string &To, db_type &TheDB) {
		if (RawObj->has(fieldname)) {
			From = Existing.*T;
			To = RawObj->get(fieldname).toString();
			if (!To.empty() && !TheDB.Exists("id", To))
				return false;
			Existing.*T = To;
		}
		return true;
	}

	inline std::string FindParentEntity(const ProvObjects::Venue &V) {
		if (V.parent.empty())
			return V.entity;
		ProvObjects::Venue P;
		if (StorageService()->VenueDB().GetRecord("id", V.parent, P))
			return FindParentEntity(P);
		return EntityDB::RootUUID();
	}

    inline bool ValidateConfigBlock(ConfigurationValidator::ConfigurationType Type, const ProvObjects::DeviceConfiguration &Config,
                                    std::vector<std::string> &Errors) {
        static const std::vector<std::string> SectionNames{
                "globals",	   "interfaces", "metrics", "radios",	  "services",	"unit",
                "definitions", "ethernet",	 "switch",	"config-raw", "third-party"};

        for (const auto &i : Config.configuration) {
            Poco::JSON::Parser P;
            if (i.name.empty()) {
                Errors.push_back("Name is empty");
                return false;
            }

            try {
                auto Blocks = P.parse(i.configuration).extract<Poco::JSON::Object::Ptr>();
                auto N = Blocks->getNames();
                for (const auto &j : N) {
                    if (std::find(SectionNames.cbegin(), SectionNames.cend(), j) ==
                        SectionNames.cend()) {
                        Errors.push_back("Unknown block name");
                        return false;
                    }
                }
            } catch (const Poco::JSON::JSONException &E) {
                Errors.push_back("Invalid JSON document");
                return false;
            }

            try {
                std::string Error;
                if (ValidateUCentralConfiguration(Type,i.configuration, Error, true)) {
                    // std::cout << "Block: " << i.name << " is valid" << std::endl;
                } else {
                    Errors.push_back(Error);
                    return false;
                }
            } catch (...) {
                Errors.push_back("Invalid configuration caused an exception");
                return false;
            }
        }
        return true;
    }

	template <typename Type>
	std::map<std::string, std::string> CreateObjects(Type &NewObject, RESTAPIHandler &R,
													 std::vector<std::string> &Errors) {
		std::map<std::string, std::string> Result;

		auto createObjects = R.GetParameter("createObjects", "");
		if (!createObjects.empty()) {
			Poco::JSON::Parser P;
			auto Objects = P.parse(createObjects).extract<Poco::JSON::Object::Ptr>();
			if (Objects->isArray("objects")) {
				auto ObjectsArray = Objects->getArray("objects");
				for (const auto &i : *ObjectsArray) {
					auto Object = i.extract<Poco::JSON::Object::Ptr>();
					if (Object->has("location")) {
						auto LocationDetails =
							Object->get("location").extract<Poco::JSON::Object::Ptr>();
						ProvObjects::Location LC;
						if (LC.from_json(LocationDetails)) {
							if constexpr (std::is_same_v<Type, ProvObjects::Venue>) {
								std::string ParentEntity = FindParentEntity(NewObject);
								ProvObjects::CreateObjectInfo(R.UserInfo_.userinfo, LC.info);
								LC.entity = ParentEntity;
								if (StorageService()->LocationDB().CreateRecord(LC)) {
									NewObject.location = LC.info.id;
									AddMembership(StorageService()->EntityDB(),
												  &ProvObjects::Entity::locations, ParentEntity,
												  LC.info.id);
									Result["location"] = LC.info.id;
								}
							}
							if constexpr (std::is_same_v<Type, ProvObjects::Operator>) {
								std::string ParentEntity = FindParentEntity(NewObject);
								ProvObjects::CreateObjectInfo(R.UserInfo_.userinfo, LC.info);
								LC.entity = ParentEntity;
								if (StorageService()->LocationDB().CreateRecord(LC)) {
									NewObject.location = LC.info.id;
									AddMembership(StorageService()->EntityDB(),
												  &ProvObjects::Entity::locations, ParentEntity,
												  LC.info.id);
									Result["location"] = LC.info.id;
								}
							}
						} else {
							Errors.push_back("Invalid JSON document");
							break;
						}
					} else if (Object->has("contact")) {
						auto ContactDetails =
							Object->get("contact").extract<Poco::JSON::Object::Ptr>();
						ProvObjects::Contact CC;
						if (CC.from_json(ContactDetails)) {
							std::cout << "contact decoded: " << CC.info.name << std::endl;
						} else {
							std::cout << "contact not decoded." << std::endl;
						}
					} else if (Object->has("configuration")) {
						auto ConfigurationDetails =
							Object->get("configuration")
								.template extract<Poco::JSON::Object::Ptr>();
						ProvObjects::DeviceConfiguration DC;
						if (DC.from_json(ConfigurationDetails)) {
							if constexpr (std::is_same_v<Type, ProvObjects::InventoryTag>) {
								auto ValidationType =
									ConfigurationValidator::GetType(NewObject.platform);
								if (!ValidateConfigBlock(ValidationType, DC, Errors)) {
									break;
								}
								ProvObjects::CreateObjectInfo(R.UserInfo_.userinfo, DC.info);
								if (StorageService()->ConfigurationDB().CreateRecord(DC)) {
									NewObject.deviceConfiguration = DC.info.id;
									Result["configuration"] = DC.info.id;
								}
							}
						} else {
							Errors.push_back("Invalid JSON document");
							break;
						}
					}
				}
			}
		}
		return Result;
	}

	inline bool ValidSchedule(const std::string &v) {
		try {
			auto cron = cron::make_cron(v);
			return true;
		} catch (cron::bad_cronexpr const &ex) {
		}
		return false;
	}

	inline bool ValidRRM(const std::string &v) {
		if ((v == "no") || (v == "inherit"))
			return true;
		try {
			Poco::JSON::Parser P;
			auto O = P.parse(v).extract<Poco::JSON::Object::Ptr>();

			ProvObjects::RRMDetails D;
			if (D.from_json(O)) {
				return ValidSchedule(D.schedule);
			}
		} catch (...) {
		}
		return false;
	}

	inline bool ValidDeviceRules(const ProvObjects::DeviceRules &DR) {
		return (ValidRRM(DR.rrm)) &&
			   (DR.firmwareUpgrade == "yes" || DR.firmwareUpgrade == "no" ||
				DR.firmwareUpgrade == "inherit") &&
			   (DR.rcOnly == "yes" || DR.rcOnly == "no" || DR.rcOnly == "inherit");
	}

	inline bool ValidDeviceRules(const ProvObjects::DeviceRules &DR, RESTAPIHandler &H) {
		if (ValidDeviceRules(DR))
			return true;
		H.BadRequest(RESTAPI::Errors::InvalidRRM);
		return false;
	}

	inline bool ValidSourceIP([[maybe_unused]] const std::vector<std::string> &IPs) { return true; }

	inline bool ValidPeriod(const std::string &P) {
		return (P == "hourly" || P == "daily" || P == "monthly" || P == "yearly" ||
				P == "quarterly" || P == "lifetime" || P == "custom1" || P == "custom2" ||
				P == "custom3" || P == "custom4");
	}

	inline bool ValidContactType(const std::string &contact) {
		auto C = Poco::toLower(contact);
		return (C == "subscriber" || C == "user" || C == "installer" || C == "csr" ||
				C == "manager" || C == "businessowner" || C == "technician" || C == "corporate");
	}

	inline bool ValidContactType(const std::string &contact, RESTAPIHandler &H) {
		auto C = Poco::toLower(contact);
		if (C == "subscriber" || C == "user" || C == "installer" || C == "csr" || C == "manager" ||
			C == "businessowner" || C == "technician" || C == "corporate")
			return true;
		H.BadRequest(RESTAPI::Errors::InvalidContactType);
		return false;
	}

	inline bool ValidLocationType(const std::string &location) {
		auto C = Poco::toLower(location);
		return (C == "service" || C == "equipment" || C == "auto" || C == "manual" ||
				C == "special" || C == "unknown" || C == "corporate");
	}

	inline bool ValidLocationType(const std::string &location, RESTAPIHandler &H) {
		auto C = Poco::toLower(location);
		if ((C == "service" || C == "equipment" || C == "auto" || C == "manual" || C == "special" ||
			 C == "unknown" || C == "corporate"))
			return true;
		H.BadRequest(RESTAPI::Errors::InvalidLocationType);
		return false;
	}

	template <typename DBType>
	bool ValidDbId(const Types::UUID_t &uuid, DBType &DB, bool AllowEmpty,
				   const RESTAPI::Errors::msg &Error, RESTAPIHandler &H) {
		if (!AllowEmpty && uuid.empty()) {
			H.BadRequest(Error);
			return false;
		}
		if (uuid.empty())
			return true;
		if (!DB.Exists("id", uuid)) {
			H.BadRequest(Error);
			return false;
		}
		return true;
	}

	inline bool ValidSubscriberId(const Types::UUID_t &uuid, bool AllowEmpty, RESTAPIHandler &H) {
		if (!AllowEmpty && uuid.empty()) {
			H.BadRequest(RESTAPI::Errors::InvalidSubscriberId);
			return false;
		}
		if (uuid.empty())
			return true;
		SecurityObjects::UserInfo NewSubInfo;
		if (!SDK::Sec::Subscriber::Get(&H, uuid, NewSubInfo)) {
			H.BadRequest(RESTAPI::Errors::InvalidSubscriberId);
			return false;
		}
		return true;
	}

	inline bool ValidSubscriberId(const Types::UUID_t &uuid, bool AllowEmpty, std::string &email,
								  RESTAPIHandler &H) {
		if (!AllowEmpty && uuid.empty()) {
			H.BadRequest(RESTAPI::Errors::InvalidSubscriberId);
			return false;
		}
		if (uuid.empty())
			return true;
		SecurityObjects::UserInfo NewSubInfo;
		if (!SDK::Sec::Subscriber::Get(&H, uuid, NewSubInfo)) {
			H.BadRequest(RESTAPI::Errors::InvalidSubscriberId);
			return false;
		}
		email = NewSubInfo.email;
		return true;
	}

	inline bool ValidSerialNumber(const std::string &serialNumber, bool AllowEmpty,
								  RESTAPIHandler &H) {
		if (!AllowEmpty && serialNumber.empty()) {
			H.BadRequest(RESTAPI::Errors::InvalidSerialNumber);
			return false;
		}

		if (!Utils::ValidSerialNumber(serialNumber)) {
			H.BadRequest(RESTAPI::Errors::InvalidSerialNumber);
			return false;
		}
		return true;
	}

	template <typename DBType, typename DBRecordType>
	void ReturnUpdatedObject(DBType &DB, const DBRecordType &R, RESTAPIHandler &H) {
		if (DB.UpdateRecord("id", R.info.id, R)) {
			DBRecordType Updated;
			DB.GetRecord("id", R.info.id, Updated);
			Poco::JSON::Object Answer;
			Updated.to_json(Answer);
			return H.ReturnObject(Answer);
		} else {
			H.InternalError(RESTAPI::Errors::RecordNotUpdated);
		}
	}

	template <typename DBType, typename DBRecordType>
	void ReturnCreatedObject(DBType &DB, const DBRecordType &R, RESTAPIHandler &H) {
		if (DB.CreateRecord(R)) {
			DBRecordType Updated;
			DB.GetRecord("id", R.info.id, Updated);
			Poco::JSON::Object Answer;
			Updated.to_json(Answer);
			return H.ReturnObject(Answer);
		} else {
			H.InternalError(RESTAPI::Errors::RecordNotCreated);
		}
	}

	template <typename DBType> void ReturnFieldList(DBType &DB, RESTAPIHandler &H) {
		Types::StringVec Fields;
		DB.GetFieldNames(Fields);
		Poco::JSON::Object Answer;
		RESTAPI_utils::field_to_json(Answer, "list", Fields);
		return H.ReturnObject(Answer);
	}
} // namespace OpenWifi
