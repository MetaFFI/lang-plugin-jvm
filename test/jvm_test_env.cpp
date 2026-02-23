#include "jvm_test_env.h"

#include <utils/env_utils.h>

#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
bool trace_enabled()
{
	static int enabled = -1;
	if(enabled < 0)
	{
		enabled = get_env_var("METAFFI_JVM_TEST_TRACE").empty() ? 0 : 1;
	}
	return enabled == 1;
}

void trace(const std::string& msg)
{
	std::cerr << "+++ " << msg << std::endl;
}

std::string require_file(const std::filesystem::path& path, const char* description)
{
	if(!std::filesystem::exists(path))
	{
		std::string msg = "Missing ";
		msg += description;
		msg += ": ";
		msg += path.string();
		throw std::runtime_error(msg);
	}
	return path.string();
}

std::filesystem::path build_path(const std::filesystem::path& root,
	const std::initializer_list<const char*>& parts)
{
	std::filesystem::path result = root;
	for(const auto* part : parts)
	{
		result /= part;
	}
	return result;
}

std::string resolve_guest_jar()
{
	trace("resolve_guest_jar: start");
	auto root = std::filesystem::path(require_env("METAFFI_SOURCE_ROOT"));
	auto path = build_path(root, {
		"sdk", "test_modules", "guest_modules", "java", "test_bin", "guest_java.jar"
	});
	trace(std::string("resolve_guest_jar: candidate=") + path.string());
	return require_file(path, "guest Java jar");
}

std::string resolve_api_jar()
{
	trace("resolve_api_jar: start");
	auto home = std::filesystem::path(require_env("METAFFI_HOME"));

	std::vector<std::filesystem::path> candidates = {
		home / "jvm" / "api" / "metaffi.api.jar",
		home / "jvm" / "api" / "metaffi.api.jvm.jar",
		home / "jvm" / "metaffi.api.jar",
		home / "sdk" / "api" / "jvm" / "metaffi.api.jar"
	};

	for(const auto& candidate : candidates)
	{
		trace(std::string("resolve_api_jar: checking=") + candidate.string());
		if(std::filesystem::exists(candidate))
		{
			trace(std::string("resolve_api_jar: found=") + candidate.string());
			return candidate.string();
		}
	}

	std::string msg = "Unable to locate metaffi api jar. Checked:";
	for(const auto& candidate : candidates)
	{
		msg += "\n  ";
		msg += candidate.string();
	}
	throw std::runtime_error(msg);
}

std::string join_classpath(const std::vector<std::string>& entries)
{
	if(entries.empty())
	{
		return {};
	}

	std::string result;
	char sep = jvm_classpath_separator();
	for(const auto& entry : entries)
	{
		if(entry.empty())
		{
			continue;
		}
		if(!result.empty())
		{
			result += sep;
		}
		result += entry;
	}
	return result;
}
} // namespace

JvmTestEnv::JvmTestEnv()
	: runtime("jvm"),
	  guest_classpath(join_classpath({resolve_guest_jar(), resolve_api_jar()})),
	  guest_module(runtime.runtime_plugin(), guest_classpath)
{
	std::cerr << "+++ jvm_test_env METAFFI_SOURCE_ROOT=" << get_env_var("METAFFI_SOURCE_ROOT") << std::endl;
	std::cerr << "+++ jvm_test_env METAFFI_HOME=" << get_env_var("METAFFI_HOME") << std::endl;
	std::cerr << "+++ jvm_test_env JAVA_HOME=" << get_env_var("JAVA_HOME") << std::endl;
	std::cerr << "+++ jvm_test_env guest_classpath=" << guest_classpath << std::endl;
	if(trace_enabled())
	{
		std::cerr << "jvm_test_env: guest_classpath=" << guest_classpath << std::endl;
	}
	trace("jvm_test_env: load_runtime_plugin start");
	runtime.load_runtime_plugin();
	std::cerr << "+++ jvm_test_env runtime_loaded=jvm" << std::endl;
	trace("jvm_test_env: load_runtime_plugin done");
}

JvmTestEnv::~JvmTestEnv()
{
	runtime.release_runtime_plugin();
}

JvmTestEnv& jvm_test_env()
{
	static JvmTestEnv env;
	return env;
}

std::string require_env(const char* name)
{
	std::string value = get_env_var(name);
	if(value.empty())
	{
		std::string msg = "Environment variable not set: ";
		msg += name;
		throw std::runtime_error(msg);
	}
	return value;
}

char jvm_classpath_separator()
{
#ifdef _WIN32
	return ';';
#else
	return ':';
#endif
}

void trace_step(const std::string& msg)
{
	trace(msg);
}
