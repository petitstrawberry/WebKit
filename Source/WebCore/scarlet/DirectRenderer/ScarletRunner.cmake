include("${CMAKE_CURRENT_LIST_DIR}/../ScarletStaticLink.cmake")

add_executable(ScarletWebCoreRender
    "${CMAKE_CURRENT_LIST_DIR}/ScarletWebCoreRender.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ScarletElementInspection.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ScarletOfflineStrategies.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ScarletAbortTrace.cpp"
)
add_executable(ScarletWebCoreScriptCounterCheck EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_LIST_DIR}/ScarletScriptCounterCheck.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ScarletOfflineStrategies.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ScarletAbortTrace.cpp"
)
set(_scarlet_runner_targets ScarletWebCoreRender ScarletWebCoreScriptCounterCheck)
set_target_properties(ScarletWebCoreScriptCounterCheck PROPERTIES
    OUTPUT_NAME scarlet-webcore-script-counter-check
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
set_target_properties(ScarletWebCoreRender PROPERTIES
    OUTPUT_NAME scarlet-webcore-render
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
foreach (_runner IN LISTS _scarlet_runner_targets)
    scarlet_attach_engine("${_runner}")
    target_link_libraries("${_runner}" PRIVATE
        "$<LINK_GROUP:RESCAN,${_scarlet_static_group_argument}>" Threads::Threads)
    # Track the native support archive even when the driver also supplies it.
    set_property(TARGET "${_runner}" APPEND PROPERTY LINK_DEPENDS
        "${SCARLET_CXX_RUNTIME_SUPPORT_LIBRARY}")
    target_link_options("${_runner}" PRIVATE
        "LINKER:--wrap=abort"
        "LINKER:--wrap=_Z16WTFCrashWithInfoiPKcS0_")
endforeach ()
