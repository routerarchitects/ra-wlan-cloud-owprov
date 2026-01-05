/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
#ifdef CGW_INTEGRATION
#include "SDK_cgw.h"
#include "framework/MicroServiceNames.h"
#include "framework/OpenAPIRequests.h"
#include "fmt/format.h"
#include "Poco/Logger.h"

namespace OpenWifi::SDK::CGW {
 bool CreateGroup(uint64_t groupId) {
        auto &logger = Poco::Logger::get("SDK_cgw");
        poco_information(logger, fmt::format("Creating Group:{}", groupId));
        Poco::JSON::Object Body;
        Body.set("group_id", groupId);
        OpenAPIRequestPost R(uSERVICE_CGW, "/api/v1/groups", {}, Body, 10000);
        Poco::JSON::Object::Ptr Response;
        auto Status = R.Do(Response);
        bool Ok = Status == Poco::Net::HTTPResponse::HTTP_OK;
        return Ok;
    }

    bool AddDeviceToGroup(uint64_t groupId, const std::string &mac) {
        auto &logger = Poco::Logger::get("SDK_cgw");
        poco_information(logger, fmt::format("Adding Device:{} to Group:{}",mac ,groupId));
        Poco::JSON::Object Body;
        Poco::JSON::Array Macs;
        Macs.add(mac);
        Body.set("mac_addrs", Macs);
        std::string EP = fmt::format("/api/v1/groups/{}/infra", groupId);
        OpenAPIRequestPost R(uSERVICE_CGW, EP, {}, Body, 10000);
        Poco::JSON::Object::Ptr Response;
        auto Status = R.Do(Response);
        bool Ok = Status == Poco::Net::HTTPResponse::HTTP_OK;
        return Ok;
    }
    
    bool DeleteDeviceFromGroup(uint64_t groupId, const std::string &mac) {
        auto &logger = Poco::Logger::get("SDK_cgw");
        poco_information(logger, fmt::format("Deleting Device:{} from Group:{}",mac ,groupId));
        Poco::JSON::Object Body;
        Poco::JSON::Array Macs;
        Macs.add(mac);
        Body.set("mac_addrs", Macs);
        std::string EP = fmt::format("/api/v1/groups/{}/infra", groupId);
        OpenAPIRequestDelete R(uSERVICE_CGW, EP, {}, 10000,"", Body);
        auto Status = R.Do();
        bool Ok = Status == Poco::Net::HTTPResponse::HTTP_OK;
        return Ok;
    }
    bool DeleteGroup(uint64_t groupId) {
        auto &logger = Poco::Logger::get("SDK_cgw");
        poco_information(logger, fmt::format("Deleting Group:{}", groupId));
        OpenAPIRequestDelete R(uSERVICE_CGW, "/api/v1/groups", {{"id", std::to_string(groupId)}}, 10000);
        Poco::JSON::Object::Ptr Response;
        auto Status = R.Do();
        bool Ok = Status == Poco::Net::HTTPResponse::HTTP_OK;
        return Ok;
    }
}
#endif
