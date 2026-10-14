# The probe is consumed from Rust, which supplies Scarlet's ordinary CRT.
add_library(ScarletSkiaVulkan STATIC ScarletSkiaVulkan.cpp)
target_link_libraries(ScarletSkiaVulkan PRIVATE Skia::Skia WTF)
target_include_directories(ScarletSkiaVulkan PRIVATE ${CMAKE_BINARY_DIR} ${WTF_DIR})
target_compile_features(ScarletSkiaVulkan PRIVATE cxx_std_23)
install(TARGETS ScarletSkiaVulkan Skia ARCHIVE DESTINATION lib)
