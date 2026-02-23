
# add SDK
if(NOT DEFINED sdk_src)
	message(FATAL_ERROR "sdk_src is not defined. Ensure sdk/CMakeLists.txt is added before lang-plugin-jvm.")
endif()

# idl plugin
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/idl")

# compiler plugin
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/compiler")

# runtime
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/runtime")

# tests
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/test")

add_custom_target(jvm
	DEPENDS xllr.jvm metaffi.idl.jvm metaffi.compiler.jvm
)

if(DEFINED jvm_host_test)
	add_dependencies(jvm ${jvm_host_test})
endif()

set(METAFFI_HOME_PATH $ENV{METAFFI_HOME})
if(NOT METAFFI_HOME_PATH)
	message(FATAL_ERROR "METAFFI_HOME environment variable not set. JVM API jar will not be copied.")
endif()

set(JVM_API_SRC_JAR "${METAFFI_HOME_PATH}/sdk/api/jvm/metaffi.api.jar")
set(JVM_API_DEST_DIR "${METAFFI_HOME_PATH}/jvm/api")

add_custom_command(
	OUTPUT "${JVM_API_DEST_DIR}/metaffi.api.jar"
	COMMAND ${CMAKE_COMMAND} -E make_directory "${JVM_API_DEST_DIR}"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${JVM_API_SRC_JAR}" "${JVM_API_DEST_DIR}"
	DEPENDS metaffi.api "${JVM_API_SRC_JAR}"
	COMMENT "Copying JVM API jar to ${JVM_API_DEST_DIR}"
)

add_custom_target(jvm_api_jar_copy
	DEPENDS "${JVM_API_DEST_DIR}/metaffi.api.jar"
)

add_dependencies(jvm jvm_api_jar_copy)

set(jvm ${jvm} PARENT_SCOPE)
