#include "jvm_compiler_plugin.h"

#include <runtime_manager/jvm/jni_helpers.h>
#include <utils/env_utils.h>
#include <utils/logger.hpp>
#include <utils/scope_guard.hpp>
#include <utils/safe_func.h>

#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

static auto LOG = metaffi::get_logger("jvm.compiler");

namespace
{
#ifdef _WIN32
    constexpr char classpath_separator = ';';
#else
    constexpr char classpath_separator = ':';
#endif

    std::string get_env_string(const char* name)
    {
        char* value = metaffi_getenv_alloc(name);
        if(!value)
        {
            return "";
        }
        std::string result = value;
        metaffi_free_env(value);
        return result;
    }

    std::string trim(const std::string& value)
    {
        size_t start = value.find_first_not_of(" \t\r\n");
        if(start == std::string::npos)
        {
            return "";
        }
        size_t end = value.find_last_not_of(" \t\r\n");
        return value.substr(start, end - start + 1);
    }

    std::vector<std::pair<std::string, std::string>> parse_options(const std::string& host_options)
    {
        std::vector<std::pair<std::string, std::string>> result;
        if(host_options.empty())
        {
            return result;
        }

        size_t pos = 0;
        while(pos < host_options.size())
        {
            size_t comma = host_options.find(',', pos);
            std::string token = host_options.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            token = trim(token);
            if(!token.empty())
            {
                size_t eq = token.find('=');
                if(eq == std::string::npos || eq == 0 || eq == token.size() - 1)
                {
                    throw std::runtime_error("Host options are invalid (expected key=value[,key=value...])");
                }
                std::string key = trim(token.substr(0, eq));
                std::string val = trim(token.substr(eq + 1));
                result.emplace_back(key, val);
            }
            if(comma == std::string::npos)
            {
                break;
            }
            pos = comma + 1;
        }

        return result;
    }

    void check_jni(JNIEnv* env, const std::string& context)
    {
        if(env && env->ExceptionCheck())
        {
            std::string error = get_exception_description(env);
            throw std::runtime_error(error.empty() ? context : error);
        }
    }

    std::string join_classpath(const std::vector<std::string>& entries)
    {
        std::string result;
        for(const auto& entry : entries)
        {
            if(entry.empty())
            {
                continue;
            }
            if(!result.empty())
            {
                result += classpath_separator;
            }
            result += entry;
        }
        return result;
    }

}

void JVMCompilerPlugin::initialize_jvm()
{
    std::lock_guard<std::mutex> lock(m_init_mutex);
    if(m_initialized)
    {
        return;
    }

    auto info = jvm_runtime_manager::select_highest_installed_jvm();
    m_runtime = jvm_runtime_manager::create(info);
    m_runtime->load_runtime();

    std::string metaffi_home = get_env_string("METAFFI_HOME");
    if(metaffi_home.empty())
    {
        throw std::runtime_error("METAFFI_HOME environment variable is not set");
    }

    std::filesystem::path compiler_dir = std::filesystem::path(metaffi_home) / "jvm" / "compiler";
    std::filesystem::path compiler_jar = compiler_dir / "metaffi.jvm.compiler.host.jar";
    std::filesystem::path gson_jar = compiler_dir / "gson-2.10.1.jar";
    std::filesystem::path idl_entities_jar = compiler_dir / "jvm_idl_entities.jar";

    if(!std::filesystem::exists(compiler_dir))
    {
        throw std::runtime_error("Missing JVM compiler directory: " + compiler_dir.string());
    }
    if(!std::filesystem::exists(compiler_jar))
    {
        throw std::runtime_error("Missing JVM host compiler jar: " + compiler_jar.string());
    }
    if(!std::filesystem::exists(gson_jar))
    {
        throw std::runtime_error("Missing Gson dependency jar: " + gson_jar.string());
    }
    if(!std::filesystem::exists(idl_entities_jar))
    {
        throw std::runtime_error("Missing JVM IDL entities jar: " + idl_entities_jar.string());
    }

    std::vector<std::string> classpath_entries;
    classpath_entries.push_back(gson_jar.string());
    classpath_entries.push_back(idl_entities_jar.string());

    std::string extra_classpath = get_env_string("CLASSPATH");
    if(!extra_classpath.empty())
    {
        classpath_entries.push_back(extra_classpath);
    }

    std::string classpath = join_classpath(classpath_entries);
    m_module = m_runtime->load_module(compiler_jar.string(), classpath);
    if(!m_module)
    {
        throw std::runtime_error("Failed to load JVM compiler module");
    }

    m_compiler_class = m_module->load_class("com.metaffi.compiler.host.HostCompiler");
    m_context_class = m_module->load_class("com.metaffi.compiler.host.CompilerContext");
    m_gson_class = m_module->load_class("com.google.gson.Gson");
    m_idl_def_class = m_module->load_class("com.metaffi.idl.entities.IDLDefinition");

    if(!m_compiler_class || !m_context_class || !m_gson_class || !m_idl_def_class)
    {
        throw std::runtime_error("Failed to resolve JVM compiler classes");
    }

    JNIEnv* env = nullptr;
    if(m_runtime->get_env(&env))
    {
        throw std::runtime_error("Failed to get Envrionment");
    }

    m_context_default = env->GetStaticMethodID(m_context_class, "defaultContext", "()Lcom/metaffi/compiler/host/CompilerContext;");
    check_jni(env, "Failed to resolve CompilerContext.defaultContext");

    m_context_get_gson = env->GetMethodID(m_context_class, "getGson", "()Lcom/google/gson/Gson;");
    check_jni(env, "Failed to resolve CompilerContext.getGson");

    m_compiler_ctor = env->GetMethodID(m_compiler_class, "<init>", "(Lcom/metaffi/compiler/host/CompilerContext;)V");
    check_jni(env, "Failed to resolve HostCompiler constructor");

    m_compile_method = env->GetMethodID(m_compiler_class, "compile", "(Lcom/metaffi/idl/entities/IDLDefinition;Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V");
    check_jni(env, "Failed to resolve HostCompiler.compile");

    m_gson_from_json = env->GetMethodID(m_gson_class, "fromJson", "(Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/Object;");
    check_jni(env, "Failed to resolve Gson.fromJson");

    jclass hash_map_class = env->FindClass("java/util/HashMap");
    check_jni(env, "Failed to find java/util/HashMap");
    m_hash_map_class = (jclass)env->NewGlobalRef(hash_map_class);
    env->DeleteLocalRef(hash_map_class);
    if(!m_hash_map_class)
    {
        throw std::runtime_error("Failed to create global reference for HashMap");
    }

    m_hash_map_ctor = env->GetMethodID(m_hash_map_class, "<init>", "()V");
    check_jni(env, "Failed to resolve HashMap constructor");

    m_hash_map_put = env->GetMethodID(m_hash_map_class, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    check_jni(env, "Failed to resolve HashMap.put");

    m_initialized = true;
}

void JVMCompilerPlugin::execute_host_compiler(
    const std::string& idl_def_json,
    const std::string& output_path,
    const std::string& host_options)
{
    initialize_jvm();

    JNIEnv* env = nullptr;
    if(m_runtime->get_env(&env))
    {
        throw std::runtime_error("Failed to get Envrionment");
    }

    jobject context = env->CallStaticObjectMethod(m_context_class, m_context_default);
    check_jni(env, "CompilerContext.defaultContext failed");
    if(!context)
    {
        throw std::runtime_error("Failed to create CompilerContext");
    }

    jobject gson = env->CallObjectMethod(context, m_context_get_gson);
    check_jni(env, "CompilerContext.getGson failed");
    if(!gson)
    {
        env->DeleteLocalRef(context);
        throw std::runtime_error("Failed to get Gson instance");
    }

    jstring json_str = env->NewStringUTF(idl_def_json.c_str());
    jobject idl_def = env->CallObjectMethod(gson, m_gson_from_json, json_str, m_idl_def_class);
    env->DeleteLocalRef(json_str);
    check_jni(env, "Gson.fromJson failed");
    if(!idl_def)
    {
        env->DeleteLocalRef(gson);
        env->DeleteLocalRef(context);
        throw std::runtime_error("Failed to parse IDL JSON");
    }

    jobject compiler = env->NewObject(m_compiler_class, m_compiler_ctor, context);
    check_jni(env, "HostCompiler constructor failed");
    if(!compiler)
    {
        env->DeleteLocalRef(idl_def);
        env->DeleteLocalRef(gson);
        env->DeleteLocalRef(context);
        throw std::runtime_error("Failed to create HostCompiler instance");
    }

    std::filesystem::path out_path(output_path);
    std::string output_dir = out_path.parent_path().string();
    std::string output_filename = out_path.stem().string();

    if(std::filesystem::is_directory(out_path) || output_filename.empty())
    {
        output_dir = output_path;
        output_filename = "host";
    }

    jstring output_dir_str = env->NewStringUTF(output_dir.c_str());
    jstring output_filename_str = env->NewStringUTF(output_filename.c_str());

    jobject opts_map = nullptr;
    auto options = parse_options(host_options);
    if(!options.empty())
    {
        opts_map = env->NewObject(m_hash_map_class, m_hash_map_ctor);
        check_jni(env, "Failed to create HashMap for host options");
        for(const auto& entry : options)
        {
            jstring key = env->NewStringUTF(entry.first.c_str());
            jstring val = env->NewStringUTF(entry.second.c_str());
            env->CallObjectMethod(opts_map, m_hash_map_put, key, val);
            env->DeleteLocalRef(key);
            env->DeleteLocalRef(val);
            check_jni(env, "Failed to populate host options map");
        }
    }

    env->CallVoidMethod(compiler, m_compile_method, idl_def, output_dir_str, output_filename_str, opts_map);
    check_jni(env, "HostCompiler.compile failed");

    if(opts_map)
    {
        env->DeleteLocalRef(opts_map);
    }
    env->DeleteLocalRef(output_dir_str);
    env->DeleteLocalRef(output_filename_str);
    env->DeleteLocalRef(compiler);
    env->DeleteLocalRef(idl_def);
    env->DeleteLocalRef(gson);
    env->DeleteLocalRef(context);

}

void JVMCompilerPlugin::cleanup_java_refs()
{
    if(!m_runtime)
    {
        return;
    }

    try
    {
        JNIEnv* env = nullptr;
        auto release_env = m_runtime->get_env(&env);

        if(m_compiler_class)
        {
            env->DeleteGlobalRef(m_compiler_class);
            m_compiler_class = nullptr;
        }
        if(m_context_class)
        {
            env->DeleteGlobalRef(m_context_class);
            m_context_class = nullptr;
        }
        if(m_gson_class)
        {
            env->DeleteGlobalRef(m_gson_class);
            m_gson_class = nullptr;
        }
        if(m_idl_def_class)
        {
            env->DeleteGlobalRef(m_idl_def_class);
            m_idl_def_class = nullptr;
        }
        if(m_hash_map_class)
        {
            env->DeleteGlobalRef(m_hash_map_class);
            m_hash_map_class = nullptr;
        }

    }
    catch(...)
    {
        METAFFI_WARN(LOG, "Unknown exception during - cleanup_java_refs");
    }
}

void JVMCompilerPlugin::init()
{
    initialize_jvm();
    METAFFI_INFO(LOG, "initialized");
}

void JVMCompilerPlugin::compile_to_guest(
    const std::string& idl_def_json,
    const std::string& output_path,
    const std::string& guest_options)
{
    (void)idl_def_json;
    (void)output_path;
    (void)guest_options;
    throw std::runtime_error("compile_to_guest is NOT IMPLEMENTED for JVM compiler plugin");
}

void JVMCompilerPlugin::compile_from_host(
    const std::string& idl_def_json,
    const std::string& output_path,
    const std::string& host_options)
{
    execute_host_compiler(idl_def_json, output_path, host_options);
}

JVMCompilerPlugin::~JVMCompilerPlugin()
{
    cleanup_java_refs();
    m_module.reset();
    m_runtime.reset();
}
