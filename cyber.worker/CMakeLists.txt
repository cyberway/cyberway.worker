add_contract_with_abi(cyber.worker cyber.worker.abi cyber.worker.cpp)
target_include_directories(cyber.worker.wasm
   PUBLIC
   ${TOKEN_INCLUDE_PATH}
   ${CMAKE_CURRENT_SOURCE_DIR}/..)
