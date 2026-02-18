#include <doctest/doctest.h>

#include "jvm_test_env.h"
#include "jvm_wrappers.h"

#include <runtime/cdt.h>

#include <string>
#include <variant>
#include <vector>

namespace
{
std::string string_value_of(const JvmHandle& obj)
{
	auto& env = jvm_test_env();
	auto value_of = env.guest_module.load_entity(
		"class=java.lang.String,callable=valueOf",
		{metaffi_handle_type},
		{metaffi_string8_type});
	auto [result] = value_of.call<std::string>(*obj.get());
	return result;
}
}

TEST_CASE("array functions")
{
	auto& env = jvm_test_env();
	trace_step("array functions: start");

	std::vector<metaffi::api::MetaFFITypeInfo> bytes2d = {make_array_type(metaffi_int8_array_type, 2)};
	trace_step("array functions: load getThreeBuffers");
	auto get_buffers = env.guest_module.load_entity_with_info(
		"class=guest.ArrayFunctions,callable=getThreeBuffers",
		{},
		bytes2d);
	trace_step("array functions: call getThreeBuffers");
	auto [buffers] = get_buffers.call<std::vector<std::vector<int8_t>>>();
	trace_step("array functions: got getThreeBuffers");
	REQUIRE(buffers.size() == 3);
	CHECK(buffers[0] == std::vector<int8_t>({1, 2, 3, 4}));
	CHECK(buffers[1] == std::vector<int8_t>({5, 6, 7}));
	CHECK(buffers[2] == std::vector<int8_t>({8, 9}));

	trace_step("array functions: load expectThreeBuffers");
	auto expect_buffers = env.guest_module.load_entity_with_info(
		"class=guest.ArrayFunctions,callable=expectThreeBuffers",
		bytes2d,
		{});
	{
		trace_step("array functions: call expectThreeBuffers");
		cdts params(1);
		params[0].set_new_array(buffers.size(), 2, metaffi_int8_type);
		cdts& outer = static_cast<cdts&>(params[0]);
		for(size_t i = 0; i < buffers.size(); ++i)
		{
			const auto& row = buffers[i];
			outer[i].set_new_array(row.size(), 1, metaffi_int8_type);
			cdts& inner = static_cast<cdts&>(outer[i]);
			for(size_t j = 0; j < row.size(); ++j)
			{
				inner[j] = row[j];
			}
		}
		CHECK_NOTHROW(expect_buffers.call_raw(std::move(params)));
		trace_step("array functions: done expectThreeBuffers");
	}

	std::vector<metaffi::api::MetaFFITypeInfo> ints2d = {make_array_type(metaffi_int32_array_type, 2)};
	trace_step("array functions: load make2dArray");
	auto make2d = env.guest_module.load_entity_with_info(
		"class=guest.ArrayFunctions,callable=make2dArray",
		{},
		ints2d);
	trace_step("array functions: call make2dArray");
	auto [arr2d] = make2d.call<std::vector<std::vector<int32_t>>>();
	trace_step("array functions: got make2dArray");
	REQUIRE(arr2d.size() == 2);
	CHECK(arr2d[0] == std::vector<int32_t>({1, 2}));
	CHECK(arr2d[1] == std::vector<int32_t>({3, 4}));

	std::vector<metaffi::api::MetaFFITypeInfo> ints3d = {make_array_type(metaffi_int32_array_type, 3)};
	trace_step("array functions: load make3dArray");
	auto make3d = env.guest_module.load_entity_with_info(
		"class=guest.ArrayFunctions,callable=make3dArray",
		{},
		ints3d);
	trace_step("array functions: call make3dArray");
	auto [arr3d] = make3d.call<std::vector<std::vector<std::vector<int32_t>>>>();
	trace_step("array functions: got make3dArray");
	REQUIRE(arr3d.size() == 2);
	CHECK(arr3d[0][0][0] == 1);
	CHECK(arr3d[1][1][0] == 4);

	trace_step("array functions: load makeRaggedArray");
	auto make_ragged = env.guest_module.load_entity_with_info(
		"class=guest.ArrayFunctions,callable=makeRaggedArray",
		{},
		ints2d);
	trace_step("array functions: call makeRaggedArray");
	auto [ragged] = make_ragged.call<std::vector<std::vector<int32_t>>>();
	trace_step("array functions: got makeRaggedArray");
	REQUIRE(ragged.size() == 3);
	CHECK(ragged[0] == std::vector<int32_t>({1, 2, 3}));
	CHECK(ragged[1] == std::vector<int32_t>({4}));
	CHECK(ragged[2] == std::vector<int32_t>({5, 6}));

	trace_step("array functions: load sum3dArrayFromFactory");
	auto sum3d = env.guest_module.load_entity_with_info(
		"class=guest.ArrayFunctions,callable=sum3dArrayFromFactory",
		{},
		{make_type(metaffi_int32_type)});
	trace_step("array functions: call sum3dArrayFromFactory");
	auto [sum3d_val] = sum3d.call<int32_t>();
	trace_step("array functions: got sum3dArrayFromFactory");
	CHECK(sum3d_val == 10);

	trace_step("array functions: load sumRaggedArray");
	auto sum_ragged = env.guest_module.load_entity_with_info(
		"class=guest.ArrayFunctions,callable=sumRaggedArray",
		ints2d,
		{make_type(metaffi_int32_type)});
	trace_step("array functions: call sumRaggedArray");
	auto [sum_ragged_val] = sum_ragged.call<int32_t>(ragged);
	trace_step("array functions: got sumRaggedArray");
	CHECK(sum_ragged_val == 21);

	trace_step("array functions: load getSomeClasses");
	auto get_classes = env.guest_module.load_entity_with_info(
		"class=guest.ArrayFunctions,callable=getSomeClasses",
		{},
		{make_alias_type(metaffi_handle_array_type, "guest.SomeClass", 1)});
	trace_step("array functions: call getSomeClasses");
	cdts classes_ret = get_classes.call_cdts();
	trace_step("array functions: got getSomeClasses");
	cdts& classes_array = static_cast<cdts&>(classes_ret[0]);
	REQUIRE(classes_array.length == 3);

	auto get_name = env.guest_module.load_entity(
		"class=guest.SomeClass,callable=getName,instance_required",
		{metaffi_handle_type},
		{metaffi_string8_type});
	for(metaffi_size i = 0; i < classes_array.length; ++i)
	{
		cdt& item = classes_array[i];
		REQUIRE(item.type == metaffi_handle_type);
		REQUIRE(item.cdt_val.handle_val != nullptr);
		cdt_metaffi_handle handle = *item.cdt_val.handle_val;
		auto [name] = get_name.call<std::string>(handle);
		CHECK(name == "some");
	}

	auto expect_classes = env.guest_module.load_entity_with_info(
		"class=guest.ArrayFunctions,callable=expectThreeSomeClasses",
		{make_alias_type(metaffi_handle_array_type, "guest.SomeClass", 1)},
		{});
	trace_step("array functions: call expectThreeSomeClasses");
	cdts params(1);
	params[0].set_new_array(classes_array.length, 1, metaffi_handle_type);
	cdts& params_array = static_cast<cdts&>(params[0]);
	for(metaffi_size i = 0; i < classes_array.length; ++i)
	{
		params_array[i].set_handle(classes_array[i].cdt_val.handle_val);
	}
	CHECK_NOTHROW(expect_classes.call_raw(std::move(params)));
	trace_step("array functions: done");
}

TEST_CASE("packed array: sum 1d int array")
{
	auto& env = jvm_test_env();
	trace_step("packed array sum: start");

	auto sum1d = env.guest_module.load_entity_with_info(
		"class=guest.ArrayFunctions,callable=sumInt1dArray",
		{make_array_type(metaffi_int32_packed_array_type, 1)},
		{make_type(metaffi_int32_type)});
	trace_step("packed array sum: loaded");

	std::vector<int32_t> arr = {1, 2, 3, 4, 5};
	auto [sum_val] = sum1d.call<int32_t>(arr);
	trace_step("packed array sum: called");
	CHECK(sum_val == 15);
}

TEST_CASE("packed array: echo 1d long array")
{
	auto& env = jvm_test_env();
	trace_step("packed array echo long: start");

	auto echo = env.guest_module.load_entity_with_info(
		"class=guest.ArrayFunctions,callable=echoLong1dArray",
		{make_array_type(metaffi_int64_packed_array_type, 1)},
		{make_array_type(metaffi_int64_packed_array_type, 1)});
	trace_step("packed array echo long: loaded");

	std::vector<int64_t> arr = {100, 200, 300};
	auto [result] = echo.call<std::vector<int64_t>>(arr);
	trace_step("packed array echo long: called");
	CHECK(result == std::vector<int64_t>({100, 200, 300}));
}

TEST_CASE("packed array: echo 1d double array")
{
	auto& env = jvm_test_env();
	trace_step("packed array echo double: start");

	auto echo = env.guest_module.load_entity_with_info(
		"class=guest.ArrayFunctions,callable=echoDouble1dArray",
		{make_array_type(metaffi_float64_packed_array_type, 1)},
		{make_array_type(metaffi_float64_packed_array_type, 1)});
	trace_step("packed array echo double: loaded");

	std::vector<double> arr = {1.5, 2.5, 3.5};
	auto [result] = echo.call<std::vector<double>>(arr);
	trace_step("packed array echo double: called");
	REQUIRE(result.size() == 3);
	CHECK(result[0] == doctest::Approx(1.5));
	CHECK(result[1] == doctest::Approx(2.5));
	CHECK(result[2] == doctest::Approx(3.5));
}

TEST_CASE("packed array: make 1d int array")
{
	auto& env = jvm_test_env();
	trace_step("packed array make: start");

	auto make1d = env.guest_module.load_entity_with_info(
		"class=guest.ArrayFunctions,callable=makeInt1dArray",
		{},
		{make_array_type(metaffi_int32_packed_array_type, 1)});
	trace_step("packed array make: loaded");

	auto [arr] = make1d.call<std::vector<int32_t>>();
	trace_step("packed array make: called");
	CHECK(arr == std::vector<int32_t>({10, 20, 30, 40, 50}));
}

TEST_CASE("core returns arrays")
{
	auto& env = jvm_test_env();

	auto ret_multiple = env.guest_module.load_entity_with_info(
		"class=guest.CoreFunctions,callable=returnMultipleReturnValues",
		{},
		{make_alias_type(metaffi_handle_type, "java.lang.Object[]")});
	auto [multi_ptr] = ret_multiple.call<cdt_metaffi_handle*>();
	JavaArray multi{JvmHandle(multi_ptr)};
	REQUIRE(multi.length() == 6);

	auto elem0 = multi.get_handle(0);
	CHECK(java_class_name(elem0) == "java.lang.Integer");
	CHECK(string_value_of(elem0) == "1");

	auto elem1 = multi.get_handle(1);
	CHECK(java_class_name(elem1) == "java.lang.String");
	CHECK(string_value_of(elem1) == "string");

	auto elem2 = multi.get_handle(2);
	CHECK(java_class_name(elem2) == "java.lang.Double");
	CHECK(string_value_of(elem2) == "3.0");

	auto elem3_any = multi.get_any(3);
	CHECK(std::holds_alternative<cdt_metaffi_handle>(elem3_any));
	CHECK(std::get<cdt_metaffi_handle>(elem3_any).handle == nullptr);

	auto elem4 = multi.get_handle(4);
	CHECK(java_class_name(elem4) == "[B");
	JavaArray bytes_array(std::move(elem4));
	CHECK(bytes_array.length() == 3);
	CHECK(string_value_of(bytes_array.get_handle(0)) == "1");
	CHECK(string_value_of(bytes_array.get_handle(1)) == "2");
	CHECK(string_value_of(bytes_array.get_handle(2)) == "3");

	auto elem5 = multi.get_handle(5);
	CHECK(java_class_name(elem5) == "guest.SomeClass");

	auto ret_diff_dims = env.guest_module.load_entity_with_info(
		"class=guest.CoreFunctions,callable=returnsArrayWithDifferentDimensions",
		{},
		{make_alias_type(metaffi_handle_type, "java.lang.Object[]")});
	auto [diff_ptr] = ret_diff_dims.call<cdt_metaffi_handle*>();
	JavaArray diff{JvmHandle(diff_ptr)};
	REQUIRE(diff.length() == 3);

	auto diff0 = diff.get_handle(0);
	CHECK(java_class_name(diff0) == "[I");
	JavaArray diff0_arr(std::move(diff0));
	CHECK(diff0_arr.length() == 3);
	CHECK(string_value_of(diff0_arr.get_handle(0)) == "1");
	CHECK(string_value_of(diff0_arr.get_handle(1)) == "2");
	CHECK(string_value_of(diff0_arr.get_handle(2)) == "3");

	auto diff1 = diff.get_handle(1);
	CHECK(java_class_name(diff1) == "java.lang.Integer");
	CHECK(string_value_of(diff1) == "4");

	auto diff2 = diff.get_handle(2);
	CHECK(java_class_name(diff2) == "[[I");
	JavaArray diff2_arr(std::move(diff2));
	CHECK(diff2_arr.length() == 2);
	auto row0 = diff2_arr.get_handle(0);
	auto row1 = diff2_arr.get_handle(1);
	JavaArray row0_arr(std::move(row0));
	JavaArray row1_arr(std::move(row1));
	CHECK(row0_arr.length() == 2);
	CHECK(row1_arr.length() == 2);
	CHECK(string_value_of(row0_arr.get_handle(0)) == "5");
	CHECK(string_value_of(row0_arr.get_handle(1)) == "6");
	CHECK(string_value_of(row1_arr.get_handle(0)) == "7");
	CHECK(string_value_of(row1_arr.get_handle(1)) == "8");

	auto ret_diff_objects = env.guest_module.load_entity_with_info(
		"class=guest.CoreFunctions,callable=returnsArrayOfDifferentObjects",
		{},
		{make_alias_type(metaffi_handle_type, "java.lang.Object[]")});
	auto [diff_obj_ptr] = ret_diff_objects.call<cdt_metaffi_handle*>();
	JavaArray diff_obj{JvmHandle(diff_obj_ptr)};
	CHECK(diff_obj.length() == 6);
}
