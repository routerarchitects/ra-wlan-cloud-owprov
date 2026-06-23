//
// Created by stephane bourque on 2022-04-06.
//

#include "RESTAPI_operators_list_handler.h"
#include "RESTAPI/RESTAPI_db_helpers.h"
#include "RESTAPI/RESTAPI_rbac_helpers.h"
#include "RESTAPI/RESTAPI_list_helpers.h"

namespace OpenWifi {
	namespace {
		bool ResolveOwnerOperator(RESTAPIHandler &handler, std::string &ownerOperatorId,
								  std::string &ownerEntityId) {
			ownerOperatorId.clear();
			ownerEntityId.clear();

			ProvObjects::Operator ownerOperator;
			if (!RBAC::ResolveUserOwnerOperator(handler.UserInfo_.userinfo, ownerOperator)) {
				return false;
			}

			ownerOperatorId = ownerOperator.info.id;
			ownerEntityId = ownerOperator.entityId;
			return true;
		}

		bool IsDirectChildOperator(const ProvObjects::Operator &op,
								   const std::string &ownerOperatorId,
								   const std::string &ownerEntityId) {
			if (!op.parentOperatorId.empty()) {
				return op.parentOperatorId == ownerOperatorId;
			}

			if (op.entityId.empty()) {
				return false;
			}

			ProvObjects::Entity entity;
			if (!StorageService()->EntityDB().GetRecord("id", op.entityId, entity)) {
				return false;
			}

			return entity.parent == ownerEntityId;
		}
	} // namespace

	void RESTAPI_operators_list_handler::DoGet() {
		if (!QB_.Select.empty()) {
			if (RBAC::IsRootUser(*this)) {
				return ReturnRecordList<decltype(DB_), ProvObjects::Operator>("operators", DB_,
																			  *this);
			}

			std::vector<ProvObjects::Operator> Filtered;
			std::string ownerOperatorId;
			std::string ownerEntityId;
			const bool directChildrenOnly = ResolveOwnerOperator(*this, ownerOperatorId,
																 ownerEntityId);
			for (const auto &id : SelectedRecords()) {
				ProvObjects::Operator Existing;
				if (DB_.GetRecord("id", id, Existing) &&
					(!directChildrenOnly ||
					 Existing.info.id == ownerOperatorId ||
					 IsDirectChildOperator(Existing, ownerOperatorId, ownerEntityId)) &&
					RBAC::HasAccess(*this, "operator", "LIST",
									RBAC::TargetScope{Existing.entityId, ""})) {
					Filtered.push_back(Existing);
				}
			}
			if (QB_.CountOnly) {
				return ReturnCountOnly(Filtered.size());
			}
			return MakeJSONObjectArray("operators", Filtered, *this);
		}

		std::vector<ProvObjects::Operator> Entries;
		auto total = DB_.Count();
		if (total > 0) {
			DB_.GetRecords(0, total, Entries);
		}

		if (!RBAC::IsRootUser(*this)) {
			// RBAC filtering must happen before CountOnly and pagination, so fetch the full candidate set first.
			std::string ownerOperatorId;
			std::string ownerEntityId;
			const bool directChildrenOnly = ResolveOwnerOperator(*this, ownerOperatorId,
																 ownerEntityId);
			Entries = RESTAPI::FilterRecords(
				Entries,
				[&](const auto &Entry) {
					if (directChildrenOnly &&
						Entry.info.id != ownerOperatorId &&
						!IsDirectChildOperator(Entry, ownerOperatorId, ownerEntityId)) {
						return false;
					}
					return RBAC::HasAccess(*this, "operator", "LIST",
										   RBAC::TargetScope{Entry.entityId, ""});
				});
		}

		if (QB_.CountOnly) {
			return ReturnCountOnly(Entries.size());
		}

		auto page = RESTAPI::ApplyPagination(Entries, QB_.Offset, QB_.Limit);
		return MakeJSONObjectArray("operators", page, *this);
	}
} // namespace OpenWifi
