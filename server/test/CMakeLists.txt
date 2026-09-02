find_package(GTest REQUIRED)
# 复用服务端的 pthread 配置
set(TEST_COMMON_LIBS
    server_lib
    GTest::GTest
    GTest::Main
    glog
    pthread
)

# ---- 测试目标定义 ----

add_executable(test_net_info test_net_info.cpp)
target_link_libraries(test_net_info PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_quality_assessor_gtest
    unit/test_quality_assessor_gtest.cpp)
target_link_libraries(test_quality_assessor_gtest PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_anomaly_detector_gtest
    unit/test_anomaly_detector_gtest.cpp)
target_link_libraries(test_anomaly_detector_gtest PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_audio_fusion_gtest
    unit/test_audio_fusion_gtest.cpp)
target_link_libraries(test_audio_fusion_gtest PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_band_conflict_gtest
    unit/test_band_conflict_gtest.cpp)
target_link_libraries(test_band_conflict_gtest PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_serializer_gtest
    unit/test_serializer_gtest.cpp)
target_link_libraries(test_serializer_gtest PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_event_manager_gtest
    unit/test_event_manager_gtest.cpp
    unit/mock_dbus_service.cpp)
target_link_libraries(test_event_manager_gtest PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_bt_full_link_gtest
    unit/test_bt_full_link_gtest.cpp
    unit/mock_dbus_service.cpp)
target_link_libraries(test_bt_full_link_gtest PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_bt_monitor
    special/test_bt_monitor.cpp)
target_link_libraries(test_bt_monitor PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_iface_type_gtest
    unit/test_iface_type_gtest.cpp)
target_link_libraries(test_iface_type_gtest PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_logger_gtest
    unit/test_logger_gtest.cpp)
target_link_libraries(test_logger_gtest PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_traffic_analyzer_gtest
    unit/test_traffic_analyzer_gtest.cpp)
target_link_libraries(test_traffic_analyzer_gtest PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_weak_netmgr_gtest
    unit/test_weak_netmgr_gtest.cpp)
target_link_libraries(test_weak_netmgr_gtest PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_net_iface_gtest
    unit/test_net_iface_gtest.cpp)
target_link_libraries(test_net_iface_gtest PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_rtt_monitor_gtest
    unit/test_rtt_monitor_gtest.cpp)
target_link_libraries(test_rtt_monitor_gtest PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_bt_monitor_extra_gtest
    unit/test_bt_monitor_extra_gtest.cpp)
target_link_libraries(test_bt_monitor_extra_gtest PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_jitter_monitor_gtest
    unit/test_jitter_monitor_gtest.cpp)
target_link_libraries(test_jitter_monitor_gtest PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_dns_monitor_gtest
    unit/test_dns_monitor_gtest.cpp)
target_link_libraries(test_dns_monitor_gtest PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_database_manager_gtest
    unit/test_database_manager_gtest.cpp)
target_link_libraries(test_database_manager_gtest PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_ebpf_monitor_observability_gtest
    unit/test_ebpf_monitor_observability_gtest.cpp)
target_link_libraries(test_ebpf_monitor_observability_gtest PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_weaknet_config_gtest
    unit/test_weaknet_config_gtest.cpp)
target_link_libraries(test_weaknet_config_gtest PRIVATE ${TEST_COMMON_LIBS})

add_executable(test_monitor_registry_gtest
    unit/test_monitor_registry_gtest.cpp)
target_link_libraries(test_monitor_registry_gtest PRIVATE ${TEST_COMMON_LIBS})

# ---- CTest 集成 ----
# enable_testing() 已在根 CMakeLists.txt 中调用
foreach(test_target
    test_net_info
    test_quality_assessor_gtest
    test_anomaly_detector_gtest
    test_audio_fusion_gtest
    test_band_conflict_gtest
    test_serializer_gtest
    test_event_manager_gtest
    test_bt_full_link_gtest
    test_bt_monitor
    test_iface_type_gtest
    test_logger_gtest
    test_traffic_analyzer_gtest
    test_weak_netmgr_gtest
    test_net_iface_gtest
    test_rtt_monitor_gtest
    test_bt_monitor_extra_gtest
    test_jitter_monitor_gtest
    test_dns_monitor_gtest
    test_database_manager_gtest
    test_ebpf_monitor_observability_gtest
    test_weaknet_config_gtest
    test_monitor_registry_gtest)
    add_test(NAME ${test_target} COMMAND ${test_target})
endforeach()
