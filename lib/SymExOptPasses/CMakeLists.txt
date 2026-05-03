#===------------------------------------------------------------------------===#
#
#                     The KLEE Symbolic Virtual Machine
#
# This file is distributed under the University of Illinois Open Source
# License. See LICENSE.TXT for details.
#
#===------------------------------------------------------------------------===#

add_library(kleeSymExOptPasses
  Passes.cpp
  AntiFlattening.cpp
  StructBlasting.cpp
  ObjectLinearization.cpp
)

llvm_config(kleeSymExOptPasses "${USE_LLVM_SHARED}"
  bitreader
  bitwriter
  core
  support
  irreader
  analysis
  )

target_link_libraries(kleeSymExOptPasses PRIVATE
  kleeSupport
)

target_include_directories(kleeSymExOptPasses PRIVATE ${KLEE_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
target_compile_options(kleeSymExOptPasses PRIVATE ${KLEE_COMPONENT_CXX_FLAGS})
target_compile_definitions(kleeSymExOptPasses PRIVATE ${KLEE_COMPONENT_CXX_DEFINES})
