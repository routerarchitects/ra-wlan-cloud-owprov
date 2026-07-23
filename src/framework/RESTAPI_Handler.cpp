//
// Created by stephane bourque on 2022-10-25.
//

#include "RESTAPI_Handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"
#include "framework/MicroServiceFuncs.h"

namespace OpenWifi {

	bool RESTAPIHandler::RoleIsAuthorized(const std::string &Path, const std::string &Method,
										  std::string &Reason) {
		// 1. Bypass check only if user is root
		if (UserInfo_.userinfo.userRole == SecurityObjects::ROOT) {
			return true;
		}

		// 2. Map path to resource
		std::string Resource = GetResourceName(Path);
		if (Resource.empty()) {
			Reason = "Unknown or prohibited resource path.";
			return false;
		}

		if (Resource == "user" &&
			(Method == Poco::Net::HTTPRequest::HTTP_POST ||
			 Method == Poco::Net::HTTPRequest::HTTP_PUT) &&
			ParsedBody_ && ParsedBody_->has("userRole")) {
			const auto RequestedUserRole = ParsedBody_->get("userRole").toString();
			if (Poco::icompare(RequestedUserRole, "root") == 0 &&
				UserInfo_.userinfo.userRole != SecurityObjects::ROOT) {
				Reason = "Only root may assign the root user role.";
				return false;
			}
		}

		if (Resource == "managementPolicy" && Method == Poco::Net::HTTPRequest::HTTP_GET) {
			return true;
		}

		std::string UserId = UserInfo_.userinfo.id;
		std::vector<ProvObjects::ManagementRole> Roles;
		FindAllUserRoles(UserId, Roles);

		auto CheckRolePolicy = [&](const ProvObjects::ManagementRole &role) -> bool {
			ProvObjects::ManagementPolicy Policy;
			if (!AuthCache::GetInstance()->GetPolicy(role.managementPolicy, Policy)) {
				if (!StorageService()->PolicyDB().GetRecord("id", role.managementPolicy, Policy)) {
					return false;
				}
				AuthCache::GetInstance()->SetPolicy(role.managementPolicy, Policy);
			}
			return PolicyAllows(Policy, Resource, Method);
		};

		// 3. Resolve target Entity and Venue
		std::string TargetEntity, TargetVenue;
		if (!ResolveTargetContext(Path, Method, TargetEntity, TargetVenue)) {
			// If a scope constraint was specified (via path ID, query param, or body)
			// but could not be resolved against the database, we must fail closed.
			if (HasScopeConstraint(Resource, Method)) {
				Reason = "Scope resolution failed for constrained operation; access denied.";
				return false;
			}
			// If no scope constraint was specified (e.g. global/collection operations like
			// GET /api/v1/entity or GET /api/v1/inventory), we check if any role permits
			// the operation globally.
			for (const auto &role : Roles) {
				if (CheckRolePolicy(role)) {
					return true;
				}
			}
			Reason = "No authorized role found for this target resource and operation.";
			return false;
		}

		if (!TargetVenue.empty()) {
			for (const auto &role : Roles) {
				if (role.entity == TargetEntity && (role.venue == TargetVenue || role.venue.empty())) {
					if (CheckRolePolicy(role)) {
						return true;
					}
				}
			}
		} else {
			for (const auto &role : Roles) {
				if (role.entity == TargetEntity && (role.venue.empty() || role.venue == "")) {
					if (CheckRolePolicy(role)) {
						return true;
					}
				}
			}
		}

		Reason = "No authorized role matches the required scope and permission.";
		return false;
	}

	bool RESTAPIHandler::ResolveTargetContext(const std::string &Path, const std::string &Method,
											  std::string &TargetEntity, std::string &TargetVenue) {
		// ----------------------------------------------------------------
		// SOURCE 1: Bound object ID in path bindings (most authoritative).
		// Always look up the record in the DB — never accept a partial scope.
		// If an ID is present but the DB lookup fails, return false immediately
		// rather than falling through to a weaker source.
		// ----------------------------------------------------------------
		std::string Id;
		auto it = Bindings_.find("id");
		if (it != Bindings_.end()) {
			Id = it->second;
		} else {
			it = Bindings_.find("uuid");
			if (it != Bindings_.end()) {
				Id = it->second;
			} else {
				it = Bindings_.find(Poco::toLower(std::string(RESTAPI::Protocol::SERIALNUMBER)));
				if (it != Bindings_.end()) {
					Id = it->second;
				}
			}
		}

		const bool HasBoundObjectId =
			!Id.empty() && Id != "0" &&
			!(Method == Poco::Net::HTTPRequest::HTTP_POST && Poco::icompare(Id, "new") == 0);

		if (HasBoundObjectId) {
			if (Path.find("/api/v1/entity") != std::string::npos) {
				ProvObjects::Entity E;
				if (StorageService()->EntityDB().GetRecord("id", Id, E)) {
					TargetEntity = Id;
					return true;
				}
				return false;
			} else if (Path.find("/api/v1/venue") != std::string::npos) {
				ProvObjects::Venue V;
				if (StorageService()->VenueDB().GetRecord("id", Id, V)) {
					TargetVenue = Id;
					TargetEntity = V.entity;
					return true;
				}
				return false;
			} else if (Path.find("/api/v1/inventory") != std::string::npos) {
				ProvObjects::InventoryTag T;
				if (StorageService()->InventoryDB().GetRecord("id", Id, T) ||
					StorageService()->InventoryDB().GetRecord(RESTAPI::Protocol::SERIALNUMBER, Id,
															  T)) {
					TargetEntity = T.entity;
					TargetVenue = T.venue;
					return true;
				}
				return false;
			} else if (Path.find("/api/v1/configuration") != std::string::npos) {
				ProvObjects::DeviceConfiguration C;
				if (StorageService()->ConfigurationDB().GetRecord("id", Id, C)) {
					TargetEntity = C.entity;
					TargetVenue = C.venue;
					return true;
				}
				return false;
			} else if (Path.find("/api/v1/managementRole") != std::string::npos) {
				ProvObjects::ManagementRole R;
				if (StorageService()->RolesDB().GetRecord("id", Id, R)) {
					TargetEntity = R.entity;
					TargetVenue = R.venue;
					return true;
				}
				return false;
			} else if (Path.find("/api/v1/contact") != std::string::npos) {
				ProvObjects::Contact C;
				if (StorageService()->ContactDB().GetRecord("id", Id, C)) {
					TargetEntity = C.entity;
					return true;
				}
				return false;
			} else if (Path.find("/api/v1/location") != std::string::npos) {
				ProvObjects::Location L;
				if (StorageService()->LocationDB().GetRecord("id", Id, L)) {
					TargetEntity = L.entity;
					return true;
				}
				return false;
			} else if (Path.find("/api/v1/managementPolicy") != std::string::npos) {
				ProvObjects::ManagementPolicy P;
				if (StorageService()->PolicyDB().GetRecord("id", Id, P)) {
					TargetEntity = P.entity;
					TargetVenue = P.venue;
					return true;
				}
				return false;
			} else if (Path.find("/api/v1/map") != std::string::npos) {
				ProvObjects::Map M;
				if (StorageService()->MapDB().GetRecord("id", Id, M)) {
					TargetEntity = M.entity;
					TargetVenue = M.venue;
					return true;
				}
				return false;
			} else if (Path.find("/api/v1/variables") != std::string::npos) {
				ProvObjects::VariableBlock V;
				if (StorageService()->VariablesDB().GetRecord("id", Id, V)) {
					TargetEntity = V.entity;
					TargetVenue = V.venue;
					return true;
				}
				return false;
			} else if (Path.find("/api/v1/serviceClass") != std::string::npos) {
				ProvObjects::ServiceClass S;
				if (StorageService()->ServiceClassDB().GetRecord("id", Id, S)) {
					ProvObjects::Operator O;
					if (StorageService()->OperatorDB().GetRecord("id", S.operatorId, O)) {
						TargetEntity = O.entityId;
						return true;
					}
				}
				return false;
			} else if (Path.find("/api/v1/operator") != std::string::npos) {
				ProvObjects::Operator O;
				if (StorageService()->OperatorDB().GetRecord("id", Id, O)) {
					TargetEntity = O.entityId;
					return true;
				}
				return false;
			} else if (Path.find("/api/v1/overrides") != std::string::npos) {
				ProvObjects::InventoryTag T;
				if (StorageService()->InventoryDB().GetRecord("id", Id, T) ||
					StorageService()->InventoryDB().GetRecord(RESTAPI::Protocol::SERIALNUMBER, Id,
															  T)) {
					TargetEntity = T.entity;
					TargetVenue = T.venue;
					return true;
				}
				return false;
			} else if (Path.find("/api/v1/subscriberDevice") != std::string::npos) {
				ProvObjects::SubscriberDevice SD;
				if (StorageService()->SubscriberDeviceDB().GetRecord("id", Id, SD) ||
					StorageService()->SubscriberDeviceDB().GetRecord("serialNumber", Id, SD)) {
					ProvObjects::Operator O;
					if (StorageService()->OperatorDB().GetRecord("id", SD.operatorId, O)) {
						TargetEntity = O.entityId;
						return true;
					}
				}
				return false;
			} else if (Path.find("/api/v1/subscriber") != std::string::npos) {
				ProvObjects::SignupEntry SE;
				if (StorageService()->SignupDB().GetRecord("userid", Id, SE) ||
					StorageService()->SignupDB().GetRecord("id", Id, SE)) {
					ProvObjects::Operator O;
					if (StorageService()->OperatorDB().GetRecord("id", SE.operatorId, O)) {
						TargetEntity = O.entityId;
						return true;
					}
				}
				return false;
			} else if (Path.find("/api/v1/op_contact") != std::string::npos) {
				ProvObjects::OperatorContact OC;
				if (StorageService()->OpContactDB().GetRecord("id", Id, OC)) {
					ProvObjects::Operator O;
					if (StorageService()->OperatorDB().GetRecord("id", OC.operatorId, O)) {
						TargetEntity = O.entityId;
						return true;
					}
				}
				return false;
			} else if (Path.find("/api/v1/op_location") != std::string::npos) {
				ProvObjects::OperatorLocation OL;
				if (StorageService()->OpLocationDB().GetRecord("id", Id, OL)) {
					ProvObjects::Operator O;
					if (StorageService()->OperatorDB().GetRecord("id", OL.operatorId, O)) {
						TargetEntity = O.entityId;
						return true;
					}
				}
				return false;
			}
			// Bound ID present but path not recognised as a scoped resource.
			return false;
		}

		// ----------------------------------------------------------------
		// SOURCE 2: Query parameters (?entity=... / ?venue=...).
		// Collect candidate IDs first, then validate both against the DB.
		// If a param is supplied but the ID doesn't exist, return false —
		// don't silently fall back and treat a bad ID as "no scope".
		// ----------------------------------------------------------------
		std::string CandidateEntity, CandidateVenue, CandidateOperator;
		for (const auto &[name, value] : Parameters_) {
			if (name == "entity")
				CandidateEntity = value;
			else if (name == "venue")
				CandidateVenue = value;
			else if (name == "operatorId" || name == "operator")
				CandidateOperator = value;
		}

		// ----------------------------------------------------------------
		// SOURCE 3: Request body (entity / venue / operator / parent fields).
		// Only consulted when params yielded nothing.
		// ----------------------------------------------------------------
		if (CandidateEntity.empty() && CandidateVenue.empty() && CandidateOperator.empty() && ParsedBody_) {
			if (ParsedBody_->has("entity"))
				CandidateEntity = ParsedBody_->get("entity").toString();
			if (ParsedBody_->has("venue"))
				CandidateVenue = ParsedBody_->get("venue").toString();
			if (ParsedBody_->has("operatorId"))
				CandidateOperator = ParsedBody_->get("operatorId").toString();
			else if (ParsedBody_->has("operator"))
				CandidateOperator = ParsedBody_->get("operator").toString();

			if (CandidateEntity.empty() && CandidateVenue.empty() && CandidateOperator.empty() && ParsedBody_->has("parent")) {
				std::string ParentId = ParsedBody_->get("parent").toString();
				if (!ParentId.empty()) {
					if (Path.find("/api/v1/entity") != std::string::npos) {
						CandidateEntity = ParentId;
					} else if (Path.find("/api/v1/venue") != std::string::npos) {
						CandidateVenue = ParentId;
					}
				}
			}
		}

		// ----------------------------------------------------------------
		// Validate whatever candidates we have against the DB.
		// Venue takes priority: if present, derive entity from it so the
		// scope is always (entity, venue) or (entity, "") — never ("", venue).
		// If a candidate ID was supplied but the DB lookup fails, the scope
		// is unresolvable — return false rather than silently ignoring it.
		// ----------------------------------------------------------------
		if (!CandidateVenue.empty()) {
			ProvObjects::Venue V;
			if (StorageService()->VenueDB().GetRecord("id", CandidateVenue, V)) {
				TargetVenue = CandidateVenue;
				TargetEntity = V.entity;
				return true;
			}
			return false; // Venue ID provided but not found in DB.
		}

		if (!CandidateEntity.empty()) {
			ProvObjects::Entity E;
			if (StorageService()->EntityDB().GetRecord("id", CandidateEntity, E)) {
				TargetEntity = CandidateEntity;
				return true;
			}
			return false; // Entity ID provided but not found in DB.
		}

		if (!CandidateOperator.empty()) {
			ProvObjects::Entity E;
			if (StorageService()->EntityDB().GetRecord("operatorId", CandidateOperator, E)) {
				TargetEntity = E.info.id;
				return true;
			}
			if (StorageService()->EntityDB().GetRecord("id", CandidateOperator, E)) {
				TargetEntity = E.info.id;
				return true;
			}
			ProvObjects::Operator O;
			if (StorageService()->OperatorDB().GetRecord("id", CandidateOperator, O)) {
				TargetEntity = O.entityId.empty() ? CandidateOperator : O.entityId;
				return true;
			}
			return false; // Operator ID provided but not found in DB.
		}

		// No scope information found from any source.
		return false;
	}

	bool RESTAPIHandler::HasScopeConstraint(const std::string &Resource,
											const std::string &Method) {
		// Only resource types that are associated with an entity or venue can have scope
		// constraints. Global/unscoped resources (like iptocountry, radiusEndpoint, openroaming) do
		// not.
		bool IsScoped =
			(Resource == "entity" || Resource == "venue" || Resource == "inventory" ||
			 Resource == "device" || Resource == "configuration" || Resource == "managementRole" ||
			 Resource == "contact" || Resource == "location" || Resource == "managementPolicy" ||
			 Resource == "map" || Resource == "variables" || Resource == "serviceClass" ||
			 Resource == "operator" || Resource == "overrides" || Resource == "subscriber" ||
			 Resource == "subscriberDevice" || Resource == "op_contact" ||
			 Resource == "op_location");

		if (!IsScoped) {
			return false;
		}

		std::string Id;
		auto it = Bindings_.find("id");
		if (it != Bindings_.end()) {
			Id = it->second;
		} else {
			it = Bindings_.find("uuid");
			if (it != Bindings_.end()) {
				Id = it->second;
			} else {
				it = Bindings_.find(Poco::toLower(std::string(RESTAPI::Protocol::SERIALNUMBER)));
				if (it != Bindings_.end()) {
					Id = it->second;
				}
			}
		}

		if (!Id.empty() && Id != "0" &&
			!(Method == Poco::Net::HTTPRequest::HTTP_POST && Poco::icompare(Id, "new") == 0)) {
			return true;
		}

		for (const auto &[name, value] : Parameters_) {
			if (name == "entity" || name == "venue" || name == "operatorId" || name == "operator") {
				return true;
			}
		}

		if (ParsedBody_) {
			if (ParsedBody_->has("entity") || ParsedBody_->has("venue") ||
				ParsedBody_->has("operatorId") || ParsedBody_->has("operator") ||
				ParsedBody_->has("parent")) {
				return true;
			}
		}

		return false;
	}

	bool RESTAPIHandler::PolicyAllows(const ProvObjects::ManagementPolicy &Policy,
									  const std::string &Resource, const std::string &Method) {
		std::string AccessRequired;
		if (Method == Poco::Net::HTTPRequest::HTTP_GET)
			AccessRequired = "READ";
		else if (Method == Poco::Net::HTTPRequest::HTTP_POST)
			AccessRequired = "CREATE";
		else if (Method == Poco::Net::HTTPRequest::HTTP_PUT)
			AccessRequired = "UPDATE";
		else if (Method == Poco::Net::HTTPRequest::HTTP_DELETE)
			AccessRequired = "DELETE";
		else
			return false;

		for (const auto &entry : Policy.entries) {
			bool ResourceMatches = false;
			for (const auto &res : entry.resources) {
				if (Poco::icompare(res, Resource) == 0 || res == "*" ||
					(Resource == "serviceClass" && (Poco::icompare(res, "operator") == 0 || Poco::icompare(res, "entity") == 0)) ||
					(Resource == "subscriberDevice" && Poco::icompare(res, "device") == 0) ||
					(Resource == "op_contact" && Poco::icompare(res, "contact") == 0) ||
					(Resource == "op_location" && Poco::icompare(res, "location") == 0)) {
					ResourceMatches = true;
					break;
				}
			}
			if (!ResourceMatches)
				continue;

			for (const auto &acc : entry.access) {
				if (acc == "FULL" || acc == AccessRequired ||
					(AccessRequired == "UPDATE" && acc == "MODIFY")) {
					return true;
				}
			}
		}
		return false;
	}

	bool AuthCache::GetUserRoles(const std::string &userId,
								 std::vector<ProvObjects::ManagementRole> &roles) {
		std::shared_lock<std::shared_mutex> lock(Mutex_);
		auto it = Cache_.find(userId);
		if (it != Cache_.end()) {
			roles = it->second.roles;
			return true;
		}
		return false;
	}

	void AuthCache::SetUserRoles(const std::string &userId,
								 const std::vector<ProvObjects::ManagementRole> &roles) {
		std::unique_lock<std::shared_mutex> lock(Mutex_);
		Cache_[userId].roles = roles;
		Cache_[userId].lastFetched = Utils::Now();
	}

	bool AuthCache::GetPolicy(const std::string &policyId, ProvObjects::ManagementPolicy &policy) {
		std::shared_lock<std::shared_mutex> lock(Mutex_);
		auto it = Policies_.find(policyId);
		if (it != Policies_.end()) {
			policy = it->second;
			return true;
		}
		return false;
	}

	void AuthCache::SetPolicy(const std::string &policyId,
							  const ProvObjects::ManagementPolicy &policy) {
		std::unique_lock<std::shared_mutex> lock(Mutex_);
		Policies_[policyId] = policy;
	}

	void AuthCache::InvalidateUser(const std::string &userId) {
		std::unique_lock<std::shared_mutex> lock(Mutex_);
		Cache_.erase(userId);
	}

	void AuthCache::Clear() {
		std::unique_lock<std::shared_mutex> lock(Mutex_);
		Cache_.clear();
		Policies_.clear();
	}

	bool RESTAPIHandler::FindAnyRole(const std::string &userId,
									 ProvObjects::ManagementRole &AnyRole) {
		std::vector<ProvObjects::ManagementRole> Roles;
		FindAllUserRoles(userId, Roles);

		if (!Roles.empty()) {
			AnyRole = Roles.front();
			return true;
		}
		return false;
	}

	bool RESTAPIHandler::FindAllUserRoles(const std::string &userId,
										  std::vector<ProvObjects::ManagementRole> &Roles) {
		if (!AuthCache::GetInstance()->GetUserRoles(userId, Roles)) {
			StorageService()->RolesDB().Iterate([&](const ProvObjects::ManagementRole &role) {
				for (const auto &user : role.users) {
					if (user == userId) {
						Roles.push_back(role);
						break;
					}
				}
				return true;
			});
			AuthCache::GetInstance()->SetUserRoles(userId, Roles);
		}
		return !Roles.empty();
	}

	bool RESTAPIHandler::FindExistingRole(const std::string &userId, const std::string &entityId,
										  const std::string &venueId,
										  ProvObjects::ManagementRole &ExistingRole) {
		std::vector<ProvObjects::ManagementRole> Roles;
		FindAllUserRoles(userId, Roles);

		if (!venueId.empty()) {
			for (const auto &role : Roles) {
				if (role.entity == entityId && role.venue == venueId) {
					ExistingRole = role;
					return true;
				}
			}
		} else {
			for (const auto &role : Roles) {
				if (role.entity == entityId && (role.venue.empty() || role.venue == "")) {
					ExistingRole = role;
					return true;
				}
			}
		}

		return false;
	}

	void RESTAPIHandler::GetDescendantEntities(const std::string &id,
											   std::set<std::string> &descendants) {
		descendants.insert(id);
		ProvObjects::Entity E;
		if (StorageService()->EntityDB().GetRecord("id", id, E)) {
			for (const auto &child : E.children) {
				GetDescendantEntities(child, descendants);
			}
		}
	}

	void RESTAPIHandler::GetDescendantVenues(const std::string &id, std::set<std::string> &venues) {
		venues.insert(id);
		ProvObjects::Venue V;
		if (StorageService()->VenueDB().GetRecord("id", id, V)) {
			for (const auto &child : V.children) {
				GetDescendantVenues(child, venues);
			}
		}
	}

	std::string RESTAPIHandler::GetResourceName(const std::string &Path) {
		if (Path.find("/api/v1/entity") != std::string::npos)
			return "entity";
		if (Path.find("/api/v1/venue") != std::string::npos)
			return "venue";
		if (Path.find("/api/v1/inventory") != std::string::npos)
			return "device";
		if (Path.find("/api/v1/subscriberDevice") != std::string::npos ||
			Path.find("/api/v1/sub_devices") != std::string::npos)
			return "device";
		if (Path.find("/api/v1/subscriber") != std::string::npos)
			return "subscriber";
		if (Path.find("/api/v1/configuration") != std::string::npos)
			return "configuration";
		if (Path.find("/api/v1/managementRole") != std::string::npos)
			return "managementRole";
		if (Path.find("/api/v1/managementPolicy") != std::string::npos)
			return "managementPolicy";
		if (Path.find("/api/v1/operator") != std::string::npos)
			return "operator";
		if (Path.find("/api/v1/contact") != std::string::npos || Path.find("/api/v1/op_contact") != std::string::npos)
			return "contact";
		if (Path.find("/api/v1/location") != std::string::npos || Path.find("/api/v1/op_location") != std::string::npos)
			return "location";
		if (Path.find("/api/v1/map") != std::string::npos)
			return "map";
		if (Path.find("/api/v1/variables") != std::string::npos)
			return "variables";
		if (Path.find("/api/v1/radiusEndpoint") != std::string::npos)
			return "radiusEndpoint";
		if (Path.find("/api/v1/openroaming") != std::string::npos)
			return "openroaming";
		if (Path.find("/api/v1/serviceClass") != std::string::npos)
			return "serviceClass";
		if (Path.find("/api/v1/overrides") != std::string::npos)
			return "overrides";
		if (Path.find("/api/v1/iptocountry") != std::string::npos)
			return "iptocountry";
		return "";
	}

	void RESTAPIHandler::AutoCreateCreatorRole(const std::string &CreatedEntityId,
											   const std::string &CreatedVenueId,
											   const std::string &ParentEntityId,
											   const std::string &ParentVenueId) {
		if (UserInfo_.userinfo.userRole == SecurityObjects::ROOT) {
			return;
		}

		ProvObjects::ManagementRole ParentRole;
		bool Found = false;
		if (!ParentVenueId.empty()) {
			Found =
				FindExistingRole(UserInfo_.userinfo.id, ParentEntityId, ParentVenueId, ParentRole);
		}
		if (!Found && !ParentEntityId.empty()) {
			Found = FindExistingRole(UserInfo_.userinfo.id, ParentEntityId, "", ParentRole);
		}

		if (Found) {
			ProvObjects::ManagementRole NewRole;
			NewRole.info.id = MicroServiceCreateUUID();
			NewRole.info.name =
				"Auto-created role for creator of " +
				(CreatedVenueId.empty() ? std::string("Entity") : std::string("Venue"));
			NewRole.info.description =
				"Grants same access policy as parent role: " + ParentRole.info.name;
			NewRole.info.created = Utils::Now();
			NewRole.info.modified = Utils::Now();
			NewRole.entity = CreatedEntityId;
			NewRole.venue = CreatedVenueId;
			NewRole.managementPolicy = ParentRole.managementPolicy;
			NewRole.users.push_back(UserInfo_.userinfo.id);

			if (StorageService()->RolesDB().CreateRecord(NewRole)) {
				poco_information(
					Logger(),
					fmt::format(
						"AutoCreateCreatorRole: Auto-created role {} for user {} on {} (policy {})",
						NewRole.info.id, UserInfo_.userinfo.email,
						CreatedVenueId.empty() ? "entity " + CreatedEntityId
											   : "venue " + CreatedVenueId,
						NewRole.managementPolicy));

				AuthCache::GetInstance()->Clear();

				if (!CreatedVenueId.empty()) {
					AddMembership(StorageService()->VenueDB(), &ProvObjects::Venue::managementRoles,
								  CreatedVenueId, NewRole.info.id);
				} else if (!CreatedEntityId.empty()) {
					AddMembership(StorageService()->EntityDB(),
								  &ProvObjects::Entity::managementRoles, CreatedEntityId,
								  NewRole.info.id);
				}

				MoveUsage(StorageService()->PolicyDB(), StorageService()->RolesDB(), "",
						  NewRole.managementPolicy, NewRole.info.id);
			} else {
				poco_error(Logger(), "AutoCreateCreatorRole: Failed to create role record in DB");
			}
		}
	}

} // namespace OpenWifi
