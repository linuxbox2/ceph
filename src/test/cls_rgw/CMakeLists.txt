add_executable(ceph_test_cls_rgw
  test_cls_rgw.cc
  )
target_link_libraries(ceph_test_cls_rgw
  cls_rgw_client
  librados
  global
  ${UNITTEST_LIBS}
  ${EXTRALIBS}
  ${BLKID_LIBRARIES}
  ${CMAKE_DL_LIBS}
  radostest-cxx)
install(TARGETS
  ceph_test_cls_rgw
  DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(ceph_test_cls_rgw_fsio_head
  test_cls_rgw_fsio_head.cc
  ${CMAKE_SOURCE_DIR}/src/cls/rgw/cls_rgw_fsio.cc
  $<TARGET_OBJECTS:unit-main>)
target_link_libraries(ceph_test_cls_rgw_fsio_head
  cls_rgw_client
  librados
  global
  ${UNITTEST_LIBS}
  radostest-cxx)
install(TARGETS ceph_test_cls_rgw_fsio_head DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(ceph_test_cls_rgw_stats test_cls_rgw_stats.cc
  $<TARGET_OBJECTS:unit-main>)
target_link_libraries(ceph_test_cls_rgw_stats cls_rgw_client global
  librados ${UNITTEST_LIBS} radostest-cxx)
install(TARGETS ceph_test_cls_rgw_stats DESTINATION ${CMAKE_INSTALL_BINDIR})

# a model checker for FSIO's head protocol;  built only on request
add_executable(ceph_test_cls_rgw_fsio_model EXCLUDE_FROM_ALL
  fsio_model.cc
  ${CMAKE_SOURCE_DIR}/src/cls/rgw/cls_rgw_fsio.cc)
target_link_libraries(ceph_test_cls_rgw_fsio_model
  ceph-common
  ${UNITTEST_LIBS})
# the explorer visits millions of states;  optimized even in a debug build
target_compile_options(ceph_test_cls_rgw_fsio_model PRIVATE -O2)
target_compile_definitions(ceph_test_cls_rgw_fsio_model PRIVATE
  CLS_RGW_FSIO_SKIP_CHECKS)

add_executable(unittest_cls_rgw_fsio
  test_cls_rgw_fsio.cc
  ${CMAKE_SOURCE_DIR}/src/cls/rgw/cls_rgw_fsio.cc)
add_ceph_unittest(unittest_cls_rgw_fsio)
target_link_libraries(unittest_cls_rgw_fsio
  ceph-common
  cls_rgw_client
  ${UNITTEST_LIBS})
target_compile_definitions(unittest_cls_rgw_fsio PRIVATE
  CLS_RGW_FSIO_SKIP_CHECKS)
