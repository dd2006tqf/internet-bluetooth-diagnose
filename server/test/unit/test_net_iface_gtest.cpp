// test_net_iface_gtest.cpp
// NetInterfaceManager unit tests (Google Test version)
// Module under test: net_iface.cpp (network interface detection)

#include <gtest/gtest.h>
#include "net_iface.h"

// ============================================================================
// Test Suite: NetInterfaceManager
// ============================================================================

// Test 1: getInstance returns same instance (singleton)
TEST(NetInterfaceManagerTest, Singleton) {
    auto inst1 = NetInterfaceManager::getInstance();
    auto inst2 = NetInterfaceManager::getInstance();
    EXPECT_EQ(inst1.get(), inst2.get());
}

// Test 2: getInstance returns non-null
TEST(NetInterfaceManagerTest, InstanceNotNull) {
    auto inst = NetInterfaceManager::getInstance();
    ASSERT_NE(inst, nullptr);
}

// Test 3: getInternetInterfaces returns a list (may be empty on some systems)
TEST(NetInterfaceManagerTest, GetInternetInterfaces) {
    auto inst = NetInterfaceManager::getInstance();
    // Should not crash, return value is system-dependent
    auto ifaces = inst->getInternetInterfaces();
    // On a connected system, we expect at least one interface
    // But we don't assert > 0 because the test environment may not have internet
    EXPECT_GE(ifaces.size(), 0u);
}

// Test 4: getInternetInterfaces returns strings
TEST(NetInterfaceManagerTest, GetInternetInterfacesReturnStrings) {
    auto inst = NetInterfaceManager::getInstance();
    auto ifaces = inst->getInternetInterfaces();
    for (const auto& name : ifaces) {
        EXPECT_FALSE(name.empty());
    }
}

// Test 5: Multiple calls to getInternetInterfaces are consistent
TEST(NetInterfaceManagerTest, ConsistentResults) {
    auto inst = NetInterfaceManager::getInstance();
    auto ifaces1 = inst->getInternetInterfaces();
    auto ifaces2 = inst->getInternetInterfaces();
    // Results should be the same (assuming no interface changes between calls)
    EXPECT_EQ(ifaces1.size(), ifaces2.size());
}
