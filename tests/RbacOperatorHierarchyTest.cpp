/*
 * RBAC Operator Hierarchy Unit Tests
 *
 * Tests the direct-child-only authorization model introduced in PR #45.
 * Uses an in-memory SQLite database to seed entities, operators, policies,
 * and roles, then calls RBAC::HasAccessForUser() to verify access decisions.
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "Poco/Data/SQLite/Connector.h"
#include "Poco/Data/SessionPool.h"
#include "Poco/Logger.h"
#include "Poco/UUIDGenerator.h"

#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "RESTAPI/RESTAPI_rbac_helpers.h"
#include "StorageService.h"
#include "storage/storage_entity.h"
#include "framework/AuthClient.h"
#include "framework/EventBusManager.h"
#include "Signup.h"
#include "SerialNumberCache.h"

#include "support/OwprovTestSupport.h"
#include "framework/RESTAPI_Handler.h"
#include "framework/RESTAPI_GenericServerAccounting.h"
#include "RESTAPI/RESTAPI_managementRole_validation.h"

class MockRESTAPIHandler : public OpenWifi::RESTAPIHandler {
public:
	MockRESTAPIHandler(const OpenWifi::RESTAPIHandler::BindingMap &bindings, Poco::Logger &L,
					   OpenWifi::RESTAPI_GenericServerAccounting &Server)
		: RESTAPIHandler(bindings, L,
						 std::vector<std::string>{"GET", "POST", "PUT", "DELETE"},
						 Server, 0, false) {}
	void DoGet() override {}
	void DoPost() override {}
	void DoPut() override {}
	void DoDelete() override {}
};

// ============================================================================
// Test Fixture
// ============================================================================

class RbacOperatorHierarchyTest : public ::testing::Test {
protected:
	// Fixed UUIDs for deterministic testing
	static inline const std::string kRootEntityId = OpenWifi::EntityDB::RootUUID();
	static inline std::string entityA, entityB, entityC, entityD, entityE;
	static inline std::string operatorA, operatorB, operatorC, operatorD, operatorE;
	static inline std::string policyA, policyB, policyC, policyD, policyE;
	static inline std::string policyNoPolicyAccess, policyNoRoleAccess;
	static inline std::string roleA, roleB, roleC, roleD, roleE;
	static inline std::string roleNoPolicyAccess, roleNoRoleAccess;
	static inline std::string userA, userB, userC, userD, userE;
	static inline std::string userNoPolicyAccess, userNoRoleAccess;
	static inline bool s_initialized = false;

	static std::string NewUUID() {
		return OpenWifi::MicroServiceCreateUUID();
	}

	static void SetUpTestSuite() {
		if (s_initialized) return;
		s_initialized = true;

		// Generate UUIDs
		entityA = NewUUID(); entityB = NewUUID(); entityC = NewUUID();
		entityD = NewUUID(); entityE = NewUUID();
		operatorA = NewUUID(); operatorB = NewUUID(); operatorC = NewUUID();
		operatorD = NewUUID(); operatorE = NewUUID();
		policyA = NewUUID(); policyB = NewUUID(); policyC = NewUUID();
		policyD = NewUUID(); policyE = NewUUID();
		policyNoPolicyAccess = NewUUID(); policyNoRoleAccess = NewUUID();
		roleA = NewUUID(); roleB = NewUUID(); roleC = NewUUID();
		roleD = NewUUID(); roleE = NewUUID();
		roleNoPolicyAccess = NewUUID(); roleNoRoleAccess = NewUUID();
		userA = NewUUID(); userB = NewUUID(); userC = NewUUID();
		userD = NewUUID(); userE = NewUUID();
		userNoPolicyAccess = NewUUID(); userNoRoleAccess = NewUUID();

		// Initialize SQLite and StorageService
		class TestApp : public Poco::Util::Application {};
		static TestApp app;
		auto *storage = OpenWifi::StorageService();
		storage->initialize(app);
		storage->Start();

		// Seed entities
		SeedEntity(kRootEntityId, "", "", "Root Entity");
		SeedEntity(entityA, kRootEntityId, operatorA, "Entity A");
		SeedEntity(entityB, kRootEntityId, operatorB, "Entity B");
		SeedEntity(entityC, entityA, operatorC, "Entity C");
		SeedEntity(entityD, entityA, operatorD, "Entity D");
		SeedEntity(entityE, entityC, operatorE, "Entity E");

		// Seed operators
		SeedOperator(operatorA, entityA, "Operator A");
		SeedOperator(operatorB, entityB, "Operator B");
		SeedOperator(operatorC, entityC, "Operator C");
		SeedOperator(operatorD, entityD, "Operator D");
		SeedOperator(operatorE, entityE, "Operator E");

		// Build a full-access policy entry (all resources, FULL access)
		auto fullPolicyEntry = MakeFullPolicyEntry();

		// Seed policies for each entity
		SeedPolicy(policyA, entityA, "Policy A", {fullPolicyEntry});
		SeedPolicy(policyB, entityB, "Policy B", {fullPolicyEntry});
		SeedPolicy(policyC, entityC, "Policy C", {fullPolicyEntry});
		SeedPolicy(policyD, entityD, "Policy D", {fullPolicyEntry});
		SeedPolicy(policyE, entityE, "Policy E", {fullPolicyEntry});

		// Policy without managementPolicy/managementRole resources
		auto limitedEntry = MakeLimitedPolicyEntry();
		SeedPolicy(policyNoPolicyAccess, entityC, "Policy No MP Access", {limitedEntry});

		// Policy without managementRole resource (but has managementPolicy)
		auto noRoleEntry = MakeNoRolePolicyEntry();
		SeedPolicy(policyNoRoleAccess, entityC, "Policy No MR Access", {noRoleEntry});

		// Seed roles - each user gets a role scoped to their entity
		SeedRole(roleA, entityA, policyA, {userA}, "Role A");
		SeedRole(roleB, entityB, policyB, {userB}, "Role B");
		SeedRole(roleC, entityC, policyC, {userC}, "Role C");
		SeedRole(roleD, entityD, policyD, {userD}, "Role D");
		SeedRole(roleE, entityE, policyE, {userE}, "Role E");

		// Roles for users without specific permissions
		SeedRole(roleNoPolicyAccess, entityC, policyNoPolicyAccess, {userNoPolicyAccess}, "Role NoPolicyAccess");
		SeedRole(roleNoRoleAccess, entityC, policyNoRoleAccess, {userNoRoleAccess}, "Role NoRoleAccess");
	}

	// ---------- Seed helpers ----------

	static void SeedEntity(const std::string &id, const std::string &parent,
						   const std::string &opId, const std::string &name) {
		// Don't recreate root, it's auto-created by InitializeSystemDBs
		if (id == kRootEntityId) {
			// Root entity is already created; update it if needed
			OpenWifi::ProvObjects::Entity existing;
			if (OpenWifi::StorageService()->EntityDB().GetRecord("id", id, existing)) {
				return; // root exists already
			}
		}

		OpenWifi::ProvObjects::Entity e;
		e.info.id = id;
		e.info.name = name;
		e.info.created = e.info.modified = OpenWifi::Now();
		e.parent = parent;
		e.operatorId = opId;
		OpenWifi::StorageService()->EntityDB().CreateRecord(e);
	}

	static void SeedOperator(const std::string &id, const std::string &entId,
							 const std::string &name) {
		OpenWifi::ProvObjects::Operator op;
		op.info.id = id;
		op.info.name = name;
		op.info.created = op.info.modified = OpenWifi::Now();
		op.entityId = entId;
		OpenWifi::StorageService()->OperatorDB().CreateRecord(op);
	}

	static OpenWifi::ProvObjects::ManagementPolicyEntry MakeFullPolicyEntry() {
		OpenWifi::ProvObjects::ManagementPolicyEntry entry;
		entry.resources = {"managementPolicy", "managementRole", "operator", "entity", "venue"};
		entry.access = {"FULL"};
		// Empty users means "applies to all users"
		return entry;
	}

	static OpenWifi::ProvObjects::ManagementPolicyEntry MakeLimitedPolicyEntry() {
		OpenWifi::ProvObjects::ManagementPolicyEntry entry;
		// Only operator, entity, venue — no managementPolicy, no managementRole
		entry.resources = {"operator", "entity", "venue"};
		entry.access = {"FULL"};
		return entry;
	}

	static OpenWifi::ProvObjects::ManagementPolicyEntry MakeNoRolePolicyEntry() {
		OpenWifi::ProvObjects::ManagementPolicyEntry entry;
		// Has managementPolicy but NOT managementRole
		entry.resources = {"managementPolicy", "operator", "entity", "venue"};
		entry.access = {"FULL"};
		return entry;
	}

	static void SeedPolicy(const std::string &id, const std::string &entId,
						   const std::string &name,
						   const std::vector<OpenWifi::ProvObjects::ManagementPolicyEntry> &entries) {
		OpenWifi::ProvObjects::ManagementPolicy policy;
		policy.info.id = id;
		policy.info.name = name;
		policy.info.created = policy.info.modified = OpenWifi::Now();
		policy.entity = entId;
		policy.entries = entries;
		OpenWifi::StorageService()->PolicyDB().CreateRecord(policy);
	}

	static void SeedRole(const std::string &id, const std::string &entId,
						 const std::string &policyId,
						 const std::vector<std::string> &users,
						 const std::string &name) {
		OpenWifi::ProvObjects::ManagementRole role;
		role.info.id = id;
		role.info.name = name;
		role.info.created = role.info.modified = OpenWifi::Now();
		role.entity = entId;
		role.managementPolicy = policyId;
		role.users = users;
		OpenWifi::StorageService()->RolesDB().CreateRecord(role);
	}

	// ---------- Access check helpers ----------

	static bool CanAccess(const std::string &userId, const std::string &resourceType,
						  const std::string &action, const std::string &targetEntityId) {
		return OpenWifi::RBAC::HasAccessForUser(
			userId, resourceType, action,
			OpenWifi::RBAC::TargetScope{targetEntityId, ""});
	}
};

// ============================================================================
// Step 5: managementPolicy hierarchy tests
// ============================================================================

TEST_F(RbacOperatorHierarchyTest, A_CannotUpdatePolicyOfB) {
	// b is a sibling of a, not a child
	EXPECT_FALSE(CanAccess(userA, "managementPolicy", "MODIFY", entityB));
}

TEST_F(RbacOperatorHierarchyTest, C_CannotUpdatePolicyOfD) {
	// d is a sibling of c, not a child
	EXPECT_FALSE(CanAccess(userC, "managementPolicy", "MODIFY", entityD));
}

TEST_F(RbacOperatorHierarchyTest, A_CanUpdatePolicyOfDirectChildC) {
	EXPECT_TRUE(CanAccess(userA, "managementPolicy", "MODIFY", entityC));
}

TEST_F(RbacOperatorHierarchyTest, A_CanUpdatePolicyOfDirectChildD) {
	EXPECT_TRUE(CanAccess(userA, "managementPolicy", "MODIFY", entityD));
}

TEST_F(RbacOperatorHierarchyTest, A_CannotUpdatePolicyOfGrandchildE) {
	// e is grandchild of a (a -> c -> e), not direct child
	EXPECT_FALSE(CanAccess(userA, "managementPolicy", "MODIFY", entityE));
}

TEST_F(RbacOperatorHierarchyTest, C_CanUpdatePolicyOfDirectChildE) {
	EXPECT_TRUE(CanAccess(userC, "managementPolicy", "MODIFY", entityE));
}

// ============================================================================
// Step 6: managementPolicy permission-missing tests
// ============================================================================

TEST_F(RbacOperatorHierarchyTest, UserWithoutManagementPolicyReadCannotReadPolicy) {
	// userNoPolicyAccess has entity scope over entityC but no managementPolicy resource
	EXPECT_FALSE(CanAccess(userNoPolicyAccess, "managementPolicy", "READ", entityC));
}

TEST_F(RbacOperatorHierarchyTest, UserWithoutManagementPolicyModifyCannotUpdatePolicy) {
	EXPECT_FALSE(CanAccess(userNoPolicyAccess, "managementPolicy", "MODIFY", entityC));
}

TEST_F(RbacOperatorHierarchyTest, UserWithoutManagementPolicyCreateCannotCreatePolicy) {
	EXPECT_FALSE(CanAccess(userNoPolicyAccess, "managementPolicy", "CREATE", entityC));
}

TEST_F(RbacOperatorHierarchyTest, UserWithoutManagementPolicyDeleteCannotDeletePolicy) {
	EXPECT_FALSE(CanAccess(userNoPolicyAccess, "managementPolicy", "DELETE", entityC));
}

// ============================================================================
// Step 7: managementRole permission-missing tests
// ============================================================================

TEST_F(RbacOperatorHierarchyTest, UserWithoutManagementRoleReadCannotReadRole) {
	// userNoRoleAccess has managementPolicy but NOT managementRole resource
	EXPECT_FALSE(CanAccess(userNoRoleAccess, "managementRole", "READ", entityC));
}

TEST_F(RbacOperatorHierarchyTest, UserWithoutManagementRoleModifyCannotUpdateRole) {
	EXPECT_FALSE(CanAccess(userNoRoleAccess, "managementRole", "MODIFY", entityC));
}

TEST_F(RbacOperatorHierarchyTest, UserWithoutManagementRoleCreateCannotCreateRole) {
	EXPECT_FALSE(CanAccess(userNoRoleAccess, "managementRole", "CREATE", entityC));
}

TEST_F(RbacOperatorHierarchyTest, UserWithoutManagementRoleDeleteCannotDeleteRole) {
	EXPECT_FALSE(CanAccess(userNoRoleAccess, "managementRole", "DELETE", entityC));
}

// ============================================================================
// Step 8: Role-Policy scope compatibility tests
// ============================================================================

TEST_F(RbacOperatorHierarchyTest, A_CannotCreateRoleUsingBPolicy) {
	// User A can create roles in entityC (direct child), but policyB is in entityB
	// which A cannot access. The handler checks both role scope AND policy scope.
	// Verify A cannot read policyB (which is in entityB scope)
	EXPECT_FALSE(CanAccess(userA, "managementPolicy", "READ", entityB));
	// But A can create role in entityC
	EXPECT_TRUE(CanAccess(userA, "managementRole", "CREATE", entityC));
}

TEST_F(RbacOperatorHierarchyTest, A_CannotUpdateRoleToUseBPolicy) {
	// Same logic: A cannot access policyB's entity scope
	EXPECT_FALSE(CanAccess(userA, "managementPolicy", "READ", entityB));
}

TEST_F(RbacOperatorHierarchyTest, A_CanCreateRoleUsingDirectChildCPolicy) {
	// A can access both the role scope (entityC) and the policy scope (entityC)
	EXPECT_TRUE(CanAccess(userA, "managementRole", "CREATE", entityC));
	EXPECT_TRUE(CanAccess(userA, "managementPolicy", "READ", entityC));
}

TEST_F(RbacOperatorHierarchyTest, A_CannotCreateRoleForGrandchildEUsingPolicyE) {
	// entityE is grandchild of A, so A cannot create roles there
	EXPECT_FALSE(CanAccess(userA, "managementRole", "CREATE", entityE));
	// And A cannot read policyE either (in entityE scope)
	EXPECT_FALSE(CanAccess(userA, "managementPolicy", "READ", entityE));
}

TEST_F(RbacOperatorHierarchyTest, C_CanCreateRoleForDirectChildEUsingPolicyE) {
	// C can access both entityE (direct child) and policyE
	EXPECT_TRUE(CanAccess(userC, "managementRole", "CREATE", entityE));
	EXPECT_TRUE(CanAccess(userC, "managementPolicy", "READ", entityE));
}

// ============================================================================
// Step 9: List visibility tests
// ============================================================================

TEST_F(RbacOperatorHierarchyTest, A_ManagementPolicyListShowsOnlyDirectChildrenCAndD) {
	// A should see policies in entityC and entityD (direct children)
	EXPECT_TRUE(CanAccess(userA, "managementPolicy", "LIST", entityC));
	EXPECT_TRUE(CanAccess(userA, "managementPolicy", "LIST", entityD));
	// A should NOT see policies in entityB (sibling) or entityE (grandchild)
	EXPECT_FALSE(CanAccess(userA, "managementPolicy", "LIST", entityB));
	EXPECT_FALSE(CanAccess(userA, "managementPolicy", "LIST", entityE));
}

TEST_F(RbacOperatorHierarchyTest, C_ManagementPolicyListShowsOnlyDirectChildE) {
	// C should see policies in entityE (direct child)
	EXPECT_TRUE(CanAccess(userC, "managementPolicy", "LIST", entityE));
	// C should NOT see policies in entityB or entityD
	EXPECT_FALSE(CanAccess(userC, "managementPolicy", "LIST", entityB));
	EXPECT_FALSE(CanAccess(userC, "managementPolicy", "LIST", entityD));
}

TEST_F(RbacOperatorHierarchyTest, A_ManagementRoleListShowsOnlyDirectChildrenCAndD) {
	EXPECT_TRUE(CanAccess(userA, "managementRole", "LIST", entityC));
	EXPECT_TRUE(CanAccess(userA, "managementRole", "LIST", entityD));
	EXPECT_FALSE(CanAccess(userA, "managementRole", "LIST", entityB));
	EXPECT_FALSE(CanAccess(userA, "managementRole", "LIST", entityE));
}

TEST_F(RbacOperatorHierarchyTest, C_ManagementRoleListShowsOnlyDirectChildE) {
	EXPECT_TRUE(CanAccess(userC, "managementRole", "LIST", entityE));
	EXPECT_FALSE(CanAccess(userC, "managementRole", "LIST", entityB));
	EXPECT_FALSE(CanAccess(userC, "managementRole", "LIST", entityD));
}

// ============================================================================
// Step 10: Direct UUID access cannot bypass list filtering
// ============================================================================

TEST_F(RbacOperatorHierarchyTest, A_CannotReadPolicyBByUuid) {
	EXPECT_FALSE(CanAccess(userA, "managementPolicy", "READ", entityB));
}

TEST_F(RbacOperatorHierarchyTest, A_CannotReadPolicyEByUuid) {
	// E is grandchild, not direct child
	EXPECT_FALSE(CanAccess(userA, "managementPolicy", "READ", entityE));
}

TEST_F(RbacOperatorHierarchyTest, C_CannotReadPolicyDByUuid) {
	// D is sibling of C
	EXPECT_FALSE(CanAccess(userC, "managementPolicy", "READ", entityD));
}

TEST_F(RbacOperatorHierarchyTest, A_CannotReadRoleBByUuid) {
	EXPECT_FALSE(CanAccess(userA, "managementRole", "READ", entityB));
}

TEST_F(RbacOperatorHierarchyTest, A_CannotReadRoleEByUuid) {
	EXPECT_FALSE(CanAccess(userA, "managementRole", "READ", entityE));
}

TEST_F(RbacOperatorHierarchyTest, C_CannotReadRoleDByUuid) {
	EXPECT_FALSE(CanAccess(userC, "managementRole", "READ", entityD));
}

// ============================================================================
// Additional hierarchy boundary tests
// ============================================================================

TEST_F(RbacOperatorHierarchyTest, B_CannotAccessA) {
	// B is sibling of A
	EXPECT_FALSE(CanAccess(userB, "managementPolicy", "MODIFY", entityA));
}

TEST_F(RbacOperatorHierarchyTest, D_CannotAccessC) {
	// D is sibling of C
	EXPECT_FALSE(CanAccess(userD, "managementPolicy", "MODIFY", entityC));
}

TEST_F(RbacOperatorHierarchyTest, E_CannotAccessC) {
	// E has no children to manage
	EXPECT_FALSE(CanAccess(userE, "managementPolicy", "MODIFY", entityC));
}

TEST_F(RbacOperatorHierarchyTest, E_CannotAccessA) {
	// E cannot manage its grandparent
	EXPECT_FALSE(CanAccess(userE, "managementPolicy", "MODIFY", entityA));
}

TEST_F(RbacOperatorHierarchyTest, A_CanReadPolicyOfDirectChildC) {
	EXPECT_TRUE(CanAccess(userA, "managementPolicy", "READ", entityC));
}

TEST_F(RbacOperatorHierarchyTest, A_CanDeletePolicyOfDirectChildD) {
	EXPECT_TRUE(CanAccess(userA, "managementPolicy", "DELETE", entityD));
}

TEST_F(RbacOperatorHierarchyTest, A_CanCreatePolicyInDirectChildC) {
	EXPECT_TRUE(CanAccess(userA, "managementPolicy", "CREATE", entityC));
}

TEST_F(RbacOperatorHierarchyTest, C_CanReadRoleOfDirectChildE) {
	EXPECT_TRUE(CanAccess(userC, "managementRole", "READ", entityE));
}

TEST_F(RbacOperatorHierarchyTest, C_CanDeleteRoleOfDirectChildE) {
	EXPECT_TRUE(CanAccess(userC, "managementRole", "DELETE", entityE));
}

// ============================================================================
// Self-scope tests: users should see their own entity scope
// ============================================================================

TEST_F(RbacOperatorHierarchyTest, A_CanReadOwnEntityScope) {
	EXPECT_TRUE(CanAccess(userA, "entity", "READ", entityA));
}

TEST_F(RbacOperatorHierarchyTest, C_CanReadOwnEntityScope) {
	EXPECT_TRUE(CanAccess(userC, "entity", "READ", entityC));
}

// ============================================================================
// Role-Policy Validation Tests
// ============================================================================

TEST_F(RbacOperatorHierarchyTest, CreateManagementRoleWithSameScopePolicySucceeds) {
	OpenWifi::RESTAPIHandler::BindingMap Bindings;
	Poco::Logger &Logger = Poco::Logger::get("TestLogger");
	OpenWifi::RESTAPI_GenericServerAccounting Server;
	MockRESTAPIHandler handler(Bindings, Logger, Server);
	handler.UserInfo_.userinfo.id = userA;

	OpenWifi::ProvObjects::ManagementRole role;
	role.entity = entityC;

	EXPECT_TRUE(OpenWifi::ValidateManagementPolicyForRole(handler, policyC, role));
}

TEST_F(RbacOperatorHierarchyTest, CreateManagementRoleWithSiblingPolicyFails) {
	OpenWifi::RESTAPIHandler::BindingMap Bindings;
	Poco::Logger &Logger = Poco::Logger::get("TestLogger");
	OpenWifi::RESTAPI_GenericServerAccounting Server;
	MockRESTAPIHandler handler(Bindings, Logger, Server);
	handler.UserInfo_.userinfo.id = userA;

	OpenWifi::ProvObjects::ManagementRole role;
	role.entity = entityC;

	EXPECT_FALSE(OpenWifi::ValidateManagementPolicyForRole(handler, policyB, role));
}

TEST_F(RbacOperatorHierarchyTest, CreateManagementRoleWithGrandchildPolicyFailsForA) {
	OpenWifi::RESTAPIHandler::BindingMap Bindings;
	Poco::Logger &Logger = Poco::Logger::get("TestLogger");
	OpenWifi::RESTAPI_GenericServerAccounting Server;
	MockRESTAPIHandler handler(Bindings, Logger, Server);
	handler.UserInfo_.userinfo.id = userA;

	OpenWifi::ProvObjects::ManagementRole role;
	role.entity = entityC;

	EXPECT_FALSE(OpenWifi::ValidateManagementPolicyForRole(handler, policyE, role));
}

TEST_F(RbacOperatorHierarchyTest, CreateManagementRoleWithDirectChildPolicySucceedsForC) {
	OpenWifi::RESTAPIHandler::BindingMap Bindings;
	Poco::Logger &Logger = Poco::Logger::get("TestLogger");
	OpenWifi::RESTAPI_GenericServerAccounting Server;
	MockRESTAPIHandler handler(Bindings, Logger, Server);
	handler.UserInfo_.userinfo.id = userC;

	OpenWifi::ProvObjects::ManagementRole role;
	role.entity = entityE;

	EXPECT_TRUE(OpenWifi::ValidateManagementPolicyForRole(handler, policyE, role));
}

TEST_F(RbacOperatorHierarchyTest, UpdateManagementRoleToSameScopePolicySucceeds) {
	OpenWifi::RESTAPIHandler::BindingMap Bindings;
	Poco::Logger &Logger = Poco::Logger::get("TestLogger");
	OpenWifi::RESTAPI_GenericServerAccounting Server;
	MockRESTAPIHandler handler(Bindings, Logger, Server);
	handler.UserInfo_.userinfo.id = userA;

	OpenWifi::ProvObjects::ManagementRole role;
	role.entity = entityC;

	EXPECT_TRUE(OpenWifi::ValidateManagementPolicyForRole(handler, policyC, role));
}

TEST_F(RbacOperatorHierarchyTest, UpdateManagementRoleToSiblingPolicyFails) {
	OpenWifi::RESTAPIHandler::BindingMap Bindings;
	Poco::Logger &Logger = Poco::Logger::get("TestLogger");
	OpenWifi::RESTAPI_GenericServerAccounting Server;
	MockRESTAPIHandler handler(Bindings, Logger, Server);
	handler.UserInfo_.userinfo.id = userA;

	OpenWifi::ProvObjects::ManagementRole role;
	role.entity = entityC;

	EXPECT_FALSE(OpenWifi::ValidateManagementPolicyForRole(handler, policyB, role));
}

TEST_F(RbacOperatorHierarchyTest, MoveRoleScopeWithIncompatiblePolicyFails) {
	OpenWifi::RESTAPIHandler::BindingMap Bindings;
	Poco::Logger &Logger = Poco::Logger::get("TestLogger");
	OpenWifi::RESTAPI_GenericServerAccounting Server;
	MockRESTAPIHandler handler(Bindings, Logger, Server);
	handler.UserInfo_.userinfo.id = userA;

	OpenWifi::ProvObjects::ManagementRole role;
	role.entity = entityD;

	EXPECT_FALSE(OpenWifi::ValidateManagementPolicyForRole(handler, policyC, role));
}

TEST_F(RbacOperatorHierarchyTest, CreateRoleWithUnknownPolicyFails) {
	OpenWifi::RESTAPIHandler::BindingMap Bindings;
	Poco::Logger &Logger = Poco::Logger::get("TestLogger");
	OpenWifi::RESTAPI_GenericServerAccounting Server;
	MockRESTAPIHandler handler(Bindings, Logger, Server);
	handler.UserInfo_.userinfo.id = userA;

	OpenWifi::ProvObjects::ManagementRole role;
	role.entity = entityC;

	std::string unknownPolicyId = NewUUID();

	EXPECT_FALSE(OpenWifi::ValidateManagementPolicyForRole(handler, unknownPolicyId, role));
}
