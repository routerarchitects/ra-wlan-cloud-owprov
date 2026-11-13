//
// Created by stephane bourque on 2022-10-25.
//

#include "RESTAPI_Handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "StorageService.h"
#include "framework/SubscriberAccess.h"
#include "framework/MicroServiceFuncs.h"

namespace OpenWifi {

	bool RESTAPIHandler::RoleIsAuthorized(const std::string &Path, const std::string &Method,
								  std::string &Reason) {
		// === Principal Classification Gate (subscriber_rbac.md sec 4.1) ===

		// Track 1: Internal microservice call authenticated via inter-service API key.
		if (Internal_ && InternalServiceAuthenticated_) {
			return true;
		}

		// Track 2: Subscriber self-care (Bearer token with userRole == SUBSCRIBER).
		if (UserInfo_.userinfo.userRole == SecurityObjects::USER_ROLE::SUBSCRIBER) {
			return AuthorizeSubscriberRequest(Path, Method, Reason);
		}
		if (UserInfo_.userinfo.userRole == SecurityObjects::UNKNOWN) {
			Reason = "Unrecognized principal.";
			return false;
		}

		// Track 3: Administrative users (ROOT, ADMIN, CSR, etc.) -- existing logic below.
		// 1. Bypass check only if user is root
		if (UserInfo_.userinfo.userRole == SecurityObjects::ROOT) {
			return true;
		}

		// 2. Map path to resource
		std::string Resource = GetResourceName(Path);
		if (Resource.empty()) {
			Reason = "Unknown or prohibited resource path.";
			Logger_.information(fmt::format("AUTH_DEBUG: Denied - Unknown resource path '{}'", Path));
			return false;
		}

		Logger_.information(fmt::format(
			"AUTH_DEBUG: User='{}' UserRole='{}' Path='{}' Method='{}' Resource='{}'",
			UserInfo_.userinfo.id,
			SecurityObjects::UserTypeToString(UserInfo_.userinfo.userRole), Path, Method,
			Resource));

		if (Resource == "user" &&
			(Method == Poco::Net::HTTPRequest::HTTP_POST ||
			 Method == Poco::Net::HTTPRequest::HTTP_PUT) &&
			ParsedBody_ && ParsedBody_->has("userRole")) {
			const auto RequestedUserRole = ParsedBody_->get("userRole").toString();
			if (Poco::icompare(RequestedUserRole, "root") == 0 &&
				UserInfo_.userinfo.userRole != SecurityObjects::ROOT) {
				Reason = "Only root may assign the root user role.";
				Logger_.information(fmt::format("AUTH_DEBUG: Denied - Non-root trying to assign root role"));
				return false;
			}
		}

		if ((Resource == "managementPolicy" || Resource == "systemConfiguration" ||
			 Resource == "radiusEndpoint" || Resource == "openroaming" ||
			 Resource == "iptocountry" ||
			 (Resource == "managementRole" && Bindings_.find("id") == Bindings_.end())) &&
			Method == Poco::Net::HTTPRequest::HTTP_GET) {
			return true;
		}

		if (Resource == "authorization") {
			return true;
		}

		std::string UserId = UserInfo_.userinfo.id;
		std::vector<ProvObjects::ManagementRole> Roles;
		FindAllUserRoles(UserId, Roles);

		Logger_.information(fmt::format("AUTH_DEBUG: User '{}' has {} management roles", UserId, Roles.size()));

		auto CheckRolePolicy = [&](const ProvObjects::ManagementRole &role) -> bool {
			ProvObjects::ManagementPolicy Policy;
			if (!AuthCache::GetInstance()->GetPolicy(role.managementPolicy, Policy)) {
				if (!StorageService()->PolicyDB().GetRecord("id", role.managementPolicy, Policy)) {
					Logger_.information(fmt::format("AUTH_DEBUG: Policy ID '{}' not found in DB", role.managementPolicy));
					return false;
				}
				AuthCache::GetInstance()->SetPolicy(role.managementPolicy, Policy);
			}
			bool res = PolicyAllows(Policy, Resource, Method);
			Logger_.information(fmt::format("AUTH_DEBUG: PolicyAllows(policy='{}', resource='{}', method='{}') -> {}", Policy.info.id, Resource, Method, res));
			return res;
		};

		// 3. Resolve target Entity and Venue
		std::string TargetEntity, TargetVenue, ScopeError;
		if (!ResolveTargetContext(Path, Method, TargetEntity, TargetVenue, &ScopeError)) {
			Logger_.information(fmt::format("AUTH_DEBUG: ResolveTargetContext returned false for Path='{}'", Path));
			if (!ScopeError.empty()) {
				Reason = ScopeError;
				return false;
			}
			if (HasScopeConstraint(Resource, Method)) {
				Reason = "Scope resolution failed for constrained operation; access denied.";
				Logger_.information(fmt::format("AUTH_DEBUG: Denied - HasScopeConstraint is true but ResolveTargetContext failed"));
				return false;
			}
			for (const auto &role : Roles) {
				if (CheckRolePolicy(role)) {
					Logger_.information(fmt::format("AUTH_DEBUG: Granted global operation via role '{}'", role.info.id));
					return true;
				}
			}
			Reason = "No authorized role found for this target resource and operation.";
			Logger_.information(fmt::format("AUTH_DEBUG: Denied - No role permits global operation for resource '{}'", Resource));
			return false;
		}

		Logger_.information(fmt::format("AUTH_DEBUG: Resolved TargetEntity='{}' TargetVenue='{}'", TargetEntity, TargetVenue));

		// Self-Role Inspection Exemption (Section 6.4):
		// Any authenticated user is inherently authorized to inspect their own assigned Management Roles.
		if (Resource == "managementRole" && Method == Poco::Net::HTTPRequest::HTTP_GET) {
			auto itRole = Bindings_.find("id");
			if (itRole != Bindings_.end() && !itRole->second.empty()) {
				ProvObjects::ManagementRole R;
				if (StorageService()->RolesDB().GetRecord("id", itRole->second, R)) {
					if (std::find(R.users.begin(), R.users.end(), UserInfo_.userinfo.id) != R.users.end()) {
						Logger_.information(fmt::format("AUTH_DEBUG: Granted self-inspection exemption for role '{}'", R.info.id));
						return true;
					}
				}
			}
		}

		if (IsScopeAuthorized(TargetEntity, TargetVenue, Resource, Method, &Reason)) {
			return true;
		}

		if (Reason.empty()) {
			Reason = "No authorized role matches the required scope and permission.";
		}
		Logger_.information(fmt::format("AUTH_DEBUG: Denied for Path='{}'. Reason='{}'", Path, Reason));
		return false;
	}

	bool RESTAPIHandler::IsScopeAuthorized(const std::string &TargetEntity,
										   const std::string &TargetVenue,
										   const std::string &Resource,
										   const std::string &Method,
										   std::string *Reason) {
		if (UserInfo_.userinfo.userRole == SecurityObjects::ROOT) {
			return true;
		}

		std::vector<ProvObjects::ManagementRole> Roles;
		FindAllUserRoles(UserInfo_.userinfo.id, Roles);

		auto CheckRolePolicy = [&](const ProvObjects::ManagementRole &role) -> bool {
			ProvObjects::ManagementPolicy Policy;
			if (!AuthCache::GetInstance()->GetPolicy(role.managementPolicy, Policy)) {
				if (!StorageService()->PolicyDB().GetRecord("id", role.managementPolicy, Policy)) {
					Logger_.information(fmt::format("AUTH_DEBUG: Policy ID '{}' not found in DB", role.managementPolicy));
					return false;
				}
				AuthCache::GetInstance()->SetPolicy(role.managementPolicy, Policy);
			}
			bool res = PolicyAllows(Policy, Resource, Method);
			Logger_.information(fmt::format("AUTH_DEBUG: PolicyAllows(policy='{}', resource='{}', method='{}') -> {}", Policy.info.id, Resource, Method, res));
			return res;
		};

		if (!TargetVenue.empty()) {
			bool foundSpecificVenueRole = false;
			for (const auto &role : Roles) {
				if (role.venue == TargetVenue) {
					foundSpecificVenueRole = true;
					if (CheckRolePolicy(role)) {
						return true;
					}
				}
			}
			if (foundSpecificVenueRole) {
				if (Reason) {
					*Reason = "Specific venue role policy denied access.";
				}
				Logger_.information(fmt::format("AUTH_DEBUG: Denied by specific venue role policy"));
				return false;
			}

			for (const auto &role : Roles) {
				if (role.entity == TargetEntity && (role.venue.empty() || role.venue == "")) {
					std::set<std::string> AllowedVenues;
					ProvObjects::Entity EntRec;
					if (StorageService()->EntityDB().GetRecord("id", role.entity, EntRec)) {
						for (const auto &vId : EntRec.venues) {
							GetDescendantVenues(vId, AllowedVenues);
						}
					}
					if (AllowedVenues.find(TargetVenue) != AllowedVenues.end()) {
						if (CheckRolePolicy(role)) {
							return true;
						}
					}
				}
			}
		} else {
			for (const auto &role : Roles) {
				Logger_.information(fmt::format("AUTH_DEBUG: Comparing role.entity='{}' with TargetEntity='{}'", role.entity, TargetEntity));
				if (role.entity == TargetEntity && (role.venue.empty() || role.venue == "")) {
					if (CheckRolePolicy(role)) {
						Logger_.information(fmt::format("AUTH_DEBUG: Granted access via matching role.entity='{}' with policy='{}'", role.entity, role.managementPolicy));
						return true;
					}
				}
			}
		}

		if (Reason) {
			*Reason = "No authorized role matches the required scope and permission.";
		}
		return false;
	}

	bool RESTAPIHandler::ResolveTargetContext(const std::string &Path, const std::string &Method,
											  std::string &TargetEntity, std::string &TargetVenue,
											  std::string *ErrorReason) {
		if (ErrorReason) {
			ErrorReason->clear();
		}
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
				it = Bindings_.find("serialNumber");
				if (it != Bindings_.end()) {
					Id = it->second;
				} else {
					it = Bindings_.find("serialnumber");
					if (it != Bindings_.end()) {
						Id = it->second;
					}
				}
			}
		}

		const bool IsInventoryCreate =
			(Method == Poco::Net::HTTPRequest::HTTP_POST &&
			 Path.find("/api/v1/inventory") != std::string::npos);

		const bool HasBoundObjectId =
			!Id.empty() && !IsInventoryCreate &&
			!(Method == Poco::Net::HTTPRequest::HTTP_POST && (Id == "0" || Id == "1" || Poco::icompare(Id, "new") == 0));

		Logger_.information(fmt::format("RESOLVE_DEBUG: Path='{}' Method='{}' Id='{}' HasBoundObjectId={}", Path, Method, Id, HasBoundObjectId));
		for (const auto &[bKey, bVal] : Bindings_) {
			Logger_.information(fmt::format("RESOLVE_DEBUG: Binding key='{}' val='{}'", bKey, bVal));
		}

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
					if (TargetEntity.empty() && !TargetVenue.empty()) {
						ProvObjects::Venue V;
						if (StorageService()->VenueDB().GetRecord("id", TargetVenue, V)) {
							TargetEntity = V.entity;
						}
					}
					if (TargetEntity.empty() && !T.subscriber.empty()) {
						ProvObjects::SignupEntry SE;
						if (StorageService()->SignupDB().GetRecord("userid", T.subscriber, SE) ||
							StorageService()->SignupDB().GetRecord("id", T.subscriber, SE)) {
							ProvObjects::Operator O;
							if (StorageService()->OperatorDB().GetRecord("id", SE.operatorId, O)) {
								TargetEntity = O.entityId;
							}
						}
					}
					if (!TargetEntity.empty() || !TargetVenue.empty()) {
						return true;
					}
				}
				// Fallback to SubscriberDeviceDB if device is in sub_devices
				ProvObjects::SubscriberDevice SD;
				if (StorageService()->SubscriberDeviceDB().GetRecord("id", Id, SD) ||
					StorageService()->SubscriberDeviceDB().GetRecord("serialNumber", Id, SD)) {
					if (!SD.operatorId.empty()) {
						ProvObjects::Operator O;
						if (StorageService()->OperatorDB().GetRecord("id", SD.operatorId, O)) {
							TargetEntity = O.entityId;
							return true;
						}
						ProvObjects::Entity E;
						if (StorageService()->EntityDB().GetRecord("operatorId", SD.operatorId, E) ||
							StorageService()->EntityDB().GetRecord("id", SD.operatorId, E)) {
							TargetEntity = E.info.id;
							return true;
						}
					}
				}
				return false;
			} else if (Path.find("/api/v1/configurationOverrides") != std::string::npos ||
					   Path.find("/api/v1/overrides") != std::string::npos) {
				ProvObjects::InventoryTag T;
				bool foundTag = StorageService()->InventoryDB().GetRecord("id", Id, T) ||
								StorageService()->InventoryDB().GetRecord(RESTAPI::Protocol::SERIALNUMBER, Id, T);
				Logger_.information(fmt::format("RESOLVE_DEBUG: configurationOverrides lookup Id='{}' foundTag={}", Id, foundTag));
				if (foundTag) {
					TargetEntity = T.entity;
					TargetVenue = T.venue;
					Logger_.information(fmt::format("RESOLVE_DEBUG: Tag T.entity='{}' T.venue='{}'", T.entity, T.venue));
					if (TargetEntity.empty() && !TargetVenue.empty()) {
						ProvObjects::Venue V;
						if (StorageService()->VenueDB().GetRecord("id", TargetVenue, V)) {
							TargetEntity = V.entity;
						}
					}
					if (!TargetEntity.empty() || !TargetVenue.empty()) {
						return true;
					}
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
			} else if (Path.find("/api/v1/managementRole") != std::string::npos ||
					   Path.find("/api/v2/managementRole") != std::string::npos) {
				ProvObjects::ManagementRole R;
				if (StorageService()->RolesDB().GetRecord("id", Id, R)) {
					TargetEntity = R.entity;
					TargetVenue = R.venue;
					return true;
				}
				if (Method != Poco::Net::HTTPRequest::HTTP_POST) {
					return false;
				}
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
				if (Method != Poco::Net::HTTPRequest::HTTP_POST) {
					return false;
				}
			} else if (Path.find("/api/v1/map") != std::string::npos) {
				ProvObjects::Map M;
				if (StorageService()->MapDB().GetRecord("id", Id, M)) {
					TargetEntity = M.entity;
					TargetVenue = M.venue;
					return true;
				}
				return false;
			} else if (Path.find("/api/v1/variable") != std::string::npos) {
				ProvObjects::VariableBlock V;
				if (StorageService()->VariablesDB().GetRecord("id", Id, V)) {
					TargetEntity = V.entity;
					TargetVenue = V.venue;
					if (TargetEntity.empty() && !TargetVenue.empty()) {
						ProvObjects::Venue Ven;
						if (StorageService()->VenueDB().GetRecord("id", TargetVenue, Ven)) {
							TargetEntity = Ven.entity;
						}
					}
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
			if (Method != Poco::Net::HTTPRequest::HTTP_POST) {
				return false;
			}
		}

		// ----------------------------------------------------------------
		// SOURCE 2 & 3: Query parameters and request body.
		// Extract both independently, reject any conflicting scope between
		// query parameters and request body, and prioritize ParsedBody_
		// over query parameters so query params cannot shadow body scope.
		// ----------------------------------------------------------------
		std::string QueryEntity, QueryVenue, QueryOperator;
		for (const auto &[name, value] : Parameters_) {
			if (name == "entity")
				QueryEntity = value;
			else if (name == "venue")
				QueryVenue = value;
			else if (name == "operatorId" || name == "operator" || name == "registrationId")
				QueryOperator = value;
			else if (name == "subscriberId" || name == "signupUUID") {
				ProvObjects::SignupEntry SE;
				if (StorageService()->SignupDB().GetRecord("userid", value, SE) ||
					StorageService()->SignupDB().GetRecord("id", value, SE)) {
					QueryOperator = SE.operatorId;
				}
			}
		}

		// When creating an operator (POST /api/v1/operator/0), its companion entity is always
		// attached directly to the Root Entity (EntityDB::RootUUID()). Target scope is therefore Root Entity.
		// Handle this before checking CandidateOperator so a new registrationId is not looked up in DB.
		if (Path.find("/api/v1/operator") != std::string::npos && Method == Poco::Net::HTTPRequest::HTTP_POST) {
			TargetEntity = EntityDB::RootUUID();
			TargetVenue = "";
			return true;
		}

		std::string BodyEntity, BodyVenue, BodyOperator;
		if (ParsedBody_) {
			auto ExtractString = [&](const char *Key) -> std::string {
				if (ParsedBody_->has(Key) && !ParsedBody_->isNull(Key)) {
					return ParsedBody_->get(Key).toString();
				}
				return "";
			};

			BodyEntity = ExtractString("entity");
			BodyVenue = ExtractString("venue");
			if (BodyVenue.empty() && ParsedBody_->isArray("venueIds")) {
				auto Array = ParsedBody_->getArray("venueIds");
				if (Array && Array->size() > 0) {
					BodyVenue = Array->get(0).toString();
				}
			}
			BodyOperator = ExtractString("operatorId");
			if (BodyOperator.empty())
				BodyOperator = ExtractString("operator");
			if (BodyOperator.empty())
				BodyOperator = ExtractString("registrationId");

			if (BodyEntity.empty() && BodyVenue.empty() && BodyOperator.empty()) {
				std::string ParentId = ExtractString("parent");
				if (!ParentId.empty()) {
					if (Path.find("/api/v1/entity") != std::string::npos) {
						BodyEntity = ParentId;
					} else if (Path.find("/api/v1/venue") != std::string::npos) {
						BodyVenue = ParentId;
					}
				}
			}
		}

		auto SetScopeError = [&](const std::string &Msg) {
			Logger_.information(fmt::format("AUTH_DEBUG: {}", Msg));
			if (ErrorReason) {
				*ErrorReason = Msg;
			}
		};

		struct ScopeFieldCheck {
			const char *Name;
			const std::string &Query;
			const std::string &Body;
		};
		for (const auto &Field : std::initializer_list<ScopeFieldCheck>{
				 {"entity", QueryEntity, BodyEntity},
				 {"venue", QueryVenue, BodyVenue},
				 {"operator", QueryOperator, BodyOperator}}) {
			if (!Field.Query.empty() && !Field.Body.empty() && Field.Query != Field.Body) {
				SetScopeError(fmt::format(
					"Target scope mismatch: query parameter {} '{}' differs from request body {} '{}'.",
					Field.Name, Field.Query, Field.Name, Field.Body));
				return false;
			}
		}
		if ((!QueryEntity.empty() && BodyEntity.empty() && (!BodyVenue.empty() || !BodyOperator.empty())) ||
			(!QueryVenue.empty() && BodyVenue.empty() && (!BodyEntity.empty() || !BodyOperator.empty())) ||
			(!QueryOperator.empty() && BodyOperator.empty() && (!BodyEntity.empty() || !BodyVenue.empty()))) {
			SetScopeError(
				"Target scope mismatch: query parameter scope differs from request body scope.");
			return false;
		}

		const bool HasBodyScope =
			!BodyEntity.empty() || !BodyVenue.empty() || !BodyOperator.empty();
		std::string CandidateEntity = HasBodyScope ? BodyEntity : QueryEntity;
		std::string CandidateVenue = HasBodyScope ? BodyVenue : QueryVenue;
		std::string CandidateOperator = HasBodyScope ? BodyOperator : QueryOperator;

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
				TargetEntity = V.entity.empty() ? FindParentEntity(V) : V.entity;
				if (!CandidateEntity.empty() && TargetEntity != CandidateEntity) {
					SetScopeError(fmt::format(
						"Target scope mismatch: venue '{}' belongs to entity '{}', not '{}'.",
						CandidateVenue, TargetEntity, CandidateEntity));
					return false;
				}
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
			Logger_.information(fmt::format("RESOLVE_DEBUG: Resolving CandidateOperator='{}'", CandidateOperator));
			ProvObjects::Entity E;
			if (StorageService()->EntityDB().GetRecord("operatorId", CandidateOperator, E)) {
				TargetEntity = E.info.id;
				Logger_.information(fmt::format("RESOLVE_DEBUG: Found Entity by operatorId, TargetEntity='{}'", TargetEntity));
				return true;
			}
			if (StorageService()->EntityDB().GetRecord("id", CandidateOperator, E)) {
				TargetEntity = E.info.id;
				Logger_.information(fmt::format("RESOLVE_DEBUG: Found Entity by id, TargetEntity='{}'", TargetEntity));
				return true;
			}
			ProvObjects::Operator O;
			if (StorageService()->OperatorDB().GetRecord("id", CandidateOperator, O) ||
				StorageService()->OperatorDB().GetRecord("registrationId", CandidateOperator, O)) {
				Logger_.information(fmt::format("RESOLVE_DEBUG: Found Operator id='{}' registrationId='{}' entityId='{}'", O.info.id, O.registrationId, O.entityId));
				if (!O.entityId.empty() && StorageService()->EntityDB().Exists("id", O.entityId)) {
					TargetEntity = O.entityId;
					Logger_.information(fmt::format("RESOLVE_DEBUG: Resolved TargetEntity='{}' from O.entityId", TargetEntity));
					return true;
				}
				if (StorageService()->EntityDB().GetRecord("operatorId", O.info.id, E)) {
					TargetEntity = E.info.id;
					Logger_.information(fmt::format("RESOLVE_DEBUG: Resolved TargetEntity='{}' from EntityDB.operatorId", TargetEntity));
					return true;
				}
				TargetEntity = O.entityId.empty() ? O.info.id : O.entityId;
				Logger_.information(fmt::format("RESOLVE_DEBUG: Fallback TargetEntity='{}'", TargetEntity));
				return true;
			}
			Logger_.information(fmt::format("RESOLVE_DEBUG: CandidateOperator='{}' not found in OperatorDB or EntityDB", CandidateOperator));
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
			 Resource == "configuration" || Resource == "managementRole" ||
			 Resource == "contact" || Resource == "location" || Resource == "managementPolicy" ||
			 Resource == "map" || Resource == "variables" || Resource == "variable" || Resource == "serviceClass" ||
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

		const bool IsInventoryCreate =
			(Method == Poco::Net::HTTPRequest::HTTP_POST &&
			 Resource == "inventory");

		if (!Id.empty() && !IsInventoryCreate &&
			!(Method == Poco::Net::HTTPRequest::HTTP_POST && (Id == "0" || Id == "1" || Poco::icompare(Id, "new") == 0))) {
			return true;
		}

		if (Resource == "operator" && Method == Poco::Net::HTTPRequest::HTTP_POST) {
			return true;
		}

		for (const auto &[name, value] : Parameters_) {
			if (name == "entity" || name == "venue" || name == "operatorId" ||
				name == "operator" || name == "registrationId" ||
				name == "subscriberId" || name == "signupUUID") {
				return true;
			}
		}

		if (ParsedBody_) {
			if (ParsedBody_->has("entity") || ParsedBody_->has("venue") ||
				ParsedBody_->has("venueIds") || ParsedBody_->has("operatorId") ||
				ParsedBody_->has("operator") || ParsedBody_->has("registrationId") ||
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
					((Resource == "variable" || Resource == "variables") && Poco::icompare(res, "configuration") == 0) ||
					(Resource == "map" && (Poco::icompare(res, "venue") == 0 || Poco::icompare(res, "entity") == 0)) ||
					(Resource == "serviceClass" && (Poco::icompare(res, "operator") == 0 || Poco::icompare(res, "entity") == 0)) ||
					(Resource == "subscriberDevice" && Poco::icompare(res, "inventory") == 0) ||
					(Resource == "overrides" && Poco::icompare(res, "inventory") == 0) ||
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
					(AccessRequired == "CREATE" && (acc == "MODIFY" || acc == "READWRITE")) ||
					(AccessRequired == "UPDATE" && (acc == "UPDATE" || acc == "MODIFY" || acc == "READWRITE")) ||
					(AccessRequired == "READ" && (acc == "READWRITE" || acc == "MODIFY" || acc == "CREATE" || acc == "UPDATE" || acc == "DELETE"))) {
					return true;
				}
			}
		}
		return false;
	}

	// Tier 1 subscriber authorization; failures never fall through to admin RBAC.
	bool RESTAPIHandler::AuthorizeSubscriberRequest(const std::string &Path,
		const std::string &Method, std::string &Reason) {
		using SubscriberAccess::Operation;
		const std::string CallerId = UserInfo_.userinfo.id;
		if (CallerId.empty()) {
			Reason = "Subscriber identity context is missing or unresolvable.";
			return false;
		}
		std::string targetId;
		// The legacy admin resource mapper matches substrings, including query strings.
		// Subscriber capabilities instead match the decoded URI path exactly.
		const auto operation = SubscriberAccess::Resolve(
			Method, Poco::URI(Path).getPath(), CallerId, targetId);
		if (operation == Operation::Denied) {
			Reason = "Method, path or identity not permitted for subscriber self-care.";
			return false;
		}

		// A supplied identity is a consistency assertion, never a scope selector.
		for (const auto &[name, value] : Parameters_) {
			if (name == "subscriberId" && value != CallerId) {
				Reason = "subscriberId must match authenticated subscriber identity.";
				return false;
			}
		}
		if (ParsedBody_ && ParsedBody_->has("subscriberId")) {
			const auto value = ParsedBody_->get("subscriberId");
			if (!value.isString()) {
				BadRequest(RESTAPI::Errors::InvalidJSONDocument);
				return false;
			}
			if (value.toString() != CallerId) {
				Reason = "subscriberId must match authenticated subscriber identity.";
				return false;
			}
		}

		if (operation == Operation::ClaimDevice) {
			if (!ParsedBody_) {
				BadRequest(RESTAPI::Errors::InvalidJSONDocument);
				return false;
			}
			for (const auto &field : *ParsedBody_) {
				if (!SubscriberAccess::IsClaimFieldAllowed(field.first)) {
					Reason = "Property not permitted on subscriber device claim.";
					return false;
				}
				if ((field.first == "configuration" && !ParsedBody_->isArray(field.first)) ||
					(field.first != "configuration" && !field.second.isString())) {
					BadRequest(RESTAPI::Errors::InvalidJSONDocument);
					return false;
				}
			}
			if (ParsedBody_->has("deviceConfiguration")) {
				const auto configuration = ParsedBody_->get("deviceConfiguration");
				if (!configuration.isString()) {
					BadRequest(RESTAPI::Errors::InvalidJSONDocument);
					return false;
				}
				if (!configuration.toString().empty()) {
					Reason = "Subscribers may not reference an existing configuration.";
					return false;
				}
			}
			return true;
		}

		if (operation == Operation::ListDevices || operation == Operation::DeleteAccount)
			return true;

		ProvObjects::SubscriberDevice device;
		bool found;
		if (Utils::ValidUUID(targetId)) {
			found = StorageService()->SubscriberDeviceDB().GetRecord("id", targetId, device);
		} else if (Utils::ValidSerialNumber(targetId)) {
			found = StorageService()->SubscriberDeviceDB().GetRecord("serialNumber", targetId, device);
		} else {
			BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
			return false;
		}
		if (!found || device.subscriberId != CallerId) {
			Logger_.information(fmt::format("SUBSCRIBER_RBAC: {} target='{}' caller='{}'",
				found ? "Foreign resource ownership" : "Resource not found in database",
				targetId, CallerId));
			NotFound();
			return false;
		}
		MemoizedSubscriberDevice_ = std::move(device);
		MemoizedDeviceLoaded_ = true;
		return true;
	}

	bool AuthCache::GetUserRoles(const std::string &userId,
								 std::vector<ProvObjects::ManagementRole> &roles) {
		std::shared_lock<std::shared_mutex> lock(Mutex_);
		auto it = Cache_.find(userId);
		if (it != Cache_.end() && !it->second.roles.empty()) {
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
			if (!Roles.empty()) {
				AuthCache::GetInstance()->SetUserRoles(userId, Roles);
			}
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
			return "inventory";
		if (Path.find("/api/v1/subscriberDevice") != std::string::npos ||
			Path.find("/api/v1/sub_devices") != std::string::npos)
			return "subscriberDevice";
		if (Path.find("/api/v1/subscriber") != std::string::npos)
			return "subscriber";
		if (Path.find("/api/v1/configurationOverrides") != std::string::npos ||
			Path.find("/api/v1/overrides") != std::string::npos)
			return "overrides";
		if (Path.find("/api/v1/configuration") != std::string::npos)
			return "configuration";
		if (Path.find("/api/v1/managementRole") != std::string::npos ||
			Path.find("/api/v2/managementRole") != std::string::npos)
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
		if (Path.find("/api/v1/variable") != std::string::npos)
			return "variable";
		if (Path.find("/api/v1/radiusEndpoint") != std::string::npos ||
			Path.find("/api/v1/RADIUSEndPoints") != std::string::npos)
			return "radiusEndpoint";
		if (Path.find("/api/v1/openroaming") != std::string::npos)
			return "openroaming";
		if (Path.find("/api/v1/serviceClass") != std::string::npos)
			return "serviceClass";
		if (Path.find("/api/v1/configurationOverrides") != std::string::npos ||
			Path.find("/api/v1/overrides") != std::string::npos)
			return "overrides";
		if (Path.find("/api/v1/iptocountry") != std::string::npos)
			return "iptocountry";
		if (Path.find("/api/v1/authorization") != std::string::npos)
			return "authorization";
		if (Path.find("/api/v1/systemConfiguration") != std::string::npos)
			return "systemConfiguration";
		return "";
	}

	bool RESTAPIHandler::AutoCreateCreatorRole(const std::string &CreatedEntityId,
											   const std::string &CreatedVenueId,
											   const std::string &ParentEntityId,
											   const std::string &ParentVenueId) {
		if (UserInfo_.userinfo.userRole == SecurityObjects::ROOT) {
			return true;
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
				return true;
			} else {
				poco_error(Logger(), "AutoCreateCreatorRole: Failed to create role record in DB");
				return false;
			}
		}
		return true;
	}

} // namespace OpenWifi
