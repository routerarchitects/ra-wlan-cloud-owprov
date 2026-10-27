//
// Created by stephane bourque on 2022-02-23.
//

#include "RESTAPI_variables_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "StorageService.h"
#include "framework/utils.h"
#include <algorithm>
#include <set>

namespace OpenWifi {

	void RESTAPI_variables_list_handler::DoGet() {
		std::string entityParam;
		if (HasParameter("entity", entityParam) && !entityParam.empty()) {
			if (!Utils::ValidUUID(entityParam)) {
				return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
			}
		}

		std::string venueParam;
		if (HasParameter("venue", venueParam) && !venueParam.empty()) {
			if (!Utils::ValidUUID(venueParam)) {
				return BadRequest(RESTAPI::Errors::MissingOrInvalidParameters);
			}
		}

		bool isRoot = (UserInfo_.userinfo.userRole == SecurityObjects::ROOT);

		// If caller is ROOT, preserve standard full-listing behavior
		if (isRoot) {
			if (!QB_.Select.empty()) {
				ProvObjects::VariableBlockList ResultList;
				for (const auto &id : SelectedRecords()) {
					ProvObjects::VariableBlock record;
					if (DB_.GetRecord("id", id, record)) {
						if (!entityParam.empty() && record.entity != entityParam) {
							continue;
						}
						if (!venueParam.empty() && record.venue != venueParam) {
							continue;
						}
						ResultList.variableBlocks.push_back(record);
					}
				}
				if (QB_.CountOnly) {
					return ReturnCountOnly(ResultList.variableBlocks.size());
				}
				return MakeJSONObjectArray("variableBlocks", ResultList.variableBlocks, *this);
			}
			return ListHandler<VariablesDB>("variableBlocks", DB_, *this);
		}

		// Non-ROOT: Gather allowed entities and venues where caller holds configuration:READ
		std::set<std::string> AllowedEntities;
		std::set<std::string> AllowedVenues;
		std::set<std::string> DeniedVenues;
		std::vector<ProvObjects::ManagementRole> RequesterRoles;

		auto policyAllowsConfigurationGet = [&](const ProvObjects::ManagementRole &role) -> bool {
			ProvObjects::ManagementPolicy Policy;
			if (!AuthCache::GetInstance()->GetPolicy(role.managementPolicy, Policy)) {
				if (!StorageService()->PolicyDB().GetRecord("id", role.managementPolicy, Policy)) {
					return false;
				}
				AuthCache::GetInstance()->SetPolicy(role.managementPolicy, Policy);
			}
			return PolicyAllows(Policy, "configuration", Poco::Net::HTTPRequest::HTTP_GET);
		};

		if (FindAllUserRoles(UserInfo_.userinfo.id, RequesterRoles)) {
			for (const auto &role : RequesterRoles) {
				if (!role.venue.empty()) {
					if (policyAllowsConfigurationGet(role)) {
						AllowedVenues.insert(role.venue);
					} else {
						DeniedVenues.insert(role.venue);
					}
				} else if (!role.entity.empty()) {
					if (policyAllowsConfigurationGet(role)) {
						std::set<std::string> descendantEntities;
						RESTAPIHandler::GetDescendantEntities(role.entity, descendantEntities);
						for (const auto &eId : descendantEntities) {
							AllowedEntities.insert(eId);
							ProvObjects::Entity E;
							if (StorageService()->EntityDB().GetRecord("id", eId, E)) {
								for (const auto &vId : E.venues) {
									RESTAPIHandler::GetDescendantVenues(vId, AllowedVenues);
								}
							}
						}
					}
				}
			}
			for (const auto &vId : DeniedVenues) {
				AllowedVenues.erase(vId);
			}
		}

		auto isVariableAuthorized = [&](const ProvObjects::VariableBlock &varBlock) -> bool {
			if (!varBlock.venue.empty()) {
				if (DeniedVenues.count(varBlock.venue)) {
					return false;
				}
				if (AllowedVenues.count(varBlock.venue)) {
					return true;
				}
				// If not found in AllowedVenues, check parent entity of venue
				ProvObjects::Venue ven;
				if (StorageService()->VenueDB().GetRecord("id", varBlock.venue, ven)) {
					if (!ven.entity.empty() && AllowedEntities.count(ven.entity)) {
						return true;
					}
				}
				return false;
			}
			if (!varBlock.entity.empty()) {
				return AllowedEntities.count(varBlock.entity) > 0;
			}
			return false;
		};

		// Case 1: Multi-ID selection (?select=id1,id2,id3...)
		if (!QB_.Select.empty()) {
			ProvObjects::VariableBlockList ResultList;
			for (const auto &id : SelectedRecords()) {
				ProvObjects::VariableBlock record;
				if (!DB_.GetRecord("id", id, record)) {
					continue;
				}

				if (!entityParam.empty() && record.entity != entityParam) {
					continue;
				}
				if (!venueParam.empty() && record.venue != venueParam) {
					continue;
				}

				// Skip variable if caller lacks configuration GET on its scope
				if (!isVariableAuthorized(record)) {
					continue;
				}

				ResultList.variableBlocks.push_back(record);
			}

			if (QB_.CountOnly) {
				return ReturnCountOnly(ResultList.variableBlocks.size());
			}

			return MakeJSONObjectArray("variableBlocks", ResultList.variableBlocks, *this);
		}

		// Case 2: General / Unfiltered listing or ?entity= / ?venue= filter
		std::vector<ProvObjects::VariableBlock> FinalVariables;
		std::set<std::string> SeenIds;

		if (!AllowedEntities.empty() || !AllowedVenues.empty()) {
			std::string Where;
			if (!entityParam.empty()) {
				if (AllowedEntities.count(entityParam) == 0) {
					// Requested entity is not within caller's allowed entities
					return MakeJSONObjectArray("variableBlocks", FinalVariables, *this);
				}
				Where = "entity='" + ORM::Escape(entityParam) + "'";
			} else if (!venueParam.empty()) {
				bool venueAllowed = (AllowedVenues.count(venueParam) > 0 && DeniedVenues.count(venueParam) == 0);
				if (!venueAllowed) {
					ProvObjects::Venue vRec;
					if (StorageService()->VenueDB().GetRecord("id", venueParam, vRec) && !vRec.entity.empty()) {
						if (AllowedEntities.count(vRec.entity) > 0) {
							venueAllowed = true;
						}
					}
				}
				if (!venueAllowed) {
					return MakeJSONObjectArray("variableBlocks", FinalVariables, *this);
				}
				Where = "venue='" + ORM::Escape(venueParam) + "'";
			} else {
				// No specific scope parameter: gather all variables where caller has permission
				auto makeInClause = [](const std::string &field, const std::set<std::string> &ids) -> std::string {
					if (ids.empty()) return "";
					std::string res = field + " IN (";
					bool first = true;
					for (const auto &id : ids) {
						if (!first) res += ",";
						res += "'" + ORM::Escape(id) + "'";
						first = false;
					}
					res += ")";
					return res;
				};

				std::string entityClause = makeInClause("entity", AllowedEntities);
				std::string venueClause = makeInClause("venue", AllowedVenues);
				std::string rbacClause;
				if (!entityClause.empty() && !venueClause.empty()) {
					rbacClause = "(" + entityClause + " OR " + venueClause + ")";
				} else if (!entityClause.empty()) {
					rbacClause = entityClause;
				} else if (!venueClause.empty()) {
					rbacClause = venueClause;
				}

				if (!rbacClause.empty()) {
					Where = rbacClause;
				}

				if (!DeniedVenues.empty()) {
					std::string deniedClause = "venue NOT IN (";
					bool first = true;
					for (const auto &id : DeniedVenues) {
						if (!first) deniedClause += ",";
						deniedClause += "'" + ORM::Escape(id) + "'";
						first = false;
					}
					deniedClause += ")";
					if (!Where.empty()) {
						Where += " AND ";
					}
					Where += deniedClause;
				}
			}

			auto lambda = [&](const ProvObjects::VariableBlock &varBlock) {
				if (isVariableAuthorized(varBlock)) {
					if (SeenIds.insert(varBlock.info.id).second) {
						FinalVariables.push_back(varBlock);
					}
				}
				return true;
			};
			DB_.Iterate(lambda, Where);
		}

		// Stable deterministic ordering by name, then id
		std::sort(FinalVariables.begin(), FinalVariables.end(),
				  [](const ProvObjects::VariableBlock &a, const ProvObjects::VariableBlock &b) {
					  if (a.info.name != b.info.name) {
						  return a.info.name < b.info.name;
					  }
					  return a.info.id < b.info.id;
				  });

		if (QB_.CountOnly) {
			return ReturnCountOnly(FinalVariables.size());
		}

		std::vector<ProvObjects::VariableBlock> PaginatedVariables;
		if (QB_.Offset < FinalVariables.size()) {
			auto start = FinalVariables.begin() + QB_.Offset;
			auto end = (QB_.Limit > 0 && QB_.Offset + QB_.Limit < FinalVariables.size())
						   ? start + QB_.Limit
						   : FinalVariables.end();
			PaginatedVariables.assign(start, end);
		}

		return MakeJSONObjectArray("variableBlocks", PaginatedVariables, *this);
	}

} // namespace OpenWifi