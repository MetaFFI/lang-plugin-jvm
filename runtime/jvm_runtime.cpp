#include <runtime/runtime_plugin_api.h>
#include <runtime/xllr_capi_loader.h>
#include <runtime/metaffi_primitives.h>
#include <cdts_serializer/jvm/cdts_jvm_serializer.h>
#include <runtime_manager/jvm/runtime_manager.h>
#include <runtime_manager/jvm/module.h>
#include <runtime_manager/jvm/entity.h>
#include <runtime_manager/jvm/jni_helpers.h>
#include <runtime_manager/jvm/jni_metaffi_handle.h>
#include <runtime_manager/jvm/jni_caller.h>
#include <runtime_manager/jvm/class_loader.h>
#include <runtime_manager/jvm/contexts.h>
#include <runtime_manager/jvm/runtime_id.h>
#include <utils/entity_path_parser.h>
#include <utils/env_utils.h>
#include <utils/logger.hpp>
#include <utils/scope_guard.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using metaffi::utils::cdts_jvm_serializer;

static auto LOG = metaffi::get_logger("jvm.runtime");

// ---------------------------------------------------------------------------
// Cached JNI class refs and method IDs for reflection hot-path
// Initialized once in init_jni_reflection_cache() and never released (lives
// for the duration of the JVM).
// ---------------------------------------------------------------------------
static jclass    g_cls_object     = nullptr; // java/lang/Object
static jclass    g_cls_method     = nullptr; // java/lang/reflect/Method
static jclass    g_cls_constructor= nullptr; // java/lang/reflect/Constructor
static jclass    g_cls_field      = nullptr; // java/lang/reflect/Field
static jclass    g_cls_class      = nullptr; // java/lang/Class
static jclass    g_cls_array      = nullptr; // java/lang/reflect/Array

static jmethodID g_mid_method_invoke          = nullptr; // Method.invoke(Object, Object[])
static jmethodID g_mid_method_getParamTypes   = nullptr; // Method.getParameterTypes()
static jmethodID g_mid_ctor_newInstance        = nullptr; // Constructor.newInstance(Object[])
static jmethodID g_mid_ctor_getParamTypes     = nullptr; // Constructor.getParameterTypes()
static jmethodID g_mid_field_get              = nullptr; // Field.get(Object)
static jmethodID g_mid_field_set              = nullptr; // Field.set(Object, Object)
static jmethodID g_mid_class_isArray          = nullptr; // Class.isArray()
static jmethodID g_mid_class_getComponentType = nullptr; // Class.getComponentType()
static jmethodID g_mid_array_getLength        = nullptr; // Array.getLength(Object)
static jmethodID g_mid_array_newInstance      = nullptr; // Array.newInstance(Class, int)
static jmethodID g_mid_array_get              = nullptr; // Array.get(Object, int)
static jmethodID g_mid_array_set              = nullptr; // Array.set(Object, int, Object)

static std::once_flag g_jni_cache_once;

static void init_jni_reflection_cache(JNIEnv* env)
{
    std::call_once(g_jni_cache_once, [&]()
    {
        auto cache_class = [&](const char* name) -> jclass
        {
            jclass lc = env->FindClass(name);
            jclass gc = (jclass)env->NewGlobalRef(lc);
            env->DeleteLocalRef(lc);
            return gc;
        };

        g_cls_object      = cache_class("java/lang/Object");
        g_cls_method      = cache_class("java/lang/reflect/Method");
        g_cls_constructor = cache_class("java/lang/reflect/Constructor");
        g_cls_field       = cache_class("java/lang/reflect/Field");
        g_cls_class       = cache_class("java/lang/Class");
        g_cls_array       = cache_class("java/lang/reflect/Array");

        g_mid_method_invoke          = env->GetMethodID(g_cls_method, "invoke", "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
        g_mid_method_getParamTypes   = env->GetMethodID(g_cls_method, "getParameterTypes", "()[Ljava/lang/Class;");
        g_mid_ctor_newInstance        = env->GetMethodID(g_cls_constructor, "newInstance", "([Ljava/lang/Object;)Ljava/lang/Object;");
        g_mid_ctor_getParamTypes     = env->GetMethodID(g_cls_constructor, "getParameterTypes", "()[Ljava/lang/Class;");
        g_mid_field_get              = env->GetMethodID(g_cls_field, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
        g_mid_field_set              = env->GetMethodID(g_cls_field, "set", "(Ljava/lang/Object;Ljava/lang/Object;)V");
        g_mid_class_isArray          = env->GetMethodID(g_cls_class, "isArray", "()Z");
        g_mid_class_getComponentType = env->GetMethodID(g_cls_class, "getComponentType", "()Ljava/lang/Class;");
        g_mid_array_getLength        = env->GetStaticMethodID(g_cls_array, "getLength", "(Ljava/lang/Object;)I");
        g_mid_array_newInstance      = env->GetStaticMethodID(g_cls_array, "newInstance", "(Ljava/lang/Class;I)Ljava/lang/Object;");
        g_mid_array_get              = env->GetStaticMethodID(g_cls_array, "get", "(Ljava/lang/Object;I)Ljava/lang/Object;");
        g_mid_array_set              = env->GetStaticMethodID(g_cls_array, "set", "(Ljava/lang/Object;ILjava/lang/Object;)V");
    });
}

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
        if(trace_enabled())
        {
            std::cerr << msg << std::endl;
        }
    }

    void set_error(char** out_err, const std::string& msg)
    {
        if(!out_err)
        {
            return;
        }
        *out_err = xllr_alloc_string(msg.c_str(), msg.size());
    }

    void clear_error(char** out_err)
    {
        if(out_err)
        {
            *out_err = nullptr;
        }
    }

    bool is_array_type(metaffi_type type)
    {
        return (type & metaffi_array_type) == metaffi_array_type;
    }

    metaffi_type base_type(metaffi_type type)
    {
        return is_array_type(type) ? (type & ~(metaffi_array_type | metaffi_packed_type)) : type;
    }

    std::string to_internal_name(const std::string& name)
    {
        std::string out = name;
        std::replace(out.begin(), out.end(), '.', '/');
        return out;
    }

    std::string to_dotted_name(const std::string& name)
    {
        std::string out = name;
        std::replace(out.begin(), out.end(), '/', '.');
        return out;
    }

    std::vector<std::string> build_class_candidates(const std::string& class_name)
    {
        std::vector<std::string> candidates;
        candidates.push_back(class_name);

        std::string nested_candidate = class_name;
        for(size_t i = nested_candidate.find_last_of('.'); i != std::string::npos; i = nested_candidate.find_last_of('.', i - 1))
        {
            nested_candidate[i] = '$';
            candidates.push_back(nested_candidate);
            if(i == 0)
            {
                break;
            }
        }

        return candidates;
    }

    jclass load_class_with_fallback(jni_class_loader& loader, const std::string& class_name)
    {
        std::string normalized = to_dotted_name(class_name);
        std::string last_error;

        for(const auto& candidate : build_class_candidates(normalized))
        {
            try
            {
                return (jclass)loader.load_class(candidate);
            }
            catch(const std::exception& e)
            {
                last_error = e.what();
            }
        }

        throw std::runtime_error(last_error.empty() ? ("Failed to load Java class: " + normalized) : last_error);
    }

    jclass load_array_class_with_fallback(jni_class_loader& loader, const std::string& descriptor)
    {
        std::string binary = descriptor;
        std::replace(binary.begin(), binary.end(), '/', '.');
        std::string last_error;

        try
        {
            return (jclass)loader.load_class(binary);
        }
        catch(const std::exception& e)
        {
            last_error = e.what();
        }

        auto pos = binary.find('L');
        auto end = binary.rfind(';');
        if(pos != std::string::npos && end != std::string::npos && end > pos + 1)
        {
            std::string prefix = binary.substr(0, pos + 1);
            std::string type_name = binary.substr(pos + 1, end - pos - 1);
            std::string suffix = binary.substr(end);

            for(const auto& candidate : build_class_candidates(type_name))
            {
                std::string candidate_binary = prefix + candidate + suffix;
                try
                {
                    return (jclass)loader.load_class(candidate_binary);
                }
                catch(const std::exception& e)
                {
                    last_error = e.what();
                }
            }
        }

        throw std::runtime_error(last_error.empty() ? ("Failed to load Java array class: " + binary) : last_error);
    }

    jclass get_primitive_class(JNIEnv* env, const char* wrapper_class_name)
    {
        jclass wrapper = env->FindClass(wrapper_class_name);
        if(!wrapper)
        {
            throw std::runtime_error("Failed to find wrapper class");
        }
        jfieldID type_field = env->GetStaticFieldID(wrapper, "TYPE", "Ljava/lang/Class;");
        if(!type_field)
        {
            env->DeleteLocalRef(wrapper);
            throw std::runtime_error("Failed to get TYPE field");
        }
        jclass prim = (jclass)env->GetStaticObjectField(wrapper, type_field);
        env->DeleteLocalRef(wrapper);
        if(!prim)
        {
            throw std::runtime_error("Failed to get primitive class");
        }
        return prim;
    }

    struct alias_array_info
    {
        std::string base;
        int dims = 0;
    };

    alias_array_info parse_alias_array(const std::string& alias)
    {
        alias_array_info info{alias, 0};
        size_t pos = std::string::npos;
        while((pos = info.base.find("[]")) != std::string::npos)
        {
            info.base.erase(pos, 2);
            info.dims++;
        }
        return info;
    }

    std::string resolve_handle_alias(const metaffi_type_info& type_info)
    {
        if(type_info.alias && *type_info.alias)
        {
            return std::string(type_info.alias);
        }
        return "";
    }

    std::string descriptor_for_base(metaffi_type base, const metaffi_type_info& type_info)
    {
        switch(base)
        {
            case metaffi_bool_type: return "Z";
            case metaffi_int8_type: return "B";
            case metaffi_uint8_type: return "S";
            case metaffi_int16_type: return "S";
            case metaffi_uint16_type: return "I";
            case metaffi_int32_type: return "I";
            case metaffi_uint32_type: return "J";
            case metaffi_int64_type: return "J";
            case metaffi_uint64_type: return "Ljava/math/BigInteger;";
            case metaffi_float32_type: return "F";
            case metaffi_float64_type: return "D";
            case metaffi_char8_type:
            case metaffi_char16_type:
            case metaffi_char32_type:
                return "C";
            case metaffi_string8_type:
            case metaffi_string16_type:
            case metaffi_string32_type:
                return "Ljava/lang/String;";
            case metaffi_callable_type:
                return "Lmetaffi/api/accessor/Caller;";
            case metaffi_handle_type:
            {
                std::string alias = resolve_handle_alias(type_info);
                if(!alias.empty())
                {
                    auto parsed = parse_alias_array(alias);
                    return "L" + to_internal_name(parsed.base) + ";";
                }
                return "Ljava/lang/Object;";
            }
            case metaffi_any_type:
            case metaffi_null_type:
                return "Ljava/lang/Object;";
            case metaffi_size_type:
                return "J";
            default:
                return "Ljava/lang/Object;";
        }
    }

    jclass resolve_jclass(JNIEnv* env, const metaffi_type_info& type_info, jni_class_loader* loader = nullptr)
    {
        metaffi_type type = type_info.type;
        if(is_array_type(type))
        {
            if(type_info.fixed_dimensions <= 0)
            {
                throw std::runtime_error("Array type requires fixed_dimensions");
            }

            std::string desc;
            desc.reserve(static_cast<size_t>(type_info.fixed_dimensions) + 8);
            for(int i = 0; i < type_info.fixed_dimensions; i++)
            {
                desc.push_back('[');
            }
            desc += descriptor_for_base(base_type(type), type_info);

            if(loader)
            {
                return load_array_class_with_fallback(*loader, desc);
            }

            jclass arr_cls = env->FindClass(desc.c_str());
            if(!arr_cls)
            {
                throw std::runtime_error("Failed to resolve array class: " + desc);
            }
            return arr_cls;
        }

        switch(type)
        {
            case metaffi_bool_type: return get_primitive_class(env, "java/lang/Boolean");
            case metaffi_int8_type: return get_primitive_class(env, "java/lang/Byte");
            case metaffi_uint8_type: return get_primitive_class(env, "java/lang/Short");
            case metaffi_int16_type: return get_primitive_class(env, "java/lang/Short");
            case metaffi_uint16_type: return get_primitive_class(env, "java/lang/Integer");
            case metaffi_int32_type: return get_primitive_class(env, "java/lang/Integer");
            case metaffi_uint32_type: return get_primitive_class(env, "java/lang/Long");
            case metaffi_int64_type: return get_primitive_class(env, "java/lang/Long");
            case metaffi_size_type: return get_primitive_class(env, "java/lang/Long");
            case metaffi_float32_type: return get_primitive_class(env, "java/lang/Float");
            case metaffi_float64_type: return get_primitive_class(env, "java/lang/Double");
            case metaffi_char8_type:
            case metaffi_char16_type:
            case metaffi_char32_type:
                return get_primitive_class(env, "java/lang/Character");
            case metaffi_string8_type:
            case metaffi_string16_type:
            case metaffi_string32_type:
                return env->FindClass("java/lang/String");
            case metaffi_uint64_type:
                return env->FindClass("java/math/BigInteger");
            case metaffi_callable_type:
                return env->FindClass("metaffi/api/accessor/Caller");
            case metaffi_handle_type:
            {
                std::string alias = resolve_handle_alias(type_info);
                if(!alias.empty())
                {
                    auto parsed = parse_alias_array(alias);
                    if(parsed.dims > 0)
                    {
                        std::string desc(static_cast<size_t>(parsed.dims), '[');
                        desc += "L" + to_internal_name(parsed.base) + ";";
                        if(loader)
                        {
                            return load_array_class_with_fallback(*loader, desc);
                        }

                        jclass arr_cls = env->FindClass(desc.c_str());
                        if(!arr_cls)
                        {
                            throw std::runtime_error("Failed to resolve handle alias array class: " + alias);
                        }
                        return arr_cls;
                    }

                    std::string name = to_internal_name(parsed.base);
                    jclass cls = nullptr;
                    if(loader)
                    {
                        cls = load_class_with_fallback(*loader, to_dotted_name(name));
                    }
                    else
                    {
                        cls = env->FindClass(name.c_str());
                    }
                    if(!cls)
                    {
                        throw std::runtime_error("Failed to resolve handle alias class: " + alias);
                    }
                    return cls;
                }
                return env->FindClass("java/lang/Object");
            }
            case metaffi_any_type:
            case metaffi_null_type:
            default:
                return env->FindClass("java/lang/Object");
        }
    }

    jobject create_big_integer(JNIEnv* env, uint64_t value)
    {
        jclass big_int_cls = env->FindClass("java/math/BigInteger");
        if(!big_int_cls)
        {
            throw std::runtime_error("Failed to find BigInteger class");
        }

        if(value == 0)
        {
            jfieldID zero_field = env->GetStaticFieldID(big_int_cls, "ZERO", "Ljava/math/BigInteger;");
            if(!zero_field)
            {
                env->DeleteLocalRef(big_int_cls);
                throw std::runtime_error("Failed to get BigInteger.ZERO");
            }
            jobject zero_obj = env->GetStaticObjectField(big_int_cls, zero_field);
            env->DeleteLocalRef(big_int_cls);
            return zero_obj;
        }

        jmethodID ctor = env->GetMethodID(big_int_cls, "<init>", "(I[B)V");
        if(!ctor)
        {
            env->DeleteLocalRef(big_int_cls);
            throw std::runtime_error("Failed to get BigInteger constructor");
        }

        jbyteArray bytes = env->NewByteArray(8);
        if(!bytes)
        {
            env->DeleteLocalRef(big_int_cls);
            throw std::runtime_error("Failed to allocate BigInteger byte array");
        }

        jbyte buf[8];
        for(int i = 0; i < 8; i++)
        {
            buf[7 - i] = static_cast<jbyte>((value >> (i * 8)) & 0xFF);
        }

        env->SetByteArrayRegion(bytes, 0, 8, buf);
        jobject big = env->NewObject(big_int_cls, ctor, 1, bytes);
        env->DeleteLocalRef(bytes);
        env->DeleteLocalRef(big_int_cls);
        return big;
    }
    uint64_t big_integer_to_uint64(JNIEnv* env, jobject big_int)
    {
        if(!big_int)
        {
            return 0;
        }

        jclass cls = env->FindClass("java/math/BigInteger");
        if(!cls)
        {
            throw std::runtime_error("Failed to find BigInteger class");
        }

        jmethodID bit_length = env->GetMethodID(cls, "bitLength", "()I");
        jmethodID long_value = env->GetMethodID(cls, "longValue", "()J");
        if(!bit_length || !long_value)
        {
            env->DeleteLocalRef(cls);
            throw std::runtime_error("Failed to get BigInteger methods");
        }

        jint bits = env->CallIntMethod(big_int, bit_length);
        if(bits > 64)
        {
            env->DeleteLocalRef(cls);
            throw std::runtime_error("BigInteger value exceeds 64 bits");
        }

        jlong value = env->CallLongMethod(big_int, long_value);
        env->DeleteLocalRef(cls);
        return static_cast<uint64_t>(value);
    }

    int get_array_dimensions(JNIEnv* env, jarray arr)
    {
        if(!arr)
        {
            return 0;
        }

        jclass cls = env->GetObjectClass(arr);
        jmethodID get_class = env->GetMethodID(cls, "getClass", "()Ljava/lang/Class;");
        jobject cls_obj = env->CallObjectMethod(arr, get_class);
        jclass cls_cls = env->GetObjectClass(cls_obj);
        jmethodID get_name = env->GetMethodID(cls_cls, "getName", "()Ljava/lang/String;");
        jstring name = (jstring)env->CallObjectMethod(cls_obj, get_name);
        const char* name_str = env->GetStringUTFChars(name, nullptr);

        int dims = 0;
        for(const char* c = name_str; *c != '\0'; ++c)
        {
            if(*c == '[')
            {
                dims++;
            }
        }

        env->ReleaseStringUTFChars(name, name_str);
        env->DeleteLocalRef(name);
        env->DeleteLocalRef(cls_cls);
        env->DeleteLocalRef(cls_obj);
        env->DeleteLocalRef(cls);

        return dims;
    }

    bool is_local_ref(JNIEnv* env, jobject obj)
    {
        if(!obj)
        {
            return false;
        }
        return env->GetObjectRefType(obj) == JNILocalRefType;
    }

    void delete_local_ref_if_needed(JNIEnv* env, jobject obj)
    {
        if(obj && is_local_ref(env, obj))
        {
            env->DeleteLocalRef(obj);
        }
    }

    struct number_cache
    {
        jclass cls = nullptr;
        jmethodID byte_value = nullptr;
        jmethodID short_value = nullptr;
        jmethodID int_value = nullptr;
        jmethodID long_value = nullptr;
        jmethodID float_value = nullptr;
        jmethodID double_value = nullptr;
    };

    number_cache& get_number_cache(JNIEnv* env)
    {
        static number_cache cache;
        if(!cache.cls)
        {
            jclass tmp = env->FindClass("java/lang/Number");
            if(!tmp)
            {
                throw std::runtime_error("Failed to find java/lang/Number");
            }
            cache.cls = (jclass)env->NewGlobalRef(tmp);
            env->DeleteLocalRef(tmp);
        }
        if(!cache.byte_value)
        {
            cache.byte_value = env->GetMethodID(cache.cls, "byteValue", "()B");
            cache.short_value = env->GetMethodID(cache.cls, "shortValue", "()S");
            cache.int_value = env->GetMethodID(cache.cls, "intValue", "()I");
            cache.long_value = env->GetMethodID(cache.cls, "longValue", "()J");
            cache.float_value = env->GetMethodID(cache.cls, "floatValue", "()F");
            cache.double_value = env->GetMethodID(cache.cls, "doubleValue", "()D");
            if(!cache.byte_value || !cache.short_value || !cache.int_value || !cache.long_value || !cache.float_value || !cache.double_value)
            {
                throw std::runtime_error("Failed to resolve Number methods");
            }
        }
        return cache;
    }

    bool is_number(JNIEnv* env, jobject obj)
    {
        auto& cache = get_number_cache(env);
        return env->IsInstanceOf(obj, cache.cls) == JNI_TRUE;
    }

    jlong number_long_value(JNIEnv* env, jobject obj)
    {
        auto& cache = get_number_cache(env);
        return env->CallLongMethod(obj, cache.long_value);
    }

    jdouble number_double_value(JNIEnv* env, jobject obj)
    {
        auto& cache = get_number_cache(env);
        return env->CallDoubleMethod(obj, cache.double_value);
    }

    jboolean boolean_value(JNIEnv* env, jobject obj)
    {
        jclass cls = env->FindClass("java/lang/Boolean");
        jmethodID mid = env->GetMethodID(cls, "booleanValue", "()Z");
        jboolean val = env->CallBooleanMethod(obj, mid);
        env->DeleteLocalRef(cls);
        return val;
    }

    jchar char_value(JNIEnv* env, jobject obj)
    {
        jclass cls = env->FindClass("java/lang/Character");
        jmethodID mid = env->GetMethodID(cls, "charValue", "()C");
        jchar val = env->CallCharMethod(obj, mid);
        env->DeleteLocalRef(cls);
        return val;
    }

    jobject box_boolean(JNIEnv* env, jboolean val)
    {
        jclass cls = env->FindClass("java/lang/Boolean");
        jmethodID ctor = env->GetMethodID(cls, "<init>", "(Z)V");
        jobject obj = env->NewObject(cls, ctor, val);
        env->DeleteLocalRef(cls);
        return obj;
    }

    jobject box_byte(JNIEnv* env, jbyte val)
    {
        jclass cls = env->FindClass("java/lang/Byte");
        jmethodID ctor = env->GetMethodID(cls, "<init>", "(B)V");
        jobject obj = env->NewObject(cls, ctor, val);
        env->DeleteLocalRef(cls);
        return obj;
    }

    jobject box_short(JNIEnv* env, jshort val)
    {
        jclass cls = env->FindClass("java/lang/Short");
        jmethodID ctor = env->GetMethodID(cls, "<init>", "(S)V");
        jobject obj = env->NewObject(cls, ctor, val);
        env->DeleteLocalRef(cls);
        return obj;
    }

    jobject box_int(JNIEnv* env, jint val)
    {
        jclass cls = env->FindClass("java/lang/Integer");
        jmethodID ctor = env->GetMethodID(cls, "<init>", "(I)V");
        jobject obj = env->NewObject(cls, ctor, val);
        env->DeleteLocalRef(cls);
        return obj;
    }

    jobject box_long(JNIEnv* env, jlong val)
    {
        jclass cls = env->FindClass("java/lang/Long");
        jmethodID ctor = env->GetMethodID(cls, "<init>", "(J)V");
        jobject obj = env->NewObject(cls, ctor, val);
        env->DeleteLocalRef(cls);
        return obj;
    }

    jobject box_float(JNIEnv* env, jfloat val)
    {
        jclass cls = env->FindClass("java/lang/Float");
        jmethodID ctor = env->GetMethodID(cls, "<init>", "(F)V");
        jobject obj = env->NewObject(cls, ctor, val);
        env->DeleteLocalRef(cls);
        return obj;
    }

    jobject box_double(JNIEnv* env, jdouble val)
    {
        jclass cls = env->FindClass("java/lang/Double");
        jmethodID ctor = env->GetMethodID(cls, "<init>", "(D)V");
        jobject obj = env->NewObject(cls, ctor, val);
        env->DeleteLocalRef(cls);
        return obj;
    }

    jobject box_char(JNIEnv* env, jchar val)
    {
        jclass cls = env->FindClass("java/lang/Character");
        jmethodID ctor = env->GetMethodID(cls, "<init>", "(C)V");
        jobject obj = env->NewObject(cls, ctor, val);
        env->DeleteLocalRef(cls);
        return obj;
    }

    void set_accessible(JNIEnv* env, jobject obj)
    {
        jclass cls = env->FindClass("java/lang/reflect/AccessibleObject");
        jmethodID mid = env->GetMethodID(cls, "setAccessible", "(Z)V");
        env->CallVoidMethod(obj, mid, JNI_TRUE);
        env->DeleteLocalRef(cls);
    }

    // Coerce arguments to match the method/constructor's declared parameter types.
    // This handles cases where CDT serialization produces Object[] for opaque handles,
    // but the Java method expects a more specific array type (e.g. SomeClass[], Color).
    void coerce_args_to_param_types(JNIEnv* env, jobject method_or_ctor, jobjectArray args, bool is_constructor)
    {
        if(!args) return;

        jsize arg_count = env->GetArrayLength(args);
        if(arg_count == 0) return; // Fast path: nothing to coerce for zero args

        jmethodID get_param_types = is_constructor ? g_mid_ctor_getParamTypes : g_mid_method_getParamTypes;
        jobjectArray param_types = (jobjectArray)env->CallObjectMethod(method_or_ctor, get_param_types);

        if(!param_types) return;

        jsize param_count = env->GetArrayLength(param_types);
        jsize count = (std::min)(param_count, arg_count);

        for(jsize i = 0; i < count; i++)
        {
            jclass expected_type = (jclass)env->GetObjectArrayElement(param_types, i);
            jobject arg = env->GetObjectArrayElement(args, i);

            if(!arg || !expected_type)
            {
                if(expected_type) env->DeleteLocalRef(expected_type);
                if(arg) env->DeleteLocalRef(arg);
                continue;
            }

            // Only coerce if the argument doesn't match and the expected type is an array
            jboolean is_expected_array = env->CallBooleanMethod(expected_type, g_mid_class_isArray);
            if(is_expected_array && !env->IsInstanceOf(arg, expected_type))
            {
                // Get the expected component type and array length
                jclass expected_component = (jclass)env->CallObjectMethod(expected_type, g_mid_class_getComponentType);
                jint len = env->CallStaticIntMethod(g_cls_array, g_mid_array_getLength, arg);

                // Create a new array with the correct component type
                jobject new_array = env->CallStaticObjectMethod(g_cls_array, g_mid_array_newInstance, expected_component, len);
                if(new_array && !env->ExceptionCheck())
                {
                    // Copy elements, using Array.get/set for type-safe element transfer
                    for(jint j = 0; j < len; j++)
                    {
                        jobject elem = env->CallStaticObjectMethod(g_cls_array, g_mid_array_get, arg, j);
                        if(env->ExceptionCheck()) { env->ExceptionClear(); break; }
                        env->CallStaticVoidMethod(g_cls_array, g_mid_array_set, new_array, j, elem);
                        if(elem) env->DeleteLocalRef(elem);
                        if(env->ExceptionCheck()) { env->ExceptionClear(); break; }
                    }
                    // Replace the argument in the args array
                    env->SetObjectArrayElement(args, i, new_array);
                    env->DeleteLocalRef(new_array);
                }
                else if(env->ExceptionCheck())
                {
                    env->ExceptionClear();
                }
                if(expected_component) env->DeleteLocalRef(expected_component);
            }

            env->DeleteLocalRef(expected_type);
            env->DeleteLocalRef(arg);
        }

        env->DeleteLocalRef(param_types);
    }

    jobject invoke_method(JNIEnv* env, jobject method, jobject instance, jobjectArray args)
    {
        // Coerce arguments to match declared parameter types before invoking
        coerce_args_to_param_types(env, method, args, false);

        jobject res = env->CallObjectMethod(method, g_mid_method_invoke, instance, args);
        return res;
    }

    jobject invoke_constructor(JNIEnv* env, jobject ctor, jobjectArray args)
    {
        jobject res = env->CallObjectMethod(ctor, g_mid_ctor_newInstance, args);
        return res;
    }

    jobject field_get_value(JNIEnv* env, jobject field, jobject instance)
    {
        jobject res = env->CallObjectMethod(field, g_mid_field_get, instance);
        return res;
    }

    void field_set_value(JNIEnv* env, jobject field, jobject instance, jobject value)
    {
        env->CallVoidMethod(field, g_mid_field_set, instance, value);
    }

    jobjectArray build_args_array(JNIEnv* env, const std::vector<jobject>& args)
    {
        jobjectArray arr = env->NewObjectArray(static_cast<jsize>(args.size()), g_cls_object, nullptr);

        for(jsize i = 0; i < static_cast<jsize>(args.size()); i++)
        {
            env->SetObjectArrayElement(arr, i, args[static_cast<size_t>(i)]);
        }

        return arr;
    }

    struct entity_context
    {
        std::string module_path;
        std::string class_name;
        std::string member_name;
        bool is_callable = false;
        bool is_constructor = false;
        bool is_getter = false;
        bool is_setter = false;
        bool instance_required = false;
        bool use_direct_call = false;
        jobject member = nullptr; // global ref to Method/Constructor/Field
        jvm_context direct_ctx{};
        std::vector<metaffi_type_info> params_types;
        std::vector<metaffi_type_info> retvals_types;
    };

    enum class jni_ret_type
    {
        void_type,
        boolean_type,
        byte_type,
        short_type,
        int_type,
        long_type,
        float_type,
        double_type,
        char_type,
        object_type
    };

    jni_ret_type get_ret_type(const std::vector<metaffi_type_info>& retvals)
    {
        if(retvals.empty())
        {
            return jni_ret_type::void_type;
        }

        if(retvals.size() > 1)
        {
            return jni_ret_type::object_type;
        }

        metaffi_type type = retvals[0].type;
        if(is_array_type(type))
        {
            return jni_ret_type::object_type;
        }

        switch(type)
        {
            case metaffi_bool_type: return jni_ret_type::boolean_type;
            case metaffi_int8_type: return jni_ret_type::byte_type;
            case metaffi_uint8_type: return jni_ret_type::short_type;
            case metaffi_int16_type: return jni_ret_type::short_type;
            case metaffi_uint16_type: return jni_ret_type::int_type;
            case metaffi_int32_type: return jni_ret_type::int_type;
            case metaffi_uint32_type: return jni_ret_type::long_type;
            case metaffi_int64_type: return jni_ret_type::long_type;
            case metaffi_uint64_type: return jni_ret_type::object_type;
            case metaffi_size_type: return jni_ret_type::long_type;
            case metaffi_float32_type: return jni_ret_type::float_type;
            case metaffi_float64_type: return jni_ret_type::double_type;
            case metaffi_char8_type:
            case metaffi_char16_type:
            case metaffi_char32_type:
                return jni_ret_type::char_type;
            case metaffi_string8_type:
            case metaffi_string16_type:
            case metaffi_string32_type:
            case metaffi_handle_type:
            case metaffi_callable_type:
            case metaffi_any_type:
            case metaffi_null_type:
                return jni_ret_type::object_type;
            default:
                return jni_ret_type::object_type;
        }
    }
    jobject convert_any_to_object(JNIEnv* env, cdts_jvm_serializer& ser)
    {
        metaffi_type actual = ser.peek_type();
        if(actual == metaffi_null_type)
        {
            ser.set_index(ser.get_index() + 1);
            return nullptr;
        }

        if(is_array_type(actual))
        {
            return ser.extract_array();
        }

        switch(actual)
        {
            case metaffi_bool_type:
                return box_boolean(env, ser.extract_boolean());
            case metaffi_int8_type:
                return box_byte(env, ser.extract_byte());
            case metaffi_uint8_type:
            {
                jbyte b = ser.extract_byte();
                jshort s = static_cast<jshort>(static_cast<uint8_t>(b));
                return box_short(env, s);
            }
            case metaffi_int16_type:
                return box_short(env, ser.extract_short());
            case metaffi_uint16_type:
            {
                jshort s = ser.extract_short();
                jint i = static_cast<jint>(static_cast<uint16_t>(s));
                return box_int(env, i);
            }
            case metaffi_int32_type:
                return box_int(env, ser.extract_int());
            case metaffi_uint32_type:
            {
                jint i = ser.extract_int();
                jlong l = static_cast<jlong>(static_cast<uint32_t>(i));
                return box_long(env, l);
            }
            case metaffi_int64_type:
                return box_long(env, ser.extract_long());
            case metaffi_uint64_type:
            {
                jlong v = ser.extract_long();
                return create_big_integer(env, static_cast<uint64_t>(v));
            }
            case metaffi_size_type:
                return box_long(env, ser.extract_long());
            case metaffi_float32_type:
                return box_float(env, ser.extract_float());
            case metaffi_float64_type:
                return box_double(env, ser.extract_double());
            case metaffi_char8_type:
            case metaffi_char16_type:
            case metaffi_char32_type:
                return box_char(env, ser.extract_char());
            case metaffi_string8_type:
            case metaffi_string16_type:
            case metaffi_string32_type:
                return ser.extract_string();
            case metaffi_handle_type:
            case metaffi_callable_type:
                return ser.extract_handle();
            default:
                return ser.extract_handle();
        }
    }

    jobject convert_param_to_object(JNIEnv* env, cdts_jvm_serializer& ser, const metaffi_type_info& type_info)
    {
        metaffi_type type = type_info.type;
        if(is_array_type(type))
        {
            return ser.extract_array(type_info);
        }

        switch(type)
        {
            case metaffi_any_type:
                return convert_any_to_object(env, ser);
            case metaffi_bool_type:
                return box_boolean(env, ser.extract_boolean());
            case metaffi_int8_type:
                return box_byte(env, ser.extract_byte());
            case metaffi_uint8_type:
            {
                jbyte b = ser.extract_byte();
                jshort s = static_cast<jshort>(static_cast<uint8_t>(b));
                return box_short(env, s);
            }
            case metaffi_int16_type:
                return box_short(env, ser.extract_short());
            case metaffi_uint16_type:
            {
                jshort s = ser.extract_short();
                jint i = static_cast<jint>(static_cast<uint16_t>(s));
                return box_int(env, i);
            }
            case metaffi_int32_type:
                return box_int(env, ser.extract_int());
            case metaffi_uint32_type:
            {
                jint i = ser.extract_int();
                jlong l = static_cast<jlong>(static_cast<uint32_t>(i));
                return box_long(env, l);
            }
            case metaffi_int64_type:
                return box_long(env, ser.extract_long());
            case metaffi_uint64_type:
            {
                jlong v = ser.extract_long();
                return create_big_integer(env, static_cast<uint64_t>(v));
            }
            case metaffi_size_type:
                return box_long(env, ser.extract_long());
            case metaffi_float32_type:
                return box_float(env, ser.extract_float());
            case metaffi_float64_type:
                return box_double(env, ser.extract_double());
            case metaffi_char8_type:
            case metaffi_char16_type:
            case metaffi_char32_type:
                return box_char(env, ser.extract_char());
            case metaffi_string8_type:
            case metaffi_string16_type:
            case metaffi_string32_type:
                return ser.extract_string();
            case metaffi_handle_type:
            case metaffi_callable_type:
                return ser.extract_handle();
            case metaffi_null_type:
                ser.set_index(ser.get_index() + 1);
                return nullptr;
            default:
                return ser.extract_handle();
        }
    }

    struct jvalue_with_sig
    {
        jvalue value{};
        char sig = 'L';
    };

    jvalue_with_sig convert_param_to_jvalue(JNIEnv* env, cdts_jvm_serializer& ser, const metaffi_type_info& type_info)
    {
        jvalue_with_sig out;
        metaffi_type type = type_info.type;

        if(is_array_type(type))
        {
            out.value.l = ser.extract_array(type_info);
            out.sig = 'L';
            return out;
        }

        switch(type)
        {
            case metaffi_any_type:
                out.value.l = convert_any_to_object(env, ser);
                out.sig = 'L';
                return out;
            case metaffi_bool_type:
                out.value.z = ser.extract_boolean();
                out.sig = 'Z';
                return out;
            case metaffi_int8_type:
                out.value.b = ser.extract_byte();
                out.sig = 'B';
                return out;
            case metaffi_uint8_type:
            {
                jbyte b = ser.extract_byte();
                out.value.s = static_cast<jshort>(static_cast<uint8_t>(b));
                out.sig = 'S';
                return out;
            }
            case metaffi_int16_type:
                out.value.s = ser.extract_short();
                out.sig = 'S';
                return out;
            case metaffi_uint16_type:
            {
                jshort s = ser.extract_short();
                out.value.i = static_cast<jint>(static_cast<uint16_t>(s));
                out.sig = 'I';
                return out;
            }
            case metaffi_int32_type:
                out.value.i = ser.extract_int();
                out.sig = 'I';
                return out;
            case metaffi_uint32_type:
            {
                jint i = ser.extract_int();
                out.value.j = static_cast<jlong>(static_cast<uint32_t>(i));
                out.sig = 'J';
                return out;
            }
            case metaffi_int64_type:
                out.value.j = ser.extract_long();
                out.sig = 'J';
                return out;
            case metaffi_uint64_type:
                out.value.l = create_big_integer(env, static_cast<uint64_t>(ser.extract_long()));
                out.sig = 'L';
                return out;
            case metaffi_size_type:
                out.value.j = ser.extract_long();
                out.sig = 'J';
                return out;
            case metaffi_float32_type:
                out.value.f = ser.extract_float();
                out.sig = 'F';
                return out;
            case metaffi_float64_type:
                out.value.d = ser.extract_double();
                out.sig = 'D';
                return out;
            case metaffi_char8_type:
            case metaffi_char16_type:
            case metaffi_char32_type:
                out.value.c = ser.extract_char();
                out.sig = 'C';
                return out;
            case metaffi_string8_type:
            case metaffi_string16_type:
            case metaffi_string32_type:
                out.value.l = ser.extract_string();
                out.sig = 'L';
                return out;
            case metaffi_handle_type:
            case metaffi_callable_type:
                out.value.l = ser.extract_handle();
                out.sig = 'L';
                return out;
            case metaffi_null_type:
                ser.set_index(ser.get_index() + 1);
                out.value.l = nullptr;
                out.sig = 'L';
                return out;
            default:
                out.value.l = ser.extract_handle();
                out.sig = 'L';
                return out;
        }
    }

    void store_return_value_from_object(JNIEnv* env, cdts_jvm_serializer& ser, const metaffi_type_info& type_info, jobject obj)
    {
        if(!obj)
        {
            ser.null();
            return;
        }

        metaffi_type type = type_info.type;
        if(metaffi_is_packed_array(type))
        {
            metaffi_type elem_type = metaffi_packed_element_type(type);
            ser.add_packed_array((jarray)obj, elem_type);
            return;
        }
        if(is_array_type(type))
        {
            int dims = type_info.fixed_dimensions > 0 ? static_cast<int>(type_info.fixed_dimensions) : get_array_dimensions(env, (jarray)obj);
            ser.add_array((jarray)obj, dims, base_type(type));
            return;
        }

        switch(type)
        {
            case metaffi_any_type:
                ser << obj;
                return;
            case metaffi_bool_type:
                ser.add(boolean_value(env, obj), metaffi_bool_type);
                return;
            case metaffi_int8_type:
                ser.add(static_cast<jbyte>(number_long_value(env, obj)), metaffi_int8_type);
                return;
            case metaffi_uint8_type:
            {
                jlong v = number_long_value(env, obj);
                jbyte b = static_cast<jbyte>(static_cast<uint8_t>(v));
                ser.add(b, metaffi_uint8_type);
                return;
            }
            case metaffi_int16_type:
                ser.add(static_cast<jshort>(number_long_value(env, obj)), metaffi_int16_type);
                return;
            case metaffi_uint16_type:
            {
                jlong v = number_long_value(env, obj);
                jshort s = static_cast<jshort>(static_cast<uint16_t>(v));
                ser.add(s, metaffi_uint16_type);
                return;
            }
            case metaffi_int32_type:
                ser.add(static_cast<jint>(number_long_value(env, obj)), metaffi_int32_type);
                return;
            case metaffi_uint32_type:
            {
                jlong v = number_long_value(env, obj);
                jint i = static_cast<jint>(static_cast<uint32_t>(v));
                ser.add(i, metaffi_uint32_type);
                return;
            }
            case metaffi_int64_type:
                ser.add(static_cast<jlong>(number_long_value(env, obj)), metaffi_int64_type);
                return;
            case metaffi_uint64_type:
            {
                uint64_t v = is_number(env, obj) ? static_cast<uint64_t>(number_long_value(env, obj)) : big_integer_to_uint64(env, obj);
                ser.add(static_cast<jlong>(v), metaffi_uint64_type);
                return;
            }
            case metaffi_size_type:
                ser.add(static_cast<jlong>(number_long_value(env, obj)), metaffi_size_type);
                return;
            case metaffi_float32_type:
                ser.add(static_cast<jfloat>(number_double_value(env, obj)), metaffi_float32_type);
                return;
            case metaffi_float64_type:
                ser.add(static_cast<jdouble>(number_double_value(env, obj)), metaffi_float64_type);
                return;
            case metaffi_char8_type:
            case metaffi_char16_type:
            case metaffi_char32_type:
                ser.add(char_value(env, obj), type);
                return;
            case metaffi_string8_type:
            case metaffi_string16_type:
            case metaffi_string32_type:
                ser.add((jstring)obj, type);
                return;
            case metaffi_handle_type:
                ser.add_handle(obj);
                return;
            case metaffi_callable_type:
                ser.add_callable(obj);
                return;
            case metaffi_null_type:
                ser.null();
                return;
            default:
                ser << obj;
                return;
        }
    }

    void store_return_value_from_jvalue(JNIEnv* env, cdts_jvm_serializer& ser, const metaffi_type_info& type_info, jvalue val, char sig)
    {
        if(sig == 'L')
        {
            store_return_value_from_object(env, ser, type_info, val.l);
            return;
        }

        switch(type_info.type)
        {
            case metaffi_bool_type:
                ser.add(val.z, metaffi_bool_type);
                return;
            case metaffi_int8_type:
                ser.add(val.b, metaffi_int8_type);
                return;
            case metaffi_uint8_type:
                ser.add(static_cast<jbyte>(static_cast<uint8_t>(val.s)), metaffi_uint8_type);
                return;
            case metaffi_int16_type:
                ser.add(val.s, metaffi_int16_type);
                return;
            case metaffi_uint16_type:
                ser.add(static_cast<jshort>(static_cast<uint16_t>(val.i)), metaffi_uint16_type);
                return;
            case metaffi_int32_type:
                ser.add(val.i, metaffi_int32_type);
                return;
            case metaffi_uint32_type:
                ser.add(static_cast<jint>(static_cast<uint32_t>(val.j)), metaffi_uint32_type);
                return;
            case metaffi_int64_type:
                ser.add(val.j, metaffi_int64_type);
                return;
            case metaffi_uint64_type:
                ser.add(static_cast<jlong>(val.j), metaffi_uint64_type);
                return;
            case metaffi_size_type:
                ser.add(static_cast<jlong>(val.j), metaffi_size_type);
                return;
            case metaffi_float32_type:
                ser.add(val.f, metaffi_float32_type);
                return;
            case metaffi_float64_type:
                ser.add(val.d, metaffi_float64_type);
                return;
            case metaffi_char8_type:
            case metaffi_char16_type:
            case metaffi_char32_type:
                ser.add(val.c, type_info.type);
                return;
            default:
                store_return_value_from_object(env, ser, type_info, val.l);
                return;
        }
    }

    void store_multiple_return_values(JNIEnv* env, cdts_jvm_serializer& ser, const std::vector<metaffi_type_info>& retvals, jobject wrapper)
    {
        if(!wrapper)
        {
            throw std::runtime_error("Expected wrapper object for multiple return values");
        }

        jclass cls = env->GetObjectClass(wrapper);
        jmethodID get_fields = env->GetMethodID(cls, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
        jobjectArray fields = (jobjectArray)env->CallObjectMethod(cls, get_fields);
        env->DeleteLocalRef(cls);

        if(!fields)
        {
            throw std::runtime_error("Failed to get declared fields from wrapper");
        }

        jclass field_cls = env->FindClass("java/lang/reflect/Field");
        jmethodID get_mods = env->GetMethodID(field_cls, "getModifiers", "()I");
        jclass modifier_cls = env->FindClass("java/lang/reflect/Modifier");
        jmethodID is_static = env->GetStaticMethodID(modifier_cls, "isStatic", "(I)Z");

        std::vector<jobject> instance_fields;
        jsize count = env->GetArrayLength(fields);
        for(jsize i = 0; i < count; i++)
        {
            jobject field = env->GetObjectArrayElement(fields, i);
            jint mods = env->CallIntMethod(field, get_mods);
            jboolean static_flag = env->CallStaticBooleanMethod(modifier_cls, is_static, mods);
            if(static_flag == JNI_FALSE)
            {
                instance_fields.push_back(field);
            }
            else
            {
                env->DeleteLocalRef(field);
            }
        }

        env->DeleteLocalRef(fields);
        env->DeleteLocalRef(field_cls);
        env->DeleteLocalRef(modifier_cls);

        if(instance_fields.size() < retvals.size())
        {
            for(auto* f : instance_fields)
            {
                env->DeleteLocalRef(f);
            }
            throw std::runtime_error("Wrapper field count does not match return values");
        }

        for(size_t i = 0; i < retvals.size(); i++)
        {
            jobject field = instance_fields[i];
            set_accessible(env, field);
            jobject value = field_get_value(env, field, wrapper);
            store_return_value_from_object(env, ser, retvals[i], value);
            delete_local_ref_if_needed(env, value);
            env->DeleteLocalRef(field);
        }

        for(size_t i = retvals.size(); i < instance_fields.size(); i++)
        {
            env->DeleteLocalRef(instance_fields[i]);
        }
    }

    jvm_installed_info choose_jvm()
    {
        auto jvms = jvm_runtime_manager::detect_installed_jvms();
        if(jvms.empty())
        {
            throw std::runtime_error("No installed JVMs detected");
        }

        auto has_prefix = [](const std::string& version, const std::string& prefix)
        {
            return !version.empty() && version.rfind(prefix, 0) == 0;
        };

        const std::vector<std::string> preferred = {"22", "21", "17", "11"};
        for(const auto& pref : preferred)
        {
            for(const auto& info : jvms)
            {
                if(has_prefix(info.version, pref))
                {
                    return info;
                }
            }
        }

        return jvms.front();
    }

    void throw_if_jni_exception(JNIEnv* env, const std::string& fallback)
    {
        if(env && env->ExceptionCheck())
        {
            std::string error = get_exception_description(env);
            throw std::runtime_error(error.empty() ? fallback : error);
        }
    }

    jobject resolve_method(JNIEnv* env, jclass cls, const std::string& name, const std::vector<jclass>& param_types, bool instance_required)
    {
        jclass class_cls = env->FindClass("java/lang/Class");
        if(!class_cls)
        {
            throw_if_jni_exception(env, "Failed to find java/lang/Class");
            throw std::runtime_error("Failed to find java/lang/Class");
        }

        jmethodID get_declared_method = env->GetMethodID(class_cls, "getDeclaredMethod", "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;");
        if(!get_declared_method)
        {
            env->DeleteLocalRef(class_cls);
            throw_if_jni_exception(env, "Failed to get Class.getDeclaredMethod");
            throw std::runtime_error("Failed to get Class.getDeclaredMethod");
        }

        jobjectArray params_array = env->NewObjectArray(static_cast<jsize>(param_types.size()), class_cls, nullptr);
        if(!params_array)
        {
            env->DeleteLocalRef(class_cls);
            throw std::runtime_error("Failed to allocate parameter types array");
        }

        for(jsize i = 0; i < static_cast<jsize>(param_types.size()); i++)
        {
            env->SetObjectArrayElement(params_array, i, param_types[static_cast<size_t>(i)]);
        }

        jstring name_obj = env->NewStringUTF(name.c_str());
        jobject method = env->CallObjectMethod(cls, get_declared_method, name_obj, params_array);
        env->DeleteLocalRef(name_obj);
        env->DeleteLocalRef(params_array);

        // If exact type match failed (e.g. MetaFFI passes Object.class for a HANDLE
        // param but method expects Color/SomeClass[]/interface), fall back to matching
        // by name and parameter count. This handles opaque-handle interop where the
        // exact Java type isn't known at the MetaFFI API level.
        if(env->ExceptionCheck())
        {
            env->ExceptionClear();
            method = nullptr;

            jmethodID get_declared_methods = env->GetMethodID(class_cls, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
            if(get_declared_methods)
            {
                jobjectArray methods = (jobjectArray)env->CallObjectMethod(cls, get_declared_methods);
                if(methods && !env->ExceptionCheck())
                {
                    jclass method_cls = env->FindClass("java/lang/reflect/Method");
                    jmethodID get_name = env->GetMethodID(method_cls, "getName", "()Ljava/lang/String;");
                    jmethodID get_param_types = env->GetMethodID(method_cls, "getParameterTypes", "()[Ljava/lang/Class;");

                    jsize count = env->GetArrayLength(methods);
                    for(jsize i = 0; i < count; i++)
                    {
                        jobject m = env->GetObjectArrayElement(methods, i);
                        jstring m_name = (jstring)env->CallObjectMethod(m, get_name);
                        const char* m_name_cstr = env->GetStringUTFChars(m_name, nullptr);
                        bool name_match = (name == m_name_cstr);
                        env->ReleaseStringUTFChars(m_name, m_name_cstr);
                        env->DeleteLocalRef(m_name);

                        if(name_match)
                        {
                            jobjectArray m_params = (jobjectArray)env->CallObjectMethod(m, get_param_types);
                            jsize m_param_count = m_params ? env->GetArrayLength(m_params) : 0;
                            if(m_params) env->DeleteLocalRef(m_params);

                            if(m_param_count == static_cast<jsize>(param_types.size()))
                            {
                                method = m; // Found matching method by name + param count
                                break;
                            }
                        }
                        env->DeleteLocalRef(m);
                    }
                    env->DeleteLocalRef(method_cls);
                    env->DeleteLocalRef(methods);
                }
            }
            if(env->ExceptionCheck()) env->ExceptionClear();
        }

        env->DeleteLocalRef(class_cls);

        if(!method)
        {
            throw std::runtime_error("Failed to resolve Java method: " + name);
        }

        jclass method_cls = env->FindClass("java/lang/reflect/Method");
        jmethodID get_mods = env->GetMethodID(method_cls, "getModifiers", "()I");
        jclass modifier_cls = env->FindClass("java/lang/reflect/Modifier");
        jmethodID is_static = env->GetStaticMethodID(modifier_cls, "isStatic", "(I)Z");
        if(!get_mods || !is_static)
        {
            env->DeleteLocalRef(method_cls);
            env->DeleteLocalRef(modifier_cls);
            env->DeleteLocalRef(method);
            throw_if_jni_exception(env, "Failed to inspect method modifiers");
            throw std::runtime_error("Failed to inspect method modifiers");
        }

        jint mods = env->CallIntMethod(method, get_mods);
        jboolean is_static_flag = env->CallStaticBooleanMethod(modifier_cls, is_static, mods);
        bool is_static_method = is_static_flag == JNI_TRUE;
        if(is_static_method == instance_required)
        {
            env->DeleteLocalRef(method_cls);
            env->DeleteLocalRef(modifier_cls);
            env->DeleteLocalRef(method);
            throw std::runtime_error("Java method static/instance mismatch for " + name);
        }

        env->DeleteLocalRef(method_cls);
        env->DeleteLocalRef(modifier_cls);

        set_accessible(env, method);
        return method;
    }

    jobject resolve_constructor(JNIEnv* env, jclass cls, const std::vector<jclass>& param_types)
    {
        jclass class_cls = env->FindClass("java/lang/Class");
        if(!class_cls)
        {
            throw_if_jni_exception(env, "Failed to find java/lang/Class");
            throw std::runtime_error("Failed to find java/lang/Class");
        }

        jmethodID get_declared_ctor = env->GetMethodID(class_cls, "getDeclaredConstructor", "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;");
        if(!get_declared_ctor)
        {
            env->DeleteLocalRef(class_cls);
            throw_if_jni_exception(env, "Failed to get Class.getDeclaredConstructor");
            throw std::runtime_error("Failed to get Class.getDeclaredConstructor");
        }

        jobjectArray params_array = env->NewObjectArray(static_cast<jsize>(param_types.size()), class_cls, nullptr);
        if(!params_array)
        {
            env->DeleteLocalRef(class_cls);
            throw std::runtime_error("Failed to allocate constructor parameter types array");
        }

        for(jsize i = 0; i < static_cast<jsize>(param_types.size()); i++)
        {
            env->SetObjectArrayElement(params_array, i, param_types[static_cast<size_t>(i)]);
        }

        jobject ctor = env->CallObjectMethod(cls, get_declared_ctor, params_array);
        env->DeleteLocalRef(params_array);
        env->DeleteLocalRef(class_cls);

        throw_if_jni_exception(env, "Failed to resolve Java constructor");
        if(!ctor)
        {
            throw std::runtime_error("Failed to resolve Java constructor");
        }

        set_accessible(env, ctor);
        return ctor;
    }

    jobject resolve_field(JNIEnv* env, jclass cls, const std::string& name, bool instance_required)
    {
        jclass class_cls = env->FindClass("java/lang/Class");
        if(!class_cls)
        {
            throw_if_jni_exception(env, "Failed to find java/lang/Class");
            throw std::runtime_error("Failed to find java/lang/Class");
        }

        jmethodID get_declared_field = env->GetMethodID(class_cls, "getDeclaredField", "(Ljava/lang/String;)Ljava/lang/reflect/Field;");
        if(!get_declared_field)
        {
            env->DeleteLocalRef(class_cls);
            throw_if_jni_exception(env, "Failed to get Class.getDeclaredField");
            throw std::runtime_error("Failed to get Class.getDeclaredField");
        }

        jstring name_obj = env->NewStringUTF(name.c_str());
        jobject field = env->CallObjectMethod(cls, get_declared_field, name_obj);
        env->DeleteLocalRef(name_obj);
        env->DeleteLocalRef(class_cls);

        throw_if_jni_exception(env, "Failed to resolve Java field: " + name);
        if(!field)
        {
            throw std::runtime_error("Failed to resolve Java field: " + name);
        }

        jclass field_cls = env->FindClass("java/lang/reflect/Field");
        jmethodID get_mods = env->GetMethodID(field_cls, "getModifiers", "()I");
        jclass modifier_cls = env->FindClass("java/lang/reflect/Modifier");
        jmethodID is_static = env->GetStaticMethodID(modifier_cls, "isStatic", "(I)Z");
        if(!get_mods || !is_static)
        {
            env->DeleteLocalRef(field_cls);
            env->DeleteLocalRef(modifier_cls);
            env->DeleteLocalRef(field);
            throw_if_jni_exception(env, "Failed to inspect field modifiers");
            throw std::runtime_error("Failed to inspect field modifiers");
        }

        jint mods = env->CallIntMethod(field, get_mods);
        jboolean is_static_flag = env->CallStaticBooleanMethod(modifier_cls, is_static, mods);
        bool is_static_field = is_static_flag == JNI_TRUE;
        if(is_static_field == instance_required)
        {
            env->DeleteLocalRef(field_cls);
            env->DeleteLocalRef(modifier_cls);
            env->DeleteLocalRef(field);
            throw std::runtime_error("Java field static/instance mismatch for " + name);
        }

        env->DeleteLocalRef(field_cls);
        env->DeleteLocalRef(modifier_cls);

        set_accessible(env, field);
        return field;
    }

    char ret_sig_from_type(jni_ret_type type)
    {
        switch(type)
        {
            case jni_ret_type::boolean_type: return 'Z';
            case jni_ret_type::byte_type: return 'B';
            case jni_ret_type::short_type: return 'S';
            case jni_ret_type::int_type: return 'I';
            case jni_ret_type::long_type: return 'J';
            case jni_ret_type::float_type: return 'F';
            case jni_ret_type::double_type: return 'D';
            case jni_ret_type::char_type: return 'C';
            case jni_ret_type::object_type: return 'L';
            case jni_ret_type::void_type:
            default:
                return 'V';
        }
    }

    void invoke_direct_call(entity_context* ctx, JNIEnv* env, cdts_jvm_serializer* params_ser, cdts_jvm_serializer* ret_ser)
    {
        if(!ctx)
        {
            throw std::runtime_error("Context is null");
        }

        size_t param_count = ctx->params_types.size();
        size_t param_offset = ctx->direct_ctx.instance_required ? 1 : 0;
        if(param_offset > param_count)
        {
            throw std::runtime_error("Instance parameter is missing");
        }

        jobject instance = nullptr;
        if(ctx->direct_ctx.instance_required)
        {
            if(!params_ser)
            {
                throw std::runtime_error("Parameters are required for instance call");
            }
            instance = convert_param_to_object(env, *params_ser, ctx->params_types[0]);
            if(instance && jni_metaffi_handle::is_metaffi_handle_wrapper_object(env, instance))
            {
                delete_local_ref_if_needed(env, instance);
                throw std::runtime_error("Instance is not a JVM object");
            }
        }

        std::vector<jvalue_with_sig> args;
        std::vector<jvalue> jargs;
        if(param_count > param_offset)
        {
            args.reserve(param_count - param_offset);
            jargs.reserve(param_count - param_offset);
        }

        for(size_t i = param_offset; i < param_count; i++)
        {
            if(!params_ser)
            {
                throw std::runtime_error("Parameters are missing");
            }
            jvalue_with_sig v = convert_param_to_jvalue(env, *params_ser, ctx->params_types[i]);
            args.push_back(v);
            jargs.push_back(v.value);
        }

        const jvalue* argv = jargs.empty() ? nullptr : jargs.data();
        jvalue result{};

        if(ctx->direct_ctx.constructor)
        {
            result.l = env->NewObjectA(ctx->direct_ctx.cls, ctx->direct_ctx.method, argv);
        }
        else
        {
            jni_ret_type ret_type = get_ret_type(ctx->retvals_types);
            if(ctx->direct_ctx.instance_required)
            {
                switch(ret_type)
                {
                    case jni_ret_type::void_type: env->CallVoidMethodA(instance, ctx->direct_ctx.method, argv); break;
                    case jni_ret_type::boolean_type: result.z = env->CallBooleanMethodA(instance, ctx->direct_ctx.method, argv); break;
                    case jni_ret_type::byte_type: result.b = env->CallByteMethodA(instance, ctx->direct_ctx.method, argv); break;
                    case jni_ret_type::short_type: result.s = env->CallShortMethodA(instance, ctx->direct_ctx.method, argv); break;
                    case jni_ret_type::int_type: result.i = env->CallIntMethodA(instance, ctx->direct_ctx.method, argv); break;
                    case jni_ret_type::long_type: result.j = env->CallLongMethodA(instance, ctx->direct_ctx.method, argv); break;
                    case jni_ret_type::float_type: result.f = env->CallFloatMethodA(instance, ctx->direct_ctx.method, argv); break;
                    case jni_ret_type::double_type: result.d = env->CallDoubleMethodA(instance, ctx->direct_ctx.method, argv); break;
                    case jni_ret_type::char_type: result.c = env->CallCharMethodA(instance, ctx->direct_ctx.method, argv); break;
                    case jni_ret_type::object_type: result.l = env->CallObjectMethodA(instance, ctx->direct_ctx.method, argv); break;
                }
            }
            else
            {
                switch(ret_type)
                {
                    case jni_ret_type::void_type: env->CallStaticVoidMethodA(ctx->direct_ctx.cls, ctx->direct_ctx.method, argv); break;
                    case jni_ret_type::boolean_type: result.z = env->CallStaticBooleanMethodA(ctx->direct_ctx.cls, ctx->direct_ctx.method, argv); break;
                    case jni_ret_type::byte_type: result.b = env->CallStaticByteMethodA(ctx->direct_ctx.cls, ctx->direct_ctx.method, argv); break;
                    case jni_ret_type::short_type: result.s = env->CallStaticShortMethodA(ctx->direct_ctx.cls, ctx->direct_ctx.method, argv); break;
                    case jni_ret_type::int_type: result.i = env->CallStaticIntMethodA(ctx->direct_ctx.cls, ctx->direct_ctx.method, argv); break;
                    case jni_ret_type::long_type: result.j = env->CallStaticLongMethodA(ctx->direct_ctx.cls, ctx->direct_ctx.method, argv); break;
                    case jni_ret_type::float_type: result.f = env->CallStaticFloatMethodA(ctx->direct_ctx.cls, ctx->direct_ctx.method, argv); break;
                    case jni_ret_type::double_type: result.d = env->CallStaticDoubleMethodA(ctx->direct_ctx.cls, ctx->direct_ctx.method, argv); break;
                    case jni_ret_type::char_type: result.c = env->CallStaticCharMethodA(ctx->direct_ctx.cls, ctx->direct_ctx.method, argv); break;
                    case jni_ret_type::object_type: result.l = env->CallStaticObjectMethodA(ctx->direct_ctx.cls, ctx->direct_ctx.method, argv); break;
                }
            }
        }

        throw_if_jni_exception(env, "Failed to invoke Java method");

        if(ret_ser)
        {
            size_t ret_count = ctx->retvals_types.size();
            if(ret_count == 1)
            {
                jni_ret_type ret_type = get_ret_type(ctx->retvals_types);
                char sig = ret_sig_from_type(ret_type);
                store_return_value_from_jvalue(env, *ret_ser, ctx->retvals_types[0], result, sig);
            }
            else if(ret_count > 1)
            {
                store_multiple_return_values(env, *ret_ser, ctx->retvals_types, result.l);
            }
        }

        for(const auto& arg : args)
        {
            if(arg.sig == 'L')
            {
                delete_local_ref_if_needed(env, arg.value.l);
            }
        }

        delete_local_ref_if_needed(env, instance);
        if(ret_ser && ctx->retvals_types.size() >= 1 && ret_sig_from_type(get_ret_type(ctx->retvals_types)) == 'L')
        {
            delete_local_ref_if_needed(env, result.l);
        }
    }

    void invoke_reflection_call(entity_context* ctx, JNIEnv* env, cdts_jvm_serializer* params_ser, cdts_jvm_serializer* ret_ser)
    {
        if(!ctx)
        {
            throw std::runtime_error("Context is null");
        }

        size_t param_count = ctx->params_types.size();
        size_t param_offset = ctx->instance_required ? 1 : 0;
        if(param_offset > param_count)
        {
            throw std::runtime_error("Instance parameter is missing");
        }

        jobject instance = nullptr;
        if(ctx->instance_required)
        {
            if(!params_ser)
            {
                throw std::runtime_error("Parameters are required for instance call");
            }
            instance = convert_param_to_object(env, *params_ser, ctx->params_types[0]);
            if(instance && jni_metaffi_handle::is_metaffi_handle_wrapper_object(env, instance))
            {
                delete_local_ref_if_needed(env, instance);
                throw std::runtime_error("Instance is not a JVM object");
            }
        }

        if(ctx->is_callable)
        {
            size_t effective_args = param_count - param_offset;

            jobject result = nullptr;
            jobjectArray args_array = nullptr;

            if(effective_args == 0 && !ctx->is_constructor)
            {
                // Fast path: no arguments — call Method.invoke(instance, null) directly.
                // Passing null for the args array is legal in Java reflection.
                result = env->CallObjectMethod(ctx->member, g_mid_method_invoke,
                                               ctx->instance_required ? instance : nullptr,
                                               (jobject)nullptr);
            }
            else
            {
                std::vector<jobject> args;
                if(effective_args > 0)
                {
                    args.reserve(effective_args);
                }

                for(size_t i = param_offset; i < param_count; i++)
                {
                    if(!params_ser)
                    {
                        throw std::runtime_error("Parameters are missing");
                    }
                    args.push_back(convert_param_to_object(env, *params_ser, ctx->params_types[i]));
                }

                args_array = build_args_array(env, args);

                if(ctx->is_constructor)
                {
                    result = invoke_constructor(env, ctx->member, args_array);
                }
                else
                {
                    result = invoke_method(env, ctx->member, ctx->instance_required ? instance : nullptr, args_array);
                }

                for(jobject arg : args)
                {
                    delete_local_ref_if_needed(env, arg);
                }
            }

            if(args_array) env->DeleteLocalRef(args_array);
            throw_if_jni_exception(env, "Failed to invoke Java method");

            if(ret_ser)
            {
                size_t ret_count = ctx->retvals_types.size();
                if(ret_count == 1)
                {
                    store_return_value_from_object(env, *ret_ser, ctx->retvals_types[0], result);
                }
                else if(ret_count > 1)
                {
                    store_multiple_return_values(env, *ret_ser, ctx->retvals_types, result);
                }
            }

            delete_local_ref_if_needed(env, result);
        }
        else if(ctx->is_getter || ctx->is_setter)
        {
            if(ctx->is_getter)
            {
                if(!ret_ser)
                {
                    throw std::runtime_error("Return values are required for getter");
                }
                jobject value = field_get_value(env, ctx->member, ctx->instance_required ? instance : nullptr);
                throw_if_jni_exception(env, "Failed to read Java field");

                size_t ret_count = ctx->retvals_types.size();
                if(ret_count == 1)
                {
                    store_return_value_from_object(env, *ret_ser, ctx->retvals_types[0], value);
                }
                else if(ret_count > 1)
                {
                    store_multiple_return_values(env, *ret_ser, ctx->retvals_types, value);
                }

                delete_local_ref_if_needed(env, value);
            }
            else
            {
                if(param_count < param_offset + 1)
                {
                    throw std::runtime_error("Setter is missing value parameter");
                }
                if(!params_ser)
                {
                    throw std::runtime_error("Parameters are missing");
                }
                jobject value = convert_param_to_object(env, *params_ser, ctx->params_types[param_offset]);
                field_set_value(env, ctx->member, ctx->instance_required ? instance : nullptr, value);
                throw_if_jni_exception(env, "Failed to write Java field");
                delete_local_ref_if_needed(env, value);
            }
        }
        else
        {
            throw std::runtime_error("Unknown entity type");
        }

        delete_local_ref_if_needed(env, instance);
    }
}

static std::shared_ptr<jvm_runtime_manager> g_runtime_manager;
static std::mutex g_runtime_mutex;

static void jvmxcall(entity_context* ctx, cdts* params, cdts* ret, char** out_err)
{
    clear_error(out_err);
    if(!ctx)
    {
        set_error(out_err, "Context is null");
        return;
    }

    if(!g_runtime_manager || !g_runtime_manager->is_runtime_loaded())
    {
        set_error(out_err, "JVM runtime is not loaded");
        return;
    }

    if(!params && !ctx->params_types.empty())
    {
        set_error(out_err, "Parameters are required but missing");
        return;
    }

    if(!ret && !ctx->retvals_types.empty())
    {
        set_error(out_err, "Return values are required but missing");
        return;
    }

    JNIEnv* env = nullptr;
    bool env_needs_release = g_runtime_manager->get_env(&env);

    // Ensure reflection cache is initialized (no-op after first call)
    init_jni_reflection_cache(env);

    try
    {
        if(!params && !ret)
        {
            // Fast path: no params and no return values — skip serializer construction entirely.
            if(ctx->use_direct_call)
            {
                invoke_direct_call(ctx, env, nullptr, nullptr);
            }
            else
            {
                invoke_reflection_call(ctx, env, nullptr, nullptr);
            }
        }
        else
        {
            jobject class_loader = jni_class_loader::get_child_class_loader();

            std::unique_ptr<cdts_jvm_serializer> params_ser;
            std::unique_ptr<cdts_jvm_serializer> ret_ser;
            if(params)
            {
                params_ser = std::make_unique<cdts_jvm_serializer>(env, *params, class_loader);
            }
            if(ret)
            {
                ret_ser = std::make_unique<cdts_jvm_serializer>(env, *ret, class_loader);
            }

            if(ctx->use_direct_call)
            {
                invoke_direct_call(ctx, env, params_ser.get(), ret_ser.get());
            }
            else
            {
                invoke_reflection_call(ctx, env, params_ser.get(), ret_ser.get());
            }
        }
    }
    catch(const std::exception& e)
    {
        set_error(out_err, e.what());
    }

    if(env_needs_release)
    {
        g_runtime_manager->release_env();
    }
}

void load_runtime(char** err)
{
    clear_error(err);
    std::lock_guard<std::mutex> lock(g_runtime_mutex);

    if(g_runtime_manager && g_runtime_manager->is_runtime_loaded())
    {
        return;
    }

    try
    {
        trace("jvm_runtime: load_runtime start");
        auto info = choose_jvm();
        trace("jvm_runtime: choose_jvm ok");
        g_runtime_manager = std::make_shared<jvm_runtime_manager>(info);
        trace("jvm_runtime: manager created");
        g_runtime_manager->load_runtime();
        trace("jvm_runtime: load_runtime done");

        // Initialize JNI reflection cache now that JVM is running
        JNIEnv* env = nullptr;
        bool env_needs_release = g_runtime_manager->get_env(&env);
        init_jni_reflection_cache(env);
        if(env_needs_release) g_runtime_manager->release_env();
        trace("jvm_runtime: reflection cache initialized");
    }
    catch(const std::exception& e)
    {
        set_error(err, e.what());
    }
}

void free_runtime(char** err)
{
    clear_error(err);
    std::lock_guard<std::mutex> lock(g_runtime_mutex);

    if(!g_runtime_manager)
    {
        return;
    }

    try
    {
        g_runtime_manager->release_runtime();
        g_runtime_manager.reset();
    }
    catch(const std::exception& e)
    {
        set_error(err, e.what());
    }
}

static void jvmxcall_params_ret(entity_context* ctx, cdts params_ret[2], char** out_err)
{
    jvmxcall(ctx, &params_ret[0], &params_ret[1], out_err);
}

static void jvmxcall_params_no_ret(entity_context* ctx, cdts parameters[1], char** out_err)
{
    jvmxcall(ctx, &parameters[0], nullptr, out_err);
}

static void jvmxcall_no_params_ret(entity_context* ctx, cdts return_values[1], char** out_err)
{
    jvmxcall(ctx, nullptr, &return_values[0], out_err);
}

static void jvmxcall_no_params_no_ret(entity_context* ctx, char** out_err)
{
    jvmxcall(ctx, nullptr, nullptr, out_err);
}

// IMPORTANT: the name of the function must be different from xcall_* to avoid symbol conflicts.
static void jvm_api_xcall_params_ret(void* context, cdts params_ret[2], char** out_err)
{
    auto* ctx = static_cast<entity_context*>(context);
    jvmxcall_params_ret(ctx, params_ret, out_err);
}

static void jvm_api_xcall_params_no_ret(void* context, cdts parameters[1], char** out_err)
{
    auto* ctx = static_cast<entity_context*>(context);
    jvmxcall_params_no_ret(ctx, parameters, out_err);
}

static void jvm_api_xcall_no_params_ret(void* context, cdts return_values[1], char** out_err)
{
    auto* ctx = static_cast<entity_context*>(context);
    jvmxcall_no_params_ret(ctx, return_values, out_err);
}

static void jvm_api_xcall_no_params_no_ret(void* context, char** out_err)
{
    auto* ctx = static_cast<entity_context*>(context);
    jvmxcall_no_params_no_ret(ctx, out_err);
}

// Forward declaration — actual implementation that does JNI work
static xcall* load_entity_impl(const char* module_path, const char* entity_path, metaffi_type_info* params_types, int8_t params_count, metaffi_type_info* retvals_types, int8_t retval_count, char** err);

#ifdef _WIN32
// Workaround for Go's VEH intercepting JVM's internal SEH exceptions.
// load_entity does JNI class/method lookups that can trigger JVM null-check
// SEH exceptions. Running on a dedicated Windows thread avoids the conflict.
// See: https://github.com/golang/go/issues/47576
//      https://github.com/golang/go/issues/58542
struct load_entity_thread_ctx
{
    const char* module_path;
    const char* entity_path;
    metaffi_type_info* params_types;
    int8_t params_count;
    metaffi_type_info* retvals_types;
    int8_t retval_count;
    char** err;
    xcall* result;
};

static DWORD WINAPI load_entity_thread_proc(LPVOID param)
{
    auto* ctx = static_cast<load_entity_thread_ctx*>(param);
    ctx->result = load_entity_impl(
        ctx->module_path, ctx->entity_path,
        ctx->params_types, ctx->params_count,
        ctx->retvals_types, ctx->retval_count,
        ctx->err);
    return 0;
}
#endif

xcall* load_entity(const char* module_path, const char* entity_path, metaffi_type_info* params_types, int8_t params_count, metaffi_type_info* retvals_types, int8_t retval_count, char** err)
{
#ifdef _WIN32
    load_entity_thread_ctx ctx{module_path, entity_path, params_types, params_count, retvals_types, retval_count, err, nullptr};
    HANDLE hThread = CreateThread(nullptr, 0, load_entity_thread_proc, &ctx, 0, nullptr);
    if(!hThread)
    {
        clear_error(err);
        std::string msg = "CreateThread for load_entity failed: " + std::to_string(GetLastError());
        set_error(err, msg.c_str());
        return nullptr;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);
    return ctx.result;
#else
    return load_entity_impl(module_path, entity_path, params_types, params_count, retvals_types, retval_count, err);
#endif
}

static xcall* load_entity_impl(const char* module_path, const char* entity_path, metaffi_type_info* params_types, int8_t params_count, metaffi_type_info* retvals_types, int8_t retval_count, char** err)
{
    clear_error(err);

    if(!entity_path || !*entity_path)
    {
        set_error(err, "Entity path is empty");
        return nullptr;
    }

    if(params_count > 0 && !params_types)
    {
        set_error(err, "Parameter types are missing");
        return nullptr;
    }

    if(retval_count > 0 && !retvals_types)
    {
        set_error(err, "Return types are missing");
        return nullptr;
    }

    if(!g_runtime_manager || !g_runtime_manager->is_runtime_loaded())
    {
        load_runtime(err);
        if(err && *err)
        {
            return nullptr;
        }
    }

    try
    {
        auto ctx = std::make_unique<entity_context>();
        if(params_types && params_count > 0)
        {
            ctx->params_types.assign(params_types, params_types + params_count);
        }
        if(retvals_types && retval_count > 0)
        {
            ctx->retvals_types.assign(retvals_types, retvals_types + retval_count);
        }

        metaffi::utils::entity_path_parser fp(entity_path);
        if(!fp.contains("class"))
        {
            set_error(err, "Entity path must include class");
            return nullptr;
        }

        ctx->instance_required = fp.contains("instance_required");

        JNIEnv* env = nullptr;
        bool env_needs_release = g_runtime_manager->get_env(&env);
        metaffi::utils::scope_guard env_guard([&](){ if(env_needs_release) g_runtime_manager->release_env(); });

        std::string module = module_path ? module_path : "";
        jni_class_loader loader(env, module);
        std::string class_name = fp["class"];
        jclass cls = load_class_with_fallback(loader, class_name);

        if(fp.contains("callable"))
        {
            std::string callable = fp["callable"];
            ctx->is_callable = true;
            ctx->is_constructor = (callable == "<init>");

            if(ctx->is_constructor && ctx->instance_required)
            {
                throw std::runtime_error("Constructor cannot require instance");
            }
            if(ctx->is_constructor && ctx->retvals_types.size() != 1)
            {
                throw std::runtime_error("Constructor must return exactly one value");
            }

            size_t param_offset = ctx->instance_required ? 1 : 0;
            if(ctx->params_types.size() < param_offset)
            {
                throw std::runtime_error("Instance parameter is missing");
            }

            std::vector<jclass> param_classes;
            for(size_t i = param_offset; i < ctx->params_types.size(); i++)
            {
                param_classes.push_back(resolve_jclass(env, ctx->params_types[i], &loader));
            }

            jobject member = nullptr;
            if(ctx->is_constructor)
            {
                member = resolve_constructor(env, cls, param_classes);
            }
            else
            {
                member = resolve_method(env, cls, callable, param_classes, ctx->instance_required);
            }

            for(jclass pc : param_classes)
            {
                delete_local_ref_if_needed(env, pc);
            }

            ctx->member = env->NewGlobalRef(member);
            env->DeleteLocalRef(member);
            if(!ctx->member)
            {
                throw std::runtime_error("Failed to create global reference for method");
            }
        }
        else if(fp.contains("field"))
        {
            std::string field_name = fp["field"];
            bool is_getter = fp.contains("getter");
            bool is_setter = fp.contains("setter");
            if(is_getter == is_setter)
            {
                throw std::runtime_error("Field entity must be getter or setter");
            }
            ctx->is_getter = is_getter;
            ctx->is_setter = is_setter;

            if(is_getter)
            {
                size_t expected_params = ctx->instance_required ? 1 : 0;
                if(ctx->params_types.size() != expected_params)
                {
                    throw std::runtime_error("Getter parameter count mismatch");
                }
                if(ctx->retvals_types.empty())
                {
                    throw std::runtime_error("Getter requires a return value");
                }
            }
            else
            {
                size_t expected_params = ctx->instance_required ? 2 : 1;
                if(ctx->params_types.size() != expected_params)
                {
                    throw std::runtime_error("Setter parameter count mismatch");
                }
                if(!ctx->retvals_types.empty())
                {
                    throw std::runtime_error("Setter must not define return values");
                }
            }

            jobject field = resolve_field(env, cls, field_name, ctx->instance_required);
            ctx->member = env->NewGlobalRef(field);
            env->DeleteLocalRef(field);
            if(!ctx->member)
            {
                throw std::runtime_error("Failed to create global reference for field");
            }
        }
        else
        {
            throw std::runtime_error("Entity path must contain callable or field");
        }

        delete_local_ref_if_needed(env, cls);

        void* xcall_func = ctx->params_types.empty() && ctx->retvals_types.empty() ? (void*)jvm_api_xcall_no_params_no_ret
                             : ctx->params_types.empty() ? (void*)jvm_api_xcall_no_params_ret
                             : ctx->retvals_types.empty() ? (void*)jvm_api_xcall_params_no_ret
                             : (void*)jvm_api_xcall_params_ret;

        xcall* pxcall = new xcall(xcall_func, ctx.release());
        return pxcall;
    }
    catch(const std::exception& e)
    {
        set_error(err, e.what());
        return nullptr;
    }
}

xcall* make_callable(void* make_callable_context, metaffi_type_info* params_types, int8_t params_count, metaffi_type_info* retvals_types, int8_t retval_count, char** err)
{
    clear_error(err);

    if(!make_callable_context)
    {
        set_error(err, "make_callable context is null");
        return nullptr;
    }

    if(params_count > 0 && !params_types)
    {
        set_error(err, "Parameter types are missing");
        return nullptr;
    }

    if(retval_count > 0 && !retvals_types)
    {
        set_error(err, "Return types are missing");
        return nullptr;
    }

    if(!g_runtime_manager || !g_runtime_manager->is_runtime_loaded())
    {
        load_runtime(err);
        if(err && *err)
        {
            return nullptr;
        }
    }

    try
    {
        auto ctx = std::make_unique<entity_context>();
        if(params_types && params_count > 0)
        {
            ctx->params_types.assign(params_types, params_types + params_count);
        }
        if(retvals_types && retval_count > 0)
        {
            ctx->retvals_types.assign(retvals_types, retvals_types + retval_count);
        }

        std::unique_ptr<jvm_context> pctxt(static_cast<jvm_context*>(make_callable_context));
        if(!pctxt || !pctxt->method)
        {
            throw std::runtime_error("Method ID is null");
        }

        JNIEnv* env = nullptr;
        bool env_needs_release = g_runtime_manager->get_env(&env);
        metaffi::utils::scope_guard env_guard([&](){ if(env_needs_release) g_runtime_manager->release_env(); });

        ctx->use_direct_call = true;
        ctx->direct_ctx = *pctxt;
        if(!ctx->direct_ctx.cls)
        {
            throw std::runtime_error("Declaring class is null");
        }
        size_t param_offset = ctx->direct_ctx.instance_required ? 1 : 0;
        if(ctx->params_types.size() < param_offset)
        {
            throw std::runtime_error("Instance parameter is missing");
        }

        jclass global_cls = (jclass)env->NewGlobalRef(ctx->direct_ctx.cls);
        if(!global_cls)
        {
            throw std::runtime_error("Failed to create global reference for declaring class");
        }
        ctx->direct_ctx.cls = global_cls;

        void* xcall_func = ctx->params_types.empty() && ctx->retvals_types.empty() ? (void*)jvm_api_xcall_no_params_no_ret
                             : ctx->params_types.empty() ? (void*)jvm_api_xcall_no_params_ret
                             : ctx->retvals_types.empty() ? (void*)jvm_api_xcall_params_no_ret
                             : (void*)jvm_api_xcall_params_ret;

        xcall* pxcall = new xcall(xcall_func, ctx.release());
        return pxcall;
    }
    catch(const std::exception& e)
    {
        set_error(err, e.what());
        return nullptr;
    }
}

void free_xcall(xcall* pxcall, char** err)
{
    clear_error(err);
    if(!pxcall)
    {
        return;
    }

    entity_context* ctx = static_cast<entity_context*>(pxcall->pxcall_and_context[1]);
    if(ctx)
    {
        if(g_runtime_manager && g_runtime_manager->is_runtime_loaded())
        {
            JNIEnv* env = nullptr;
            bool env_needs_release = g_runtime_manager->get_env(&env);
            metaffi::utils::scope_guard env_guard([&](){ if(env_needs_release) g_runtime_manager->release_env(); });

            if(ctx->member)
            {
                env->DeleteGlobalRef(ctx->member);
                ctx->member = nullptr;
            }
            if(ctx->direct_ctx.cls)
            {
                env->DeleteGlobalRef(ctx->direct_ctx.cls);
                ctx->direct_ctx.cls = nullptr;
            }
        }

        delete ctx;
    }

    delete pxcall;
}
