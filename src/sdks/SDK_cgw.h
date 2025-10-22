/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
#pragma once
#ifdef CGW_INTEGRATION
#include "framework/RESTAPI_Handler.h"

namespace OpenWifi::SDK::CGW {

    /**
     * @brief Create a CGW group with the given ID.
     *
     * Issues a POST to `/api/v1/groups` with JSON body `{"group_id": <groupId>}`.
     * Returns true iff the CGW replies HTTP 200 (OK).
     *
     * @param groupId that will be created in CGW.
     * @return true on success (HTTP 200), false otherwise.
     * @note Requires `uSERVICE_CGW` routing to be configured and reachable.
     */
     bool CreateGroup(uint64_t groupId);

    /**
     * @brief Add a device to a group in CGW.
     *
     * Issues a POST to `/api/v1/groups/{groupId}/infra` with JSON
     * body `{"mac_addrs": [ "<mac>" ]}`. Returns true iff CGW replies HTTP 200.
     *
     * @param groupId Target group ID.
     * @param mac     Device MAC address string as accepted by CGW.
     * @return true on success (HTTP 200), false otherwise.
     * @pre The group should already exist in CGW.
     */
     bool AddDeviceToGroup(uint64_t groupId, const std::string &mac);

    /**
     * @brief Remove a device from a group in CGW.
     *
     * Issues a DELETE to `/api/v1/groups/{groupId}/infra` with JSON
     * body `{"mac_addrs": [ "<mac>" ]}`. Returns true iff CGW replies HTTP 200.
     *
     * @param groupId Target group ID.
     * @param mac     Device MAC address string as accepted by CGW.
     * @return true on success (HTTP 200), false otherwise.
     */
    bool DeleteDeviceFromGroup(uint64_t groupId, const std::string &mac);

    /**
     * @brief Delete a CGW group.
     *
     * Issues a DELETE to `/api/v1/groups?id=<groupId>`. Returns true
     * iff CGW replies HTTP 200.
     *
     * @param groupId Target group to delete.
     * @return true on success (HTTP 200), false otherwise.
     */
    bool DeleteGroup(uint64_t groupId);

}
#endif
