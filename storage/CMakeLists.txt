find_package(TBB REQUIRED)

set(STORAGE_SOURCES
    src/storage/file.cc
    src/storage/types.cc
)

add_library(storage ${STORAGE_SOURCES})
target_include_directories(storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(storage PUBLIC uring cppcoro)
target_link_libraries(storage PUBLIC TBB::tbb)
target_link_libraries(storage PUBLIC TBB::tbbmalloc)


add_executable(load_data src/storage/load_data.cc)
target_link_libraries(load_data Threads::Threads storage)