# The probe is consumed from Rust, which supplies Scarlet's ordinary CRT.
add_library(ScarletSkiaVulkan STATIC ScarletSkiaVulkan.cpp)
target_link_libraries(ScarletSkiaVulkan PRIVATE Skia::Skia)
target_compile_features(ScarletSkiaVulkan PRIVATE cxx_std_23)
install(TARGETS ScarletSkiaVulkan Skia ARCHIVE DESTINATION lib)
