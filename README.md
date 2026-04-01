<p align="center">
    <img src="images/project/logo.svg" width="200"/>
</p>

# OpenWiFi Provisioning Service (OWPROV)
## What is it?
The OWPROV is a service for the TIP OpenWiFi CloudSDK (OWSDK).
OWPROV manages groups of access points through the use of entities and vanues. OWPROV, like all other OWSDK microservices, is
defined using an OpenAPI definition and uses the ucentral communication protocol to interact with Access Points. To use
the OWPROV, you either need to [build it](#building) or use the [Docker version](#docker).

## OpenAPI
The OWPROV-REST-API is defined in [openapi/owprov.yaml](https://raw.githubusercontent.com/routerarchitects/ra-wlan-cloud-owprov/refs/heads/main/openapi/owprov.yaml) . You can use this OpenAPI definition to generate static API documentation, inspect the available endpoints, or build client SDKs.


## Building
To build the microservice from source, please follow the instructions in [here](./BUILDING.md)

## Docker
To use the CLoudSDK deployment please follow [here](https://github.com/routerarchitects/mango-cloud-deployment)

## Root entity
It's UUID value is 0000-0000-0000. Its parent entity must be empty.

## Entity
### Creation rules
- You must set the parent of an entity.
- The only properties you may set at creation are:
  - name
  - description
  - notes
  - parent

### Modification rules
You may modify the following fields in the POST
- name
- description
- notes

### Delete
- Children must be empty

## Inventory Tags
### Creation rules
- Entity must point to an existing non-root entity
- If you associate a venue, it must exist.
- You must use an existing device type. Device type cannot be empty.
- Name, description, notes are allowed.
- Inventory ownership updates on device assignment.

### Modification rules
- You can modify the device type to another valid one.

## Venue
### Creation rules
- If you include an entity, the parent must not be set
- if you include a parent, the entity must not be set
- You cannot have children upon creation.
- You may include an array of devices UUIDs
- Topology and design cannot be set
- Each subscriber receives an auto-created venue; it cannot be deleted while devices remain.

## Operator
### Creation rules
- Creating an operator automatically creates and links a default entity.

### Delete
- Subscribers under the operator must be deleted first.
- Deleting the operator then removes the auto-created entity.

## Subscriber
### Creation rules
- Creating a subscriber automatically creates and links a default subscriber venue.
- Email verification is supported; the link can be resent.

### Subscriber device onboarding rules
- First device per subscriber becomes the gateway (OLG); others become AP/mesh nodes.
- Only one gateway per subscriber; it cannot be deleted until its mesh devices are removed.
- Monitoring auto-enables on gateway add; can be toggled in the UI.

### Delete
- Subscriber deletion is allowed only when no devices remain.
- Device deletion follows hierarchy (remove mesh nodes before the gateway).
- After devices are removed, deleting the subscriber also removes its auto-created venue.

## Firewall Considerations
| Port  | Description                                    | Configurable |
|:------|:-----------------------------------------------|:------------:|
| 16004 | Default port for REST API Access to the OWPROV |     yes      |

### OWPROV Service Configuration
The configuration is kept in a file called `owprov.properties`. To understand the content of this file,
please look [here](https://github.com/routerarchitects/ra-wlan-cloud-owprov/blob/main/CONFIGURATION.md)

## Kafka topics
To read more about Kafka, follow the [document](https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/main/KAFKA.md)

