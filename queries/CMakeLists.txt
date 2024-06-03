find_package(TBB REQUIRED)

add_compile_definitions(USE_ALLOCATOR)
#add_compile_definitions(USE_SCALABLE_ALLOCATOR)
add_executable(tpch_q1 tpch_q1.cc)
target_link_libraries(tpch_q1 Threads::Threads storage)
target_link_libraries(tpch_q1 TBB::tbb)
target_link_libraries(tpch_q1 TBB::tbbmalloc)

add_executable(tpch_q14 tpch_q14.cc)
target_link_libraries(tpch_q14 Threads::Threads storage)
