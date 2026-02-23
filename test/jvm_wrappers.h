#pragma once

#include "jvm_test_env.h"

#include <runtime/metaffi_primitives.h>
#include <runtime/xcall.h>
#include <runtime/xllr_capi_loader.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

class JvmHandle
{
public:
	explicit JvmHandle(cdt_metaffi_handle* handle = nullptr);
	~JvmHandle();

	JvmHandle(const JvmHandle&) = delete;
	JvmHandle& operator=(const JvmHandle&) = delete;

	JvmHandle(JvmHandle&& other) noexcept;
	JvmHandle& operator=(JvmHandle&& other) noexcept;

	[[nodiscard]] cdt_metaffi_handle* get() const;
	[[nodiscard]] bool is_null() const;
	cdt_metaffi_handle* release();

private:
	cdt_metaffi_handle* _handle = nullptr;
};

std::string take_string8(metaffi_string8 value);
std::string java_object_to_string(const JvmHandle& obj);
std::string java_class_name(const JvmHandle& obj);

class JavaArray
{
public:
	explicit JavaArray(JvmHandle handle);

	[[nodiscard]] metaffi_size length() const;
	[[nodiscard]] JvmHandle get_handle(metaffi_size index) const;
	[[nodiscard]] metaffi_variant get_any(metaffi_size index) const;
	[[nodiscard]] cdt_metaffi_handle* handle() const;

private:
	JvmHandle _handle;
};

inline metaffi::api::MetaFFITypeInfo make_type(metaffi_type type)
{
	return metaffi::api::MetaFFITypeInfo(type);
}

inline metaffi::api::MetaFFITypeInfo make_array_type(metaffi_type type, metaffi_int64 fixed_dimensions)
{
	return metaffi::api::MetaFFITypeInfo(type, nullptr, false, fixed_dimensions);
}

inline metaffi::api::MetaFFITypeInfo make_alias_type(metaffi_type type, const char* alias, metaffi_int64 fixed_dimensions = MIXED_OR_UNKNOWN_DIMENSIONS)
{
	return metaffi::api::MetaFFITypeInfo(type, alias, true, fixed_dimensions);
}

inline cdt_metaffi_callable make_callable(xcall& cb_xcall,
	std::vector<metaffi_type>& params_types,
	std::vector<metaffi_type>& retval_types)
{
	cdt_metaffi_callable callable{};
	callable.val = &cb_xcall;
	callable.parameters_types = params_types.empty() ? nullptr : params_types.data();
	callable.params_types_length = static_cast<metaffi_int8>(params_types.size());
	callable.retval_types = retval_types.empty() ? nullptr : retval_types.data();
	callable.retval_types_length = static_cast<metaffi_int8>(retval_types.size());
	return callable;
}

void set_callback_error(char** out_err, const std::string& msg);
