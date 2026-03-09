# JVM host tests (plugin-level)

find_or_install_package(Boost COMPONENTS filesystem)
find_or_install_package(doctest)

set(JVM_THIRD_PARTY_DIR ${CMAKE_CURRENT_BINARY_DIR}/third_party)

set(LOG4J_VERSION "2.25.3")
set(JACKSON_VERSION "2.21.0")
set(JACKSON_ANNOTATIONS_VERSION "2.21")
set(COMMONS_LANG_VERSION "3.20.0")

find_or_install_maven_package(LOG4J_API_JAR "org.apache.logging.log4j" "log4j-api" ${LOG4J_VERSION} ${JVM_THIRD_PARTY_DIR})
find_or_install_maven_package(LOG4J_CORE_JAR "org.apache.logging.log4j" "log4j-core" ${LOG4J_VERSION} ${JVM_THIRD_PARTY_DIR})
find_or_install_maven_package(JACKSON_CORE_JAR "com.fasterxml.jackson.core" "jackson-core" ${JACKSON_VERSION} ${JVM_THIRD_PARTY_DIR})
find_or_install_maven_package(JACKSON_DATABIND_JAR "com.fasterxml.jackson.core" "jackson-databind" ${JACKSON_VERSION} ${JVM_THIRD_PARTY_DIR})
find_or_install_maven_package(JACKSON_ANNOTATIONS_JAR "com.fasterxml.jackson.core" "jackson-annotations" ${JACKSON_ANNOTATIONS_VERSION} ${JVM_THIRD_PARTY_DIR})
find_or_install_maven_package(COMMONS_LANG_JAR "org.apache.commons" "commons-lang3" ${COMMONS_LANG_VERSION} ${JVM_THIRD_PARTY_DIR})

set(JVM_THIRD_PARTY_JARS
	${LOG4J_API_JAR}
	${LOG4J_CORE_JAR}
	${JACKSON_CORE_JAR}
	${JACKSON_DATABIND_JAR}
	${JACKSON_ANNOTATIONS_JAR}
	${COMMONS_LANG_JAR}
)

if(WIN32)
	set(JVM_THIRD_PARTY_CLASSPATH "")
	foreach(jar ${JVM_THIRD_PARTY_JARS})
		if(JVM_THIRD_PARTY_CLASSPATH STREQUAL "")
			set(JVM_THIRD_PARTY_CLASSPATH "${jar}")
		else()
			set(JVM_THIRD_PARTY_CLASSPATH "${JVM_THIRD_PARTY_CLASSPATH};${jar}")
		endif()
	endforeach()
else()
	set(JVM_THIRD_PARTY_CLASSPATH "")
	foreach(jar ${JVM_THIRD_PARTY_JARS})
		if(JVM_THIRD_PARTY_CLASSPATH STREQUAL "")
			set(JVM_THIRD_PARTY_CLASSPATH "${jar}")
		else()
			set(JVM_THIRD_PARTY_CLASSPATH "${JVM_THIRD_PARTY_CLASSPATH}:${jar}")
		endif()
	endforeach()
endif()

set(jvm_host_test_src
	${CMAKE_CURRENT_LIST_DIR}/test_main.cpp
	${CMAKE_CURRENT_LIST_DIR}/jvm_test_env.cpp
	${CMAKE_CURRENT_LIST_DIR}/jvm_wrappers.cpp
	${CMAKE_CURRENT_LIST_DIR}/test_core.cpp
	${CMAKE_CURRENT_LIST_DIR}/test_arrays.cpp
	${CMAKE_CURRENT_LIST_DIR}/test_classes.cpp
	${CMAKE_CURRENT_LIST_DIR}/test_collections.cpp
	${CMAKE_CURRENT_LIST_DIR}/test_callbacks.cpp
	${CMAKE_CURRENT_LIST_DIR}/test_errors.cpp
)

set(jvm_host_test_includes
	${sdk_include_dir}
	${metaffi_sdk_root}/api/cpp/include
	${CMAKE_CURRENT_LIST_DIR}
	${doctest_INCLUDE_DIRS}
	${Boost_INCLUDE_DIRS}
)

set(jvm_host_test_libs
	doctest::doctest
	Boost::filesystem
	metaffi.api.cpp
)

c_cpp_exe(jvm_host_test
	"${jvm_host_test_src}"
	"${jvm_host_test_includes}"
	"${jvm_host_test_libs}"
	"$ENV{METAFFI_HOME}"
	INCLUDE_IN_ALL
)

add_dependencies(jvm_host_test xllr.jvm metaffi.idl.jvm metaffi.compiler.jvm java_guest_module)

add_test(NAME jvm_host_test
	COMMAND $ENV{METAFFI_HOME}/jvm_host_test${CMAKE_EXECUTABLE_SUFFIX}
)

set(jvm_host_third_party_test_src
	${CMAKE_CURRENT_LIST_DIR}/test_main.cpp
	${CMAKE_CURRENT_LIST_DIR}/jvm_test_env.cpp
	${CMAKE_CURRENT_LIST_DIR}/jvm_wrappers.cpp
	${CMAKE_CURRENT_LIST_DIR}/test_third_party.cpp
)

c_cpp_exe(jvm_host_third_party_test
	"${jvm_host_third_party_test_src}"
	"${jvm_host_test_includes}"
	"${jvm_host_test_libs}"
	"$ENV{METAFFI_HOME}"
	INCLUDE_IN_ALL
)

add_dependencies(jvm_host_third_party_test xllr.jvm metaffi.idl.jvm metaffi.compiler.jvm java_guest_module)

add_test(NAME jvm_host_third_party_test
	COMMAND $ENV{METAFFI_HOME}/jvm_host_third_party_test${CMAKE_EXECUTABLE_SUFFIX}
)

if(WIN32)
	get_jvm_dll_search_paths(jvm_host_jvm_paths)
	set(jvm_host_test_path "$ENV{METAFFI_HOME}/sdk/api/cpp;$ENV{METAFFI_HOME}/jvm")
	if(jvm_host_jvm_paths)
		list(JOIN jvm_host_jvm_paths ";" jvm_host_jvm_paths_joined)
		set(jvm_host_test_path "${jvm_host_test_path};${jvm_host_jvm_paths_joined}")
	endif()
	set(jvm_host_test_path "${jvm_host_test_path};$ENV{PATH}")
	string(REPLACE ";" "\\;" jvm_host_test_path "${jvm_host_test_path}")
	string(REPLACE ";" "\\;" JVM_THIRD_PARTY_CLASSPATH_ESC "${JVM_THIRD_PARTY_CLASSPATH}")
	set_tests_properties(jvm_host_test jvm_host_third_party_test PROPERTIES
		ENVIRONMENT "PATH=${jvm_host_test_path};METAFFI_JVM_THIRD_PARTY_CLASSPATH=${JVM_THIRD_PARTY_CLASSPATH_ESC}"
	)
elseif(APPLE)
	set_tests_properties(jvm_host_test jvm_host_third_party_test PROPERTIES
		ENVIRONMENT "DYLD_LIBRARY_PATH=$ENV{METAFFI_HOME}/sdk/api/cpp:$ENV{METAFFI_HOME}/jvm:$ENV{DYLD_LIBRARY_PATH};METAFFI_JVM_THIRD_PARTY_CLASSPATH=${JVM_THIRD_PARTY_CLASSPATH}"
	)
else()
	set_tests_properties(jvm_host_test jvm_host_third_party_test PROPERTIES
		ENVIRONMENT "LD_LIBRARY_PATH=$ENV{METAFFI_HOME}/sdk/api/cpp:$ENV{METAFFI_HOME}/jvm:$ENV{LD_LIBRARY_PATH};METAFFI_JVM_THIRD_PARTY_CLASSPATH=${JVM_THIRD_PARTY_CLASSPATH}"
	)
endif()

set(jvm_host_test jvm_host_test jvm_host_third_party_test PARENT_SCOPE)
