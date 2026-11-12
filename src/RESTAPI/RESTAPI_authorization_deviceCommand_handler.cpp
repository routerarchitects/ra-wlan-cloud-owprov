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

	static std::string CanonicalDeviceCommandAction(const std::string &action) {
		const std::string upper = Poco::toUpper(action);
		if (upper == "REBOOT" || upper == "CONFIGURE" || upper == "TELEMETRY" ||
			upper == "UPGRADE" || upper == "TRACE" || upper == "RTTY" || upper == "SCRIPT") {
			return upper;
		}
		if (upper == "BLINK" || upper == "LEDS") {
			return "BLINK";
		}
		if (upper == "WIFI_SCAN" || upper == "WIFISCAN") {
			return "WIFI_SCAN";
		}
		if (upper == "FACTORY_RESET" || upper == "FACTORY" || upper == "FACTORYRESET") {
			return "FACTORY_RESET";
		}
		if (upper == "RE_ENROLL" || upper == "REENROLL") {
			return "RE_ENROLL";
		}
		return "";
	}

	static bool PolicyGrantsDeviceCommand(const ProvObjects::ManagementPolicy &policy,
										  const std::string &action) {
		const std::string CanonicalAction = CanonicalDeviceCommandAction(action);
		if (CanonicalAction.empty()) {
			return false;
		}
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
				if (upperAcc == "FULL" || CanonicalDeviceCommandAction(upperAcc) == CanonicalAction) {
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

		// On the internal server, strictly require valid service credentials (X-INTERNAL-NAME
		// and X-API-KEY) before evaluating the caller's Bearer user token — never fall back.
		if (Internal_) {
			if (Request->get("X-INTERNAL-NAME", "").empty() ||
				Request->get("X-API-KEY", "").empty() ||
				!MicroServiceIsValidAPIKEY(*Request)) {
				return UnAuthorized(RESTAPI::Errors::INVALID_TOKEN);
			}
		}

		// On the public server when both X-API-KEY and Authorization are supplied, or when
		// UserInfo_ is still empty (e.g. internal call missing Bearer token), validate Bearer
		// explicitly so X-API-KEY never overrides or bypasses caller identity.
		if ((!Internal_ && Request->has("X-API-KEY") && Request->has("Authorization")) ||
			UserInfo_.userinfo.id.empty()) {
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

			UserInfo_ = SecurityObjects::UserInfoAndPolicy{};
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
			REST_Requester_ = UserInfo_.userinfo.email;
		}

		std::string UserId = UserInfo_.userinfo.id;
		if (UserId.empty()) {
			return UnAuthorized(RESTAPI::Errors::INVALID_TOKEN);
		}

		if (!Utils::NormalizeMac(SerialNumber)) {
			return BadRequest(RESTAPI::Errors::InvalidSerialNumber);
		}
		if (CanonicalDeviceCommandAction(Action).empty()) {
			return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters,
							  fmt::format("Unsupported deviceCommand action '{}'.", Action));
		}
		const std::string UpperAction = Poco::toUpper(Action);

		Poco::JSON::Object Answer;
		Answer.set("allowed", false);
		Answer.set("resource", "deviceCommand");
		Answer.set("action", UpperAction);
		Answer.set("serialNumber", SerialNumber);
		Answer.set("userId", UserId);

		// 1. Look up target device in inventory before checking permissions
		ProvObjects::InventoryTag Device;
		if (!StorageService()->InventoryDB().GetRecord("serialNumber", SerialNumber, Device)) {
			Answer.set("reason", "DEVICE_NOT_FOUND");
			Answer.set("details",
					   fmt::format("Device with serialNumber '{}' was not found in inventory.",
								   SerialNumber));
			return ReturnObject(Answer);
		}

		// 2. Root user exemption: bypass if userRole is ROOT
		if (UserInfo_.userinfo.userRole == SecurityObjects::ROOT) {
			Answer.set("allowed", true);
			Answer.set("reason", "ALLOWED");
			Answer.set("details", "Root user has full administrative command authority.");
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

			// 4B. If no venue role, fallback to the venue's owning Entity Role
			if (!MatchingRole) {
				ProvObjects::Venue V;
				if (StorageService()->VenueDB().GetRecord("id", Device.venue, V) && !V.entity.empty()) {
					for (const auto &role : Roles) {
						if (role.venue.empty() && role.entity == V.entity) {
							MatchingRole = &role;
							break;
						}
					}
				}
			}
		}
		// 5. Branch B: Device belongs directly to an Entity
		else if (!Device.entity.empty()) {
			for (const auto &role : Roles) {
				if (role.venue.empty() && role.entity == Device.entity) {
					MatchingRole = &role;
					break;
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

		// 7. Evaluate Policy on the matched role (via AuthCache, populating cache on miss)
		ProvObjects::ManagementPolicy Policy;
		bool policyLoaded = AuthCache::GetInstance()->GetPolicy(MatchingRole->managementPolicy, Policy);
		if (!policyLoaded &&
			StorageService()->PolicyDB().GetRecord("id", MatchingRole->managementPolicy, Policy)) {
			AuthCache::GetInstance()->SetPolicy(MatchingRole->managementPolicy, Policy);
			policyLoaded = true;
		}
		if (policyLoaded) {

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
