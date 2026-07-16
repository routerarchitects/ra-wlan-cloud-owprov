//
// Created by stephane bourque on 2023-09-27.
//

#include "RESTAPI_radius_endpoint_handler.h"
#include <storage/storage_orion_accounts.h>
#include <RESTObjects/RESTAPI_GWobjects.h>

namespace OpenWifi {

    void RESTAPI_radius_endpoint_handler::DoGet() {
        auto id = GetBinding("id");
        if(id.empty()) {
            return BadRequest(RESTAPI::Errors::MissingAuthenticationInformation);
        }

        RecordType Record;
        if(DB_.GetRecord("id",id,Record)) {
            return ReturnObject(Record);
        }

        return NotFound();
    }

    void RESTAPI_radius_endpoint_handler::DoDelete() {
        auto id = GetBinding("id");
        if(id.empty()) {
            return BadRequest(RESTAPI::Errors::MissingAuthenticationInformation);
        }
        RecordType Record;
        if(DB_.GetRecord("id",id,Record)) {
            DB_.DeleteRecord("id",id);
            ProvObjects::RADIUSEndpointUpdateStatus Status;
            Status.ChangeConfiguration();
            return OK();
        }
        return NotFound();
    }

    static bool ValidPort(std::uint32_t P) {
        return P>0 && P<65535;
    }

    static bool ValidRadiusServer(const ProvObjects::RADIUSServer &S) {
        if(S.Hostname.empty() || !ValidPort(S.Port) || !Utils::ValidIP(S.IP) || S.Secret.empty()) {
            return false;
        }
        return true;
    }

    static bool ValidRadiusServer(const std::vector<ProvObjects::RADIUSServer> &ServerList) {
        return std::all_of(ServerList.begin(),ServerList.end(),[](const ProvObjects::RADIUSServer &Server)->bool { return ValidRadiusServer(Server); });
    }

    void RESTAPI_radius_endpoint_handler::DoPost() {
        auto id = GetBinding("id");
        if(id.empty()) {
            return BadRequest(RESTAPI::Errors::MissingAuthenticationInformation);
        }

        const auto &RawObject = ParsedBody_;
        RecordType     NewRecord;
        if(!NewRecord.from_json(RawObject)) {
            return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
        }

        if(GWObjects::RadiusEndpointType(NewRecord.Type)==GWObjects::RadiusEndpointType::unknown) {
            return BadRequest(RESTAPI::Errors::InvalidRadiusTypeEndpoint);
        }
        if(GWObjects::RadiusPoolStrategy(NewRecord.PoolStrategy)==GWObjects::RadiusPoolStrategy::unknown) {
            return BadRequest(RESTAPI::Errors::InvalidRadiusEndpointPoolStrategy);
        }
        if(!NewRecord.RadiusServers.empty() && !NewRecord.RadsecServers.empty()) {
            return BadRequest(RESTAPI::Errors::EndpointMustHaveOneTypeOfServers);
        }

        auto EndPointType = GWObjects::RadiusEndpointType(NewRecord.Type);
        switch(EndPointType) {
            case GWObjects::RadiusEndpointType::radsec:
            case GWObjects::RadiusEndpointType::orion:
            case GWObjects::RadiusEndpointType::globalreach:
            {
                if(NewRecord.RadsecServers.empty()) {
                    return BadRequest(RESTAPI::Errors::EndpointMustHaveOneTypeOfServers);
                }
            } break;
            case GWObjects::RadiusEndpointType::generic: {
                if(NewRecord.RadiusServers.empty()) {
                    return BadRequest(RESTAPI::Errors::EndpointMustHaveOneTypeOfServers);
                }
            } break;
            default:
                return BadRequest(RESTAPI::Errors::EndpointMustHaveOneTypeOfServers);
        }

        if(NewRecord.Index.empty() || !RadiusEndpointDB::ValidIndex(NewRecord.Index)) {
            return BadRequest(RESTAPI::Errors::RadiusEndpointIndexInvalid);
        }

        //  Make sure that nobody is using that index
        auto where = fmt::format(" radiusindex='{}' ", NewRecord.Index);
        if(DB_.Count(where)!=0) {
            return BadRequest(RESTAPI::Errors::RadiusEndpointIndexInvalid);
        }

        if(EndPointType==GWObjects::RadiusEndpointType::generic) {
            for(const auto &Server:NewRecord.RadiusServers) {
                if(!ValidRadiusServer(Server.Authentication) ||
                !ValidRadiusServer(Server.Accounting) ||
                !ValidRadiusServer(Server.CoA)) {
                    return BadRequest(RESTAPI::Errors::InvalidRadiusServer);
                }
            }
        } else {
            switch(EndPointType) {
                case GWObjects::RadiusEndpointType::orion: {
                    for(const auto &Server:NewRecord.RadsecServers) {
                        if(!StorageService()->OrionAccountsDB().Exists("id",Server.UseOpenRoamingAccount)) {
                            return BadRequest(RESTAPI::Errors::OrionAccountMustExist);
                        }
                    }
                } break;
                case GWObjects::RadiusEndpointType::globalreach: {
                    for(const auto &Server:NewRecord.RadsecServers) {
                        if(!StorageService()->GLBLRCertsDB().Exists("id",Server.UseOpenRoamingAccount)) {
                            return BadRequest(RESTAPI::Errors::GlobalReachCertMustExist);
                        }
                    }
                } break;
                case GWObjects::RadiusEndpointType::radsec: {
                    for(const auto &Server:NewRecord.RadsecServers) {
                        if(Server.Certificate.empty() || !Utils::ValidX509Certificate(Server.Certificate)) {
                            return BadRequest(RESTAPI::Errors::InvalidRadsecMainCertificate);
                        }
                        if(Server.CaCerts.empty() || !Utils::ValidX509Certificate(Server.CaCerts)) {
                            return BadRequest(RESTAPI::Errors::InvalidRadsecCaCertificate);
                        }
                        if(Server.PrivateKey.empty() || !Utils::VerifyPrivateKey(Server.PrivateKey)) {
                            return BadRequest(RESTAPI::Errors::InvalidRadsecPrivteKey);
                        }
                        if(!Utils::ValidIP(Server.IP)) {
                            return BadRequest(RESTAPI::Errors::InvalidRadsecIPAddress);
                        }
                        if(!(Server.Port>0 && Server.Port<65535)) {
                            return BadRequest(RESTAPI::Errors::InvalidRadsecPort);
                        }
                        if(Server.Secret.empty()) {
                            return BadRequest(RESTAPI::Errors::InvalidRadsecSecret);
                        }
                    }

                } break;
                default: {

                }
            }
        }

        ProvObjects::CreateObjectInfo(RawObject,UserInfo_.userinfo,NewRecord.info);
        if(DB_.CreateRecord(NewRecord)) {
            RecordType  AddedRecord;
            DB_.GetRecord("id", NewRecord.info.id, AddedRecord);
            ProvObjects::RADIUSEndpointUpdateStatus Status;
            Status.ChangeConfiguration();
            return ReturnObject(AddedRecord);
        }
        return BadRequest(RESTAPI::Errors::RecordNotCreated);
    }

    void RESTAPI_radius_endpoint_handler::DoPut() {
        auto id = GetBinding("id");
        if(id.empty()) {
            return BadRequest(RESTAPI::Errors::MissingAuthenticationInformation);
        }

        const auto &RawObject = ParsedBody_;
        RecordType     ModifiedRecord;
        if(!ModifiedRecord.from_json(RawObject)) {
            return BadRequest(RESTAPI::Errors::InvalidJSONDocument);
        }

        RecordType     Existing;
        if(!DB_.GetRecord("id",id,Existing)) {
            return NotFound();
        }

        AssignIfPresent(RawObject,"NasIdentifier", Existing.NasIdentifier);
        AssignIfPresent(RawObject,"AccountingInterval", Existing.AccountingInterval);

        ProvObjects::UpdateObjectInfo(RawObject, UserInfo_.userinfo, Existing.info);
        if(DB_.UpdateRecord("id", Existing.info.id, Existing)) {
            RecordType  AddedRecord;
            DB_.GetRecord("id", Existing.info.id, AddedRecord);
            ProvObjects::RADIUSEndpointUpdateStatus Status;
            Status.ChangeConfiguration();
            return ReturnObject(AddedRecord);
        }

        return BadRequest(RESTAPI::Errors::NotImplemented);
    }

} // OpenWifi
