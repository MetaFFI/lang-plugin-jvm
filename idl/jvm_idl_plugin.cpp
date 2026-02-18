#include <idl_compiler/idl_plugin_interface.h>
#include <runtime/xllr_capi_loader.h>
#include <runtime_manager/jvm/runtime_manager.h>
#include <runtime_manager/jvm/module.h>
#include <runtime_manager/jvm/jni_helpers.h>
#include <utils/env_utils.h>
#include <utils/logger.hpp>
#include <utils/scope_guard.hpp>
#include <utils/safe_func.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

static auto LOG = metaffi::get_logger("jvm.idl");

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

    std::vector<std::string> split_idl_paths(const std::string& value)
    {
        std::vector<std::string> paths;
        char separator = ';';
        if(value.find(':') != std::string::npos && value.find(';') == std::string::npos)
        {
            separator = ':';
        }

        size_t start = 0;
        while(start < value.size())
        {
            size_t sep = value.find(separator, start);
            if(sep == std::string::npos)
            {
                sep = value.size();
            }

            if(sep > start)
            {
                paths.push_back(value.substr(start, sep - start));
            }

            start = sep + 1;
        }

        if(paths.empty() && !value.empty())
        {
            paths.push_back(value);
        }

        return paths;
    }

    std::string join_classpath(const std::vector<std::string>& entries)
    {
        std::string result;
        for(size_t i = 0; i < entries.size(); ++i)
        {
            if(entries[i].empty())
            {
                continue;
            }
            if(!result.empty())
            {
                result += classpath_separator;
            }
            result += entries[i];
        }
        return result;
    }

    void set_error(char** out_err, uint32_t* out_err_len, const std::string& msg)
    {
        if(!out_err || !out_err_len)
        {
            return;
        }
        *out_err = xllr_alloc_string(msg.c_str(), msg.size());
        *out_err_len = static_cast<uint32_t>(msg.size());
    }

    void clear_error(char** out_err, uint32_t* out_err_len)
    {
        if(!out_err || !out_err_len)
        {
            return;
        }
        *out_err = nullptr;
        *out_err_len = 0;
    }
}

class JVMIDLPlugin : public idl_plugin_interface
{
private:
    std::shared_ptr<jvm_runtime_manager> m_runtime;
    std::shared_ptr<Module> m_module;
    jclass m_extractor_class = nullptr;
    jclass m_string_class = nullptr;
    jmethodID m_extract_method = nullptr;
    std::mutex m_init_mutex;
    bool m_initialized = false;

    void initialize_jvm()
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

        std::filesystem::path idl_dir = std::filesystem::path(metaffi_home) / "jvm" / "idl";
        std::filesystem::path compiler_jar = idl_dir / "jvm_idl_compiler.jar";
        std::filesystem::path asm_jar = idl_dir / "asm-9.6.jar";
        std::filesystem::path gson_jar = idl_dir / "gson-2.10.1.jar";

        if(!std::filesystem::exists(compiler_jar))
        {
            throw std::runtime_error("Missing JVM IDL compiler jar: " + compiler_jar.string());
        }
        if(!std::filesystem::exists(asm_jar))
        {
            throw std::runtime_error("Missing ASM dependency jar: " + asm_jar.string());
        }
        if(!std::filesystem::exists(gson_jar))
        {
            throw std::runtime_error("Missing Gson dependency jar: " + gson_jar.string());
        }

        std::vector<std::string> classpath_entries;
        classpath_entries.push_back(asm_jar.string());
        classpath_entries.push_back(gson_jar.string());

        std::string extra_classpath = get_env_string("CLASSPATH");
        if(!extra_classpath.empty())
        {
            classpath_entries.push_back(extra_classpath);
        }

        std::string classpath = join_classpath(classpath_entries);
        m_module = m_runtime->load_module(compiler_jar.string(), classpath);
        if(!m_module)
        {
            throw std::runtime_error("Failed to load JVM IDL compiler module");
        }

        m_extractor_class = m_module->load_class("com.metaffi.idl.JvmExtractor");
        if(!m_extractor_class)
        {
            throw std::runtime_error("Failed to load com.metaffi.idl.JvmExtractor");
        }

        JNIEnv* env = nullptr;
        if(m_runtime->get_env(&env))
        {
            throw std::runtime_error("Failed to get Envrionment");
        }

        jclass string_class = env->FindClass("java/lang/String");
        if(env->ExceptionCheck() || !string_class)
        {
            std::string error = get_exception_description(env);
            throw std::runtime_error(error.empty() ? "Failed to find java/lang/String" : error);
        }
        m_string_class = (jclass)env->NewGlobalRef(string_class);
        env->DeleteLocalRef(string_class);
        if(!m_string_class)
        {
            throw std::runtime_error("Failed to create global reference for java/lang/String");
        }

        m_extract_method = env->GetStaticMethodID(m_extractor_class, "extract", "([Ljava/lang/String;)Ljava/lang/String;");
        if(env->ExceptionCheck() || !m_extract_method)
        {
            std::string error = get_exception_description(env);
            throw std::runtime_error(error.empty() ? "Failed to resolve JvmExtractor.extract" : error);
        }
        m_initialized = true;
    }

    void cleanup_java_refs()
    {
        if(!m_runtime)
        {
            return;
        }

        try
        {
            JNIEnv* env = nullptr;
            if(m_runtime->get_env(&env))
            {
                throw std::runtime_error("Failed to get Envrionment");
            }

            if(m_extractor_class)
            {
                env->DeleteGlobalRef(m_extractor_class);
                m_extractor_class = nullptr;
            }
            if(m_string_class)
            {
                env->DeleteGlobalRef(m_string_class);
                m_string_class = nullptr;
            }

        }
        catch(...)
        {
            METAFFI_WARN(LOG, "Unknown exception during - cleanup_java_refs");
        }
    }

public:
    void init() override
    {
        initialize_jvm();
        METAFFI_INFO(LOG, "initialized");
    }

    void parse_idl(const char* source_code, uint32_t source_length,
                   const char* file_or_path, uint32_t file_or_path_length,
                   char** out_idl_def_json, uint32_t* out_idl_def_json_length,
                   char** out_err, uint32_t* out_err_len) override
    {
        (void)source_code;
        (void)source_length;

        if(out_idl_def_json)
        {
            *out_idl_def_json = nullptr;
        }
        if(out_idl_def_json_length)
        {
            *out_idl_def_json_length = 0;
        }
        clear_error(out_err, out_err_len);

        if(!file_or_path)
        {
            set_error(out_err, out_err_len, "file_or_path is null");
            return;
        }

        try
        {
            initialize_jvm();

            std::string paths_value(file_or_path, file_or_path_length);
            auto paths = split_idl_paths(paths_value);
            if(paths.empty())
            {
                set_error(out_err, out_err_len, "No IDL paths provided");
                return;
            }

            JNIEnv* env = nullptr;
            auto release_env = m_runtime->get_env(&env);

            jobjectArray paths_array = env->NewObjectArray(static_cast<jsize>(paths.size()), m_string_class, nullptr);
            if(env->ExceptionCheck() || !paths_array)
            {
                std::string error = get_exception_description(env);
                set_error(out_err, out_err_len, error.empty() ? "Failed to create Java String array" : error);
                return;
            }

            for(jsize i = 0; i < static_cast<jsize>(paths.size()); ++i)
            {
                jstring path = env->NewStringUTF(paths[i].c_str());
                env->SetObjectArrayElement(paths_array, i, path);
                env->DeleteLocalRef(path);
                if(env->ExceptionCheck())
                {
                    std::string error = get_exception_description(env);
                    env->DeleteLocalRef(paths_array);
                    set_error(out_err, out_err_len, error.empty() ? "Failed to fill Java String array" : error);
                    return;
                }
            }

            jstring result = (jstring)env->CallStaticObjectMethod(m_extractor_class, m_extract_method, paths_array);
            env->DeleteLocalRef(paths_array);

            if(env->ExceptionCheck() || !result)
            {
                std::string error = get_exception_description(env);
                set_error(out_err, out_err_len, error.empty() ? "JvmExtractor.extract failed" : error);
                return;
            }

            const char* json_chars = env->GetStringUTFChars(result, nullptr);
            std::string json = json_chars ? json_chars : "";
            if(json_chars)
            {
                env->ReleaseStringUTFChars(result, json_chars);
            }
            env->DeleteLocalRef(result);
            if(json.empty())
            {
                set_error(out_err, out_err_len, "JvmExtractor.extract returned empty result");
                return;
            }

            if(json.find("\"error\"") != std::string::npos)
            {
                set_error(out_err, out_err_len, json);
                return;
            }

            if(out_idl_def_json && out_idl_def_json_length)
            {
                *out_idl_def_json = xllr_alloc_string(json.c_str(), json.size());
                *out_idl_def_json_length = static_cast<uint32_t>(json.size());
            }
        }
        catch(const std::exception& e)
        {
            set_error(out_err, out_err_len, e.what());
        }
        catch(...)
        {
            set_error(out_err, out_err_len, "Unknown error in parse_idl");
        }
    }

    ~JVMIDLPlugin()
    {
        cleanup_java_refs();
        m_module.reset();
        m_runtime.reset();
    }
};

extern "C"
{
    __declspec(dllexport) idl_plugin_interface* create_plugin()
    {
        return new JVMIDLPlugin();
    }

    __declspec(dllexport) void init_plugin()
    {
        static JVMIDLPlugin plugin;
        plugin.init();
    }

    __declspec(dllexport) void parse_idl(const char* source_code, uint32_t source_code_length,
                                         const char* file_or_path, uint32_t file_or_path_length,
                                         char** out_idl_def_json, uint32_t* out_idl_def_json_length,
                                         char** out_err, uint32_t* out_err_len)
    {
        static JVMIDLPlugin plugin;
        static bool initialized = false;

        if(!initialized)
        {
            plugin.init();
            initialized = true;
        }

        plugin.parse_idl(source_code, source_code_length,
                         file_or_path, file_or_path_length,
                         out_idl_def_json, out_idl_def_json_length,
                         out_err, out_err_len);
    }
}
