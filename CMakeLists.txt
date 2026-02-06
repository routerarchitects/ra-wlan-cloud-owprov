cmake_minimum_required(VERSION 3.13)
project(owprov VERSION 4.1.0)

set(CMAKE_CXX_STANDARD 17)

if(UNIX AND APPLE)
    set(OPENSSL_ROOT_DIR /usr/local/opt/openssl)
    set(MYSQL_ROOT_DIR /usr/local/opt/mysql-client)
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)
endif()

if(UNIX AND NOT APPLE)
    set(PostgreSQL_TYPE_INCLUDE_DIR /usr/include/postgresql)
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/build)
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/build BUILD_NUM)
    if(BUILD_INCREMENT)
        MATH(EXPR BUILD_NUM "${BUILD_NUM}+1")
        file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/build ${BUILD_NUM})
    endif()
else()
    set(BUILD_NUM 1)
    file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/build ${BUILD_NUM})
endif()

find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            RESULT_VARIABLE GIT_RESULT
            OUTPUT_VARIABLE GIT_HASH)
    if(NOT GIT_RESULT EQUAL "0")
        message(FATAL_ERROR "git rev-parse --short HEAD failed with ${GIT_RESULT}")
    endif()
    string(REGEX REPLACE "\n$" "" GIT_HASH "${GIT_HASH}")
endif()

add_definitions(-DAWS_CUSTOM_MEMORY_MANAGEMENT -DBOOST_NO_CXX98_FUNCTION_BASE=1)

find_package(OpenSSL    REQUIRED)
find_package(ZLIB       REQUIRED)
find_package(Poco       REQUIRED COMPONENTS Crypto JWT Net Util NetSSL Data DataSQLite)
find_package(nlohmann_json  REQUIRED)
find_package(fmt        REQUIRED)
# find_package(valijson   REQUIRED)

if(SMALL_BUILD)
    find_package(Poco REQUIRED COMPONENTS Crypto JWT Net Util NetSSL Data DataSQLite)
else()
    find_package(CppKafka REQUIRED)
    find_package(PostgreSQL REQUIRED)
    find_package(MySQL REQUIRED)
    find_package(Poco REQUIRED COMPONENTS JSON Crypto JWT Net Util NetSSL Data DataSQLite DataPostgreSQL DataMySQL)
endif()

include_directories(/usr/local/include  /usr/local/opt/openssl/include src include/kafka /usr/local/opt/mysql-client/include)

configure_file(src/ow_version.h.in ${PROJECT_SOURCE_DIR}/src/ow_version.h @ONLY)

add_definitions(-DPOCO_LOG_DEBUG="1")

add_compile_options(-Wall -Wextra)
if(ASAN)
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
endif()

add_executable(owprov
        build
        src/ow_version.h.in
        src/framework/CountryCodes.h
        src/framework/KafkaTopics.h
        src/framework/MicroService.h
        src/framework/OpenWifiTypes.h
        src/framework/orm.h
        src/framework/StorageClass.h
        src/framework/MicroServiceErrorHandler.h
        src/framework/UI_WebSocketClientServer.cpp
        src/framework/UI_WebSocketClientServer.h
        src/framework/utils.h
        src/framework/utils.cpp
        src/framework/AppServiceRegistry.h
        src/framework/SubSystemServer.cpp
        src/framework/SubSystemServer.h
        src/framework/RESTAPI_utils.h
        src/framework/UI_WebSocketClientNotifications.cpp
        src/framework/AuthClient.cpp
        src/framework/AuthClient.h
        src/framework/MicroServiceNames.h
        src/framework/MicroServiceFuncs.h
        src/framework/OpenAPIRequests.cpp
        src/framework/OpenAPIRequests.h
        src/framework/MicroServiceFuncs.cpp
        src/framework/ALBserver.cpp
        src/framework/ALBserver.h
        src/framework/KafkaManager.cpp
        src/framework/KafkaManager.h
        src/framework/RESTAPI_RateLimiter.h
        src/framework/WebSocketLogger.h
        src/framework/RESTAPI_GenericServerAccounting.h
        src/framework/CIDR.h
        src/framework/RESTAPI_Handler.cpp
        src/framework/RESTAPI_Handler.h
        src/framework/RESTAPI_ExtServer.h
        src/framework/RESTAPI_ExtServer.cpp
        src/framework/RESTAPI_IntServer.cpp
        src/framework/RESTAPI_IntServer.h
        src/framework/RESTAPI_SystemCommand.h
        src/framework/RESTAPI_WebSocketServer.h
        src/framework/RESTAPI_SystemConfiguration.h
        src/framework/EventBusManager.cpp
        src/framework/EventBusManager.h
        src/framework/RESTAPI_PartHandler.h
        src/framework/MicroService.cpp
        src/framework/MicroServiceExtra.h
        src/framework/ConfigurationValidator.cpp
        src/framework/ConfigurationValidator.h
        src/framework/default_device_types.h
        src/UI_Prov_WebSocketNotifications.h
        src/UI_Prov_WebSocketNotifications.cpp
        src/RESTObjects/RESTAPI_SecurityObjects.h src/RESTObjects/RESTAPI_SecurityObjects.cpp
        src/RESTObjects/RESTAPI_GWobjects.h src/RESTObjects/RESTAPI_GWobjects.cpp
        src/RESTObjects/RESTAPI_FMSObjects.h src/RESTObjects/RESTAPI_FMSObjects.cpp
        src/RESTObjects/RESTAPI_CertObjects.cpp src/RESTObjects/RESTAPI_CertObjects.h
        src/RESTObjects/RESTAPI_OWLSobjects.cpp src/RESTObjects/RESTAPI_OWLSobjects.h
        src/RESTObjects/RESTAPI_ProvObjects.cpp src/RESTObjects/RESTAPI_ProvObjects.h
        src/RESTObjects/RESTAPI_AnalyticsObjects.cpp src/RESTObjects/RESTAPI_AnalyticsObjects.h
        src/RESTObjects/RESTAPI_SubObjects.cpp src/RESTObjects/RESTAPI_SubObjects.h

        src/RESTAPI/RESTAPI_routers.cpp
        src/Daemon.cpp src/Daemon.h
        src/Dashboard.h src/Dashboard.cpp
        src/StorageService.cpp src/StorageService.h

        src/storage/storage_entity.cpp src/storage/storage_entity.h
        src/storage/storage_policies.cpp src/storage/storage_policies.h
        src/storage/storage_venue.cpp src/storage/storage_venue.h
        src/storage/storage_location.cpp src/storage/storage_location.h
        src/storage/storage_contact.cpp src/storage/storage_contact.h
        src/storage/storage_inventory.cpp src/storage/storage_inventory.h
        src/storage/storage_management_roles.cpp src/storage/storage_management_roles.h
        src/storage/storage_configurations.cpp src/storage/storage_configurations.h
        src/storage/storage_tags.cpp src/storage/storage_tags.h
        src/storage/storage_operataor.cpp src/storage/storage_operataor.h
        src/storage/storage_sub_devices.cpp src/storage/storage_sub_devices.h
        src/storage/storage_service_class.cpp src/storage/storage_service_class.h
        src/storage/storage_maps.cpp src/storage/storage_maps.h
        src/storage/storage_signup.cpp src/storage/storage_signup.h
        src/storage/storage_variables.cpp src/storage/storage_variables.h
        src/storage/storage_overrides.cpp src/storage/storage_overrides.h

        src/RESTAPI/RESTAPI_entity_handler.cpp src/RESTAPI/RESTAPI_entity_handler.h
        src/RESTAPI/RESTAPI_contact_handler.cpp src/RESTAPI/RESTAPI_contact_handler.h
        src/RESTAPI/RESTAPI_location_handler.cpp src/RESTAPI/RESTAPI_location_handler.h
        src/RESTAPI/RESTAPI_venue_handler.cpp src/RESTAPI/RESTAPI_venue_handler.h
        src/RESTAPI/RESTAPI_inventory_handler.cpp src/RESTAPI/RESTAPI_inventory_handler.h
        src/RESTAPI/RESTAPI_managementPolicy_handler.cpp src/RESTAPI/RESTAPI_managementPolicy_handler.h
        src/RESTAPI/RESTAPI_inventory_list_handler.cpp src/RESTAPI/RESTAPI_inventory_list_handler.h
        src/RESTAPI/RESTAPI_entity_list_handler.cpp src/RESTAPI/RESTAPI_entity_list_handler.h
        src/RESTAPI/RESTAPI_configurations_handler.cpp src/RESTAPI/RESTAPI_configurations_handler.h
        src/RESTAPI/RESTAPI_contact_list_handler.cpp src/RESTAPI/RESTAPI_contact_list_handler.h
        src/RESTAPI/RESTAPI_location_list_handler.cpp src/RESTAPI/RESTAPI_location_list_handler.h
        src/RESTAPI/RESTAPI_venue_list_handler.cpp src/RESTAPI/RESTAPI_venue_list_handler.h
        src/RESTAPI/RESTAPI_managementPolicy_list_handler.cpp src/RESTAPI/RESTAPI_managementPolicy_list_handler.h
        src/RESTAPI/RESTAPI_managementRole_handler.cpp src/RESTAPI/RESTAPI_managementRole_handler.h
        src/RESTAPI/RESTAPI_managementRole_list_handler.cpp src/RESTAPI/RESTAPI_managementRole_list_handler.h
        src/RESTAPI/RESTAPI_configurations_list_handler.cpp src/RESTAPI/RESTAPI_configurations_list_handler.h
        src/RESTAPI/RESTAPI_iptocountry_handler.cpp src/RESTAPI/RESTAPI_iptocountry_handler.h
        src/RESTAPI/RESTAPI_signup_handler.h src/RESTAPI/RESTAPI_signup_handler.cpp
        src/RESTAPI/RESTAPI_asset_server.cpp src/RESTAPI/RESTAPI_asset_server.h
        src/RESTAPI/RESTAPI_db_helpers.h
        src/RESTAPI/RESTAPI_map_handler.cpp src/RESTAPI/RESTAPI_map_handler.h
        src/RESTAPI/RESTAPI_map_list_handler.cpp src/RESTAPI/RESTAPI_map_list_handler.h
        src/RESTAPI/RESTAPI_variables_handler.cpp src/RESTAPI/RESTAPI_variables_handler.h
        src/RESTAPI/RESTAPI_variables_list_handler.cpp src/RESTAPI/RESTAPI_variables_list_handler.h
        src/RESTAPI/RESTAPI_overrides_handler.cpp src/RESTAPI/RESTAPI_overrides_handler.h

        src/FindCountry.h
        src/sdks/SDK_gw.cpp src/sdks/SDK_gw.h
        src/sdks/SDK_analytics.cpp src/sdks/SDK_analytics.h
        src/sdks/SDK_prov.cpp src/sdks/SDK_prov.h
        src/sdks/SDK_sec.cpp src/sdks/SDK_sec.h
        src/SerialNumberCache.h src/SerialNumberCache.cpp
        src/APConfig.cpp src/APConfig.h
        src/AutoDiscovery.cpp src/AutoDiscovery.h
        src/ConfigSanityChecker.cpp src/ConfigSanityChecker.h
        src/TagServer.cpp src/TagServer.h
        src/JobController.cpp src/JobController.h
        src/JobRegistrations.cpp
        src/Signup.cpp src/Signup.h
        src/DeviceTypeCache.h
        src/FileDownloader.cpp src/FileDownloader.h
        src/Tasks/VenueConfigUpdater.h
        src/libs/croncpp.h
        src/Kafka_ProvUpdater.cpp src/Kafka_ProvUpdater.h
        src/RESTAPI/RESTAPI_sub_devices_list_handler.cpp src/RESTAPI/RESTAPI_sub_devices_list_handler.h
        src/RESTAPI/RESTAPI_sub_devices_handler.cpp src/RESTAPI/RESTAPI_sub_devices_handler.h
        src/RESTAPI/RESTAPI_subscriber_venue_handler.cpp src/RESTAPI/RESTAPI_subscriber_venue_handler.h
        src/RESTAPI/RESTAPI_service_class_list_handler.cpp src/RESTAPI/RESTAPI_service_class_list_handler.h
        src/RESTAPI/RESTAPI_service_class_handler.cpp src/RESTAPI/RESTAPI_service_class_handler.h
        src/RESTAPI/RESTAPI_operators_list_handler.cpp src/RESTAPI/RESTAPI_operators_list_handler.h
        src/RESTAPI/RESTAPI_operators_handler.cpp src/RESTAPI/RESTAPI_operators_handler.h
        src/storage/storage_op_contacts.cpp src/storage/storage_op_contacts.h
        src/storage/storage_op_locations.cpp src/storage/storage_op_locations.h
        src/RESTAPI/RESTAPI_op_contact_list_handler.cpp src/RESTAPI/RESTAPI_op_contact_list_handler.h
        src/RESTAPI/RESTAPI_op_contact_handler.cpp src/RESTAPI/RESTAPI_op_contact_handler.h
        src/RESTAPI/RESTAPI_op_location_list_handler.cpp src/RESTAPI/RESTAPI_op_location_list_handler.h
        src/RESTAPI/RESTAPI_op_location_handler.cpp src/RESTAPI/RESTAPI_op_location_handler.h
        src/ProvWebSocketClient.cpp src/ProvWebSocketClient.h
        src/Tasks/VenueRebooter.h src/Tasks/VenueUpgrade.h
        src/sdks/SDK_fms.cpp src/sdks/SDK_fms.h
        src/RESTAPI/RESTAPI_overrides_handler.cpp src/RESTAPI/RESTAPI_overrides_handler.h
        src/storage/storage_glblraccounts.cpp src/storage/storage_glblraccounts.h
        src/storage/storage_glblrcerts.cpp src/storage/storage_glblrcerts.h
        src/RESTAPI/RESTAPI_openroaming_gr_list_acct_handler.cpp src/RESTAPI/RESTAPI_openroaming_gr_list_acct_handler.h
        src/RESTAPI/RESTAPI_openroaming_gr_acct_handler.cpp src/RESTAPI/RESTAPI_openroaming_gr_acct_handler.h
        src/RESTAPI/RESTAPI_openroaming_gr_list_certificates.cpp src/RESTAPI/RESTAPI_openroaming_gr_list_certificates.h
        src/RESTAPI/RESTAPI_openroaming_gr_cert_handler.cpp src/RESTAPI/RESTAPI_openroaming_gr_cert_handler.h
        src/RESTAPI/RESTAPI_openroaming_orion_list_acct_handler.cpp src/RESTAPI/RESTAPI_openroaming_orion_list_acct_handler.h
        src/RESTAPI/RESTAPI_openroaming_orion_acct_handler.cpp src/RESTAPI/RESTAPI_openroaming_orion_acct_handler.h
        src/storage/storage_orion_accounts.cpp src/storage/storage_orion_accounts.h
        src/storage/storage_radius_endpoints.cpp
        src/storage/storage_radius_endpoints.h
        src/RESTAPI/RESTAPI_radiusendpoint_list_handler.cpp
        src/RESTAPI/RESTAPI_radiusendpoint_list_handler.h
        src/RESTAPI/RESTAPI_radius_endpoint_handler.cpp
        src/RESTAPI/RESTAPI_radius_endpoint_handler.h
        src/RadiusEndpointTypes/GlobalReach.cpp src/RadiusEndpointTypes/GlobalReach.h
        src/RadiusEndpointTypes/OrionWifi.h
        src/RadiusEndpointUpdater.cpp
        src/RadiusEndpointUpdater.h
        src/RadiusEndpointTypes/Radsec.cpp
        src/RadiusEndpointTypes/Radsec.h
        src/RadiusEndpointTypes/GenericRadius.cpp
        src/RadiusEndpointTypes/GenericRadius.h
)

target_link_libraries(owprov PUBLIC
        ${Poco_LIBRARIES}
        ${MySQL_LIBRARIES}
        ${ZLIB_LIBRARIES}
        CppKafka::cppkafka
        resolv
        fmt::fmt)
