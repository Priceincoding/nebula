nebula_add_executable(
    NAME
        storage_perf
    SOURCES
        StoragePerfTool.cpp
    OBJECTS
        $<TARGET_OBJECTS:storage_client>
        $<TARGET_OBJECTS:storage_service_handler>
        $<TARGET_OBJECTS:storage_thrift_obj>
        $<TARGET_OBJECTS:kvstore_obj>
        $<TARGET_OBJECTS:meta_client>
        $<TARGET_OBJECTS:meta_thrift_obj>
        $<TARGET_OBJECTS:common_thrift_obj>
        $<TARGET_OBJECTS:raftex_obj>
        $<TARGET_OBJECTS:raftex_thrift_obj>
        $<TARGET_OBJECTS:wal_obj>
        $<TARGET_OBJECTS:thrift_obj>
        $<TARGET_OBJECTS:dataman_obj>
        $<TARGET_OBJECTS:schema_obj>
        $<TARGET_OBJECTS:filter_obj>
        $<TARGET_OBJECTS:base_obj>
        $<TARGET_OBJECTS:thread_obj>
        $<TARGET_OBJECTS:time_obj>
        $<TARGET_OBJECTS:fs_obj>
        $<TARGET_OBJECTS:network_obj>
    LIBRARIES
        ${ROCKSDB_LIBRARIES}
        ${THRIFT_LIBRARIES}
        wangle
        gtest
)

