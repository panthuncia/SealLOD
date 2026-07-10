find_package(nlohmann_json CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(DirectX-Headers CONFIG REQUIRED)

add_library(ProceduralWindOffline STATIC
    src/WindOffline.cpp
    src/WindProfile.cpp)
add_library(BasicRenderer::ProceduralWindOffline ALIAS ProceduralWindOffline)
target_compile_features(ProceduralWindOffline PUBLIC cxx_std_20)
target_include_directories(ProceduralWindOffline PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(ProceduralWindOffline PUBLIC nlohmann_json::nlohmann_json lz4::lz4 Microsoft::DirectX-Headers)

add_executable(ProceduralWindOfflineTests tests/WindOfflineTests.cpp)
target_link_libraries(ProceduralWindOfflineTests PRIVATE ProceduralWindOffline)
add_test(NAME ProceduralWindOfflineTests COMMAND ProceduralWindOfflineTests)

add_library(ProceduralWindPlugin STATIC src/ProceduralWindRuntime.cpp src/ProceduralWindExtension.cpp)
add_library(BasicRenderer::ProceduralWindPlugin ALIAS ProceduralWindPlugin)
target_compile_features(ProceduralWindPlugin PUBLIC cxx_std_20)
target_include_directories(ProceduralWindPlugin PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(ProceduralWindPlugin PUBLIC ProceduralWindOffline BasicRenderer::BasicRenderer)
