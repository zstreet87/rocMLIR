add_mlir_dialect_library(MLIRAsyncDialect
  Async.cpp

  ADDITIONAL_HEADER_DIRS
  ${MLIR_MAIN_INCLUDE_DIR}/mlir/Dialect/Async

  DEPENDS
  MLIRAsyncOpsIncGen

  LINK_LIBS PUBLIC
  MLIRControlFlowInterfaces
  MLIRCallInterfaces
  MLIRDialect
  MLIRFuncDialect
  MLIRInferTypeOpInterface
  MLIRIR
  )
