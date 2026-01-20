//
// Created by stephane bourque on 2021-10-23.
//

#include "RESTAPI/RESTAPI_asset_server.h"
#include "RESTAPI/RESTAPI_configurations_handler.h"
#include "RESTAPI/RESTAPI_configurations_list_handler.h"
#include "RESTAPI/RESTAPI_contact_handler.h"
#include "RESTAPI/RESTAPI_contact_list_handler.h"
#include "RESTAPI/RESTAPI_entity_handler.h"
#include "RESTAPI/RESTAPI_entity_list_handler.h"
#include "RESTAPI/RESTAPI_inventory_handler.h"
#include "RESTAPI/RESTAPI_inventory_list_handler.h"
#include "RESTAPI/RESTAPI_iptocountry_handler.h"
#include "RESTAPI/RESTAPI_location_handler.h"
#include "RESTAPI/RESTAPI_location_list_handler.h"
#include "RESTAPI/RESTAPI_managementPolicy_handler.h"
#include "RESTAPI/RESTAPI_managementPolicy_list_handler.h"
#include "RESTAPI/RESTAPI_managementRole_list_handler.h"
#include "RESTAPI/RESTAPI_map_handler.h"
#include "RESTAPI/RESTAPI_map_list_handler.h"
#include "RESTAPI/RESTAPI_op_contact_handler.h"
#include "RESTAPI/RESTAPI_op_contact_list_handler.h"
#include "RESTAPI/RESTAPI_op_location_handler.h"
#include "RESTAPI/RESTAPI_op_location_list_handler.h"
#include "RESTAPI/RESTAPI_operators_handler.h"
#include "RESTAPI/RESTAPI_operators_list_handler.h"
#include "RESTAPI/RESTAPI_overrides_handler.h"
#include "RESTAPI/RESTAPI_service_class_handler.h"
#include "RESTAPI/RESTAPI_service_class_list_handler.h"
#include "RESTAPI/RESTAPI_signup_handler.h"
#include "RESTAPI/RESTAPI_sub_devices_handler.h"
#include "RESTAPI/RESTAPI_sub_devices_list_handler.h"
#include "RESTAPI/RESTAPI_subscriber_provision_handler.h"
#include "RESTAPI/RESTAPI_variables_handler.h"
#include "RESTAPI/RESTAPI_variables_list_handler.h"
#include "RESTAPI/RESTAPI_venue_handler.h"
#include "RESTAPI/RESTAPI_venue_list_handler.h"
#include "RESTAPI/RESTAPI_openroaming_gr_acct_handler.h"
#include "RESTAPI/RESTAPI_openroaming_gr_list_acct_handler.h"
#include "RESTAPI/RESTAPI_openroaming_gr_cert_handler.h"
#include "RESTAPI/RESTAPI_openroaming_gr_list_certificates.h"
#include "RESTAPI/RESTAPI_openroaming_orion_acct_handler.h"
#include "RESTAPI/RESTAPI_openroaming_orion_list_acct_handler.h"
#include "RESTAPI/RESTAPI_radiusendpoint_list_handler.h"
#include "RESTAPI/RESTAPI_radius_endpoint_handler.h"

#include "framework/RESTAPI_SystemCommand.h"
#include "framework/RESTAPI_WebSocketServer.h"
#include "framework/RESTAPI_SystemConfiguration.h"

namespace OpenWifi {

	Poco::Net::HTTPRequestHandler *
	RESTAPI_ExtRouter(const std::string &Path, RESTAPIHandler::BindingMap &Bindings,
					  Poco::Logger &L, RESTAPI_GenericServerAccounting &S, uint64_t TransactionId) {
		return RESTAPI_Router<
			RESTAPI_system_command, RESTAPI_system_configuration,
            RESTAPI_entity_handler, RESTAPI_entity_list_handler,
			RESTAPI_contact_handler, RESTAPI_contact_list_handler, RESTAPI_location_handler,
			RESTAPI_location_list_handler, RESTAPI_venue_handler, RESTAPI_venue_list_handler,
			RESTAPI_inventory_handler, RESTAPI_inventory_list_handler,
			RESTAPI_managementPolicy_handler, RESTAPI_managementPolicy_list_handler,
			RESTAPI_managementRole_list_handler, RESTAPI_configurations_handler,
			RESTAPI_configurations_list_handler, RESTAPI_map_handler, RESTAPI_map_list_handler,
			RESTAPI_webSocketServer, RESTAPI_iptocountry_handler, RESTAPI_signup_handler,
			RESTAPI_variables_handler, RESTAPI_variables_list_handler, RESTAPI_sub_devices_handler,
			RESTAPI_sub_devices_list_handler, RESTAPI_subscriber_provision_handler,
			RESTAPI_operators_handler,
			RESTAPI_operators_list_handler, RESTAPI_service_class_handler,
			RESTAPI_service_class_list_handler, RESTAPI_op_contact_handler,
			RESTAPI_op_contact_list_handler, RESTAPI_op_location_handler,
			RESTAPI_op_location_list_handler, RESTAPI_asset_server, RESTAPI_overrides_handler,
            RESTAPI_openroaming_gr_acct_handler, RESTAPI_openroaming_gr_list_acct_handler,
            RESTAPI_openroaming_gr_cert_handler, RESTAPI_openroaming_gr_list_certificates,
            RESTAPI_openroaming_orion_acct_handler, RESTAPI_openroaming_orion_list_acct_handler,
            RESTAPI_radiusendpoint_list_handler, RESTAPI_radius_endpoint_handler>(
			Path, Bindings, L, S, TransactionId);
	}

	Poco::Net::HTTPRequestHandler *
	RESTAPI_IntRouter(const std::string &Path, RESTAPIHandler::BindingMap &Bindings,
					  Poco::Logger &L, RESTAPI_GenericServerAccounting &S, uint64_t TransactionId) {
		return RESTAPI_Router_I<
			RESTAPI_system_command, RESTAPI_system_configuration, RESTAPI_entity_handler,
            RESTAPI_entity_list_handler,
			RESTAPI_contact_handler, RESTAPI_contact_list_handler, RESTAPI_location_handler,
			RESTAPI_location_list_handler, RESTAPI_venue_handler, RESTAPI_venue_list_handler,
			RESTAPI_inventory_handler, RESTAPI_inventory_list_handler,
			RESTAPI_managementPolicy_handler, RESTAPI_managementPolicy_list_handler,
			RESTAPI_managementRole_list_handler, RESTAPI_configurations_handler,
			RESTAPI_configurations_list_handler, RESTAPI_map_handler, RESTAPI_map_list_handler,
			RESTAPI_webSocketServer, RESTAPI_iptocountry_handler, RESTAPI_signup_handler,
			RESTAPI_variables_handler, RESTAPI_variables_list_handler, RESTAPI_sub_devices_handler,
			RESTAPI_sub_devices_list_handler, RESTAPI_subscriber_provision_handler,
			RESTAPI_operators_handler,
			RESTAPI_operators_list_handler, RESTAPI_service_class_handler,
			RESTAPI_service_class_list_handler, RESTAPI_op_contact_handler,
			RESTAPI_op_contact_list_handler, RESTAPI_op_location_handler,
			RESTAPI_op_location_list_handler, RESTAPI_overrides_handler,
            RESTAPI_openroaming_gr_acct_handler, RESTAPI_openroaming_gr_list_acct_handler,
            RESTAPI_openroaming_gr_cert_handler, RESTAPI_openroaming_gr_list_certificates,
            RESTAPI_openroaming_orion_acct_handler, RESTAPI_openroaming_orion_list_acct_handler,
            RESTAPI_radiusendpoint_list_handler, RESTAPI_radius_endpoint_handler>(
                    Path, Bindings, L, S,TransactionId);
	}
} // namespace OpenWifi
