# E2E Tests for JVM Host Compiler
#
# This test suite validates the JVM host compiler by:
# 1. Running the metaffi compiler to generate host code from xllr.test IDL
# 2. Compiling and executing the generated host code using the JVM API

set(JVM_COMPILER_TEST_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/src)
set(JVM_COMPILER_TEST_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/classes)

set(METAFFI_HOME_CMAKE "$ENV{METAFFI_HOME}")
file(TO_CMAKE_PATH "${METAFFI_HOME_CMAKE}" METAFFI_HOME_CMAKE)
set(JVM_API_JAR "${METAFFI_HOME_CMAKE}/jvm/api/metaffi.api.jar")

compile_java(jvm_host_compiler_e2e_test_classes
	SOURCES
		${JVM_COMPILER_TEST_SRC_DIR}/*.java
	OUTPUT_DIR
		${JVM_COMPILER_TEST_OUTPUT_DIR}
	CLASSPATH
		${JVM_API_JAR}
	DEPENDS
		jvm_api_jar_copy
		metaffi.compiler.jvm
		metaffi
		xllr.test
	COMMENT
		"Compiling JVM host compiler E2E tests"
)

add_java_main_test(jvm_host_compiler_e2e_test
	TEST_CLASS
		HostCompilerE2ETest
	CLASSPATH
		${JVM_COMPILER_TEST_OUTPUT_DIR}
		${JVM_API_JAR}
	OUTPUT_DIR
		${JVM_COMPILER_TEST_OUTPUT_DIR}
	DEPENDS
		jvm_host_compiler_e2e_test_classes
		metaffi.compiler.jvm
		metaffi
		xllr.test
	COMMENT
		"Running JVM host compiler E2E tests"
)
