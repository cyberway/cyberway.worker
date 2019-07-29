add_contract_with_abi(golos.worker golos.worker.abi golos.worker.cpp)
target_include_directories(golos.worker.wasm
   PUBLIC
   ${TOKEN_INCLUDE_PATH}
   ${CMAKE_CURRENT_SOURCE_DIR}/..)
