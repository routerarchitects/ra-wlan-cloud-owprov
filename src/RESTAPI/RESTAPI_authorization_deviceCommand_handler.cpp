//
// Created for OpenWifi Provisioning.
//

#include "RESTAPI_authorization_deviceCommand_handler.h"
#include "RESTAPI_db_helpers.h"
#include "StorageService.h"
#include "framework/utils.h"
#include "Poco/String.h"
#include "fmt/format.h"

namespace OpenWifi {

	static bool PolicyGrantsDeviceCommand(const ProvObjects::ManagementPolicy &policy,
										  const std::string &action) {
		const std::string UpperAction = Poco::toUpper(action);
		for (const auto &entry : policy.entries) {
			bool resourceMatches = false;
			for (const auto &res : entry.resources) {
				if (Poco::icompare(res, "deviceCommand") == 0 || res == "*") {
					resourceMatches = true;
					break;
				}
			}
			if (!resourceMatches)
				continue;

			for (const auto &acc : entry.access) {
				const std::string upperAcc = Poco::toUpper(acc);
				if (upperAcc == "FULL" || upperAcc == UpperAction ||
					((UpperAction == "BLINK" || UpperAction == "LEDS") && (upperAcc == "BLINK" || upperAcc == "LEDS")) ||
					((UpperAction == "WIFI_SCAN" || UpperAction == "WIFISCAN") && (upperAcc == "WIFI_SCAN" || upperAcc == "WIFISCAN")) ||
					((UpperAction == "FACTORY_RESET" || UpperAction == "FACTORY" || UpperAction == "FACTORYRESET") &&
					 (upperAcc == "FACTORY_RESET" || upperAcc == "FACTORY" || upperAcc == "FACTORYRESET")) ||
					((UpperAction == "RE_ENROLL" || UpperAction == "REENROLL") &&
					 (upperAcc == "RE_ENROLL" || upperAcc == "REENROLL"))) {
					return true;
				}
			}
		}
		return false;
	}

	void RESTAPI_authorization_deviceCommand_handler::DoPost() {
		const auto &RawObject = ParsedBody_;
		if (!RawObject) {
			return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
		}

		std::string SerialNumber =
			RawObject->has("serialNumber") ? RawObject->get("serialNumber").toString() : "";
		std::string Action = RawObject->has("action") ? RawObject->get("action").toString() : "";

		if (SerialNumber.empty() || Action.empty()) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
		}

		// On internal server, service key was verified but user token must be validated explicitly
		if (UserInfo_.userinfo.id.empty()) {
			std::string Token;
			try {
				Poco::Net::OAuth20Credentials Auth(*Request);
				if (Auth.getScheme() == "Bearer") {
					Token = Auth.getBearerToken();
				}
			} catch (...) {}

			if (Token.empty()) {
				return UnAuthorized(RESTAPI::Errors::INVALID_TOKEN);
			}

			bool Expired = false, Contacted = false;
			if (!AuthClient()->IsAuthorized(Token, UserInfo_, TransactionId_, Expired, Contacted, false)) {
				if (Expired) {
					return UnAuthorized(RESTAPI::Errors::EXPIRED_TOKEN);
				}
				if (Contacted) {
					return UnAuthorized(RESTAPI::Errors::INVALID_TOKEN);
				}
				return UnAuthorized(RESTAPI::Errors::SECURITY_SERVICE_UNREACHABLE);
			}
		}

		std::string UserId = UserInfo_.userinfo.id;
		if (UserId.empty()) {
			return UnAuthorized(RESTAPI::Errors::INVALID_TOKEN);
		}

		Utils::NormalizeMac(SerialNumber);
		const std::string UpperAction = Poco::toUpper(Action);

		Poco::JSON::Object Answer;
		Answer.set("allowed", false);
		Answer.set("resource", "deviceCommand");
		Answer.set("action", UpperAction);
		Answer.set("serialNumber", SerialNumber);
		Answer.set("userId", UserId);

		// 1. Root user exemption: bypass if userRole is ROOT
		if (UserInfo_.userinfo.userRole == SecurityObjects::ROOT) {
			Answer.set("allowed", true);
			Answer.set("reason", "ALLOWED");
			Answer.set("details", "Root user has full administrative command authority.");
			return ReturnObject(Answer);
		}

		// 2. Look up target device in inventory
		ProvObjects::InventoryTag Device;
		if (!StorageService()->InventoryDB().GetRecord("serialNumber", SerialNumber, Device)) {
			Answer.set("reason", "DEVICE_NOT_FOUND");
			Answer.set("details",
					   fmt::format("Device with serialNumber '{}' was not found in inventory.",
								   SerialNumber));
			return ReturnObject(Answer);
		}

		// 3. Look up user roles
		std::vector<ProvObjects::ManagementRole> Roles;
		FindAllUserRoles(UserId, Roles);

		if (Roles.empty()) {
			Answer.set("reason", "USER_HAS_NO_ROLES");
			Answer.set("details",
					   fmt::format("User '{}' has no management roles assigned.", UserId));
			return ReturnObject(Answer);
		}

		const ProvObjects::ManagementRole *MatchingRole = nullptr;

		// 4. Branch A: Device belongs to a Venue
		if (!Device.venue.empty()) {
			// 4A. Check for authoritative Venue Role first
			for (const auto &role : Roles) {
				if (role.venue == Device.venue) {
					MatchingRole = &role;
					break;
				}
			}

			// 4B. If no venue role, fallback to parent Entity Role
			if (!MatchingRole) {
				ProvObjects::Venue V;
				if (StorageService()->VenueDB().GetRecord("id", Device.venue, V) && !V.entity.empty()) {
					for (const auto &role : Roles) {
						if (role.venue.empty()) {
							if (role.entity == V.entity) {
								MatchingRole = &role;
								break;
							}
							std::set<std::string> AllowedVenues;
							ProvObjects::Entity EntRec;
							if (StorageService()->EntityDB().GetRecord("id", role.entity, EntRec)) {
								for (const auto &vId : EntRec.venues) {
									GetDescendantVenues(vId, AllowedVenues);
								}
							}
							if (AllowedVenues.find(Device.venue) != AllowedVenues.end()) {
								MatchingRole = &role;
								break;
							}
						}
					}
				}
			}
		}
		// 5. Branch B: Device belongs directly to an Entity
		else if (!Device.entity.empty()) {
			for (const auto &role : Roles) {
				if (role.venue.empty()) {
					if (role.entity == Device.entity) {
						MatchingRole = &role;
						break;
					}
					std::set<std::string> DescendantEntities;
					GetDescendantEntities(role.entity, DescendantEntities);
					if (DescendantEntities.find(Device.entity) != DescendantEntities.end()) {
						MatchingRole = &role;
						break;
					}
				}
			}
		}

		// 6. No role matched the device's scope -> OUT_OF_SCOPE
		if (!MatchingRole) {
			Answer.set("allowed", false);
			Answer.set("reason", "OUT_OF_SCOPE");
			Answer.set(
				"details",
				fmt::format("User '{}' has no management role covering device '{}'.", UserId,
							SerialNumber));
			return ReturnObject(Answer);
		}

		// 7. Evaluate Policy on the matched role
		ProvObjects::ManagementPolicy Policy;
		if (AuthCache::GetInstance()->GetPolicy(MatchingRole->managementPolicy, Policy) ||
			StorageService()->PolicyDB().GetRecord("id", MatchingRole->managementPolicy, Policy)) {

			const std::string PolicyDisplayName =
				Policy.info.name.empty() ? Policy.info.id : Policy.info.name;

			if (PolicyGrantsDeviceCommand(Policy, UpperAction)) {
				Answer.set("allowed", true);
				Answer.set("reason", "ALLOWED");
				Answer.set(
					"details",
					fmt::format(
						"User is authorized for action '{}' on device '{}' via policy '{}'.",
						UpperAction, SerialNumber, PolicyDisplayName));
				Answer.set("policyId", Policy.info.id);
				Answer.set("policyName", PolicyDisplayName);
				return ReturnObject(Answer);
			} else {
				Answer.set("allowed", false);
				Answer.set("reason", "ACTION_NOT_GRANTED");
				Answer.set(
					"details",
					fmt::format("Policy '{}' does not grant action '{}' on deviceCommand.",
								PolicyDisplayName, UpperAction));
				Answer.set("policyId", Policy.info.id);
				Answer.set("policyName", PolicyDisplayName);
				return ReturnObject(Answer);
			}
		}

		Answer.set("allowed", false);
		Answer.set("reason", "POLICY_NOT_FOUND");
		Answer.set("details",
				   fmt::format("Management policy '{}' for role was not found.",
							   MatchingRole->managementPolicy));
		return ReturnObject(Answer);
	}

} // namespace OpenWifi
