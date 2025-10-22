/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
#pragma once
#ifdef CGW_INTEGRATION
#include "framework/orm.h"

namespace OpenWifi {
    struct GroupsMapRecord {
        std::string venueid;
        uint64_t groupid = 0;
    };

    typedef Poco::Tuple<std::string, uint64_t> GroupsMapDBRecordType;

    class GroupsMapDB : public ORM::DB<GroupsMapDBRecordType, GroupsMapRecord> {
      public:
        GroupsMapDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L);
        /**
         * @brief Create a new mapping entry for the provided venue id.
         *
         * Generates a new group id, persists the mapping and returns the id via
         * the output parameter.
         *
         * @param venueId provisioning venue identifier.
         * @param groupId populated with the newly allocated group id when the call succeeds.
         * @return true on successful persistence, false otherwise.
         */
        bool AddVenue(const std::string &venueId, uint64_t &groupId);
        /**
         * @brief Lookup the CGW group id associated with the given venue.
         *
         * @param venueId provisioning venue identifier.
         * @param groupId mapped Id for CGW.
         * @return true if a mapping exists, false if none is found or an error occurs.
         */
        bool GetGroup(const std::string &venueId, uint64_t &groupId);
        /**
         * @brief Remove the mapping entry for the specified venue.
         *
         * @param venueId provisioning venue identifier.
         * @return true when the entry is deleted, false if the record does not exist or deletion fails.
         */
        bool DeleteVenue(const std::string &venueId);
      private:
        /**
         * @brief Compute the next available group id for a new mapping.
         *
         * Queries the backing store for the current maximum id and returns the next
         * sequential value.
         *
         * @return the next available group id.
         */
        uint64_t NextId();
    };
}
#endif