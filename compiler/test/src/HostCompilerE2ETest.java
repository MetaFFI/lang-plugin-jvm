import api.MetaFFIRuntime;
import metaffi.api.accessor.Caller;

import javax.tools.Diagnostic;
import javax.tools.DiagnosticCollector;
import javax.tools.JavaCompiler;
import javax.tools.JavaFileObject;
import javax.tools.StandardJavaFileManager;
import javax.tools.ToolProvider;
import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.math.BigInteger;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.Objects;
import java.util.stream.Collectors;
import java.util.concurrent.TimeUnit;

public class HostCompilerE2ETest {
    private static final String MODULE_NAME = "test";
    private static final String HOST_CLASS_NAME = "host_MetaFFIHost";
    private static final String HOST_FQN = MODULE_NAME + "." + HOST_CLASS_NAME;

    private static Class<?> hostClass;
    private static Class<?> handleClass;
    private static ClassLoader hostClassLoader;

    public static void main(String[] args) throws Exception {
        runAll();
        System.out.println("HostCompilerE2ETest PASSED");
    }

    private static void runAll() throws Exception {
        startWatchdog(120);
        log("Starting HostCompilerE2ETest");
        String sourceRoot = requireEnv("METAFFI_SOURCE_ROOT");
        String metaffiHome = requireEnv("METAFFI_HOME");

        Path idlPath = Paths.get(sourceRoot, "sdk", "test_modules", "guest_modules", "test", "xllr.test.idl.json");
        requireExists(idlPath, "IDL file not found");

        Path metaffiExe = resolveMetaffiExecutable(metaffiHome);
        requireExists(metaffiExe, "metaffi executable not found");

        Path testPlugin = resolveTestPluginPath(metaffiHome);
        requireExists(testPlugin, "xllr.test plugin not found");

        Path outputBase = Paths.get(System.getProperty("user.dir"), "output");
        resetDirectory(outputBase);

        log("Running metaffi host compiler");
        runMetaffi(metaffiExe, idlPath, outputBase);
        log("metaffi host compiler completed");

        Path generatedJava = outputBase.resolve(MODULE_NAME).resolve(HOST_CLASS_NAME + ".java");
        requireExists(generatedJava, "Generated host file not found");

        Path apiJar = Paths.get(metaffiHome, "jvm", "api", "metaffi.api.jar");
        requireExists(apiJar, "metaffi.api.jar not found");

        Path classesDir = outputBase.resolve("classes");
        log("Compiling generated host sources");
        compileGeneratedSources(outputBase, classesDir, apiJar);
        log("Generated host sources compiled");

        log("Loading generated host classes");
        loadHostClasses(classesDir, apiJar);
        log("Generated host classes loaded");

        invokeStatic("bindModuleToCode", new Class[]{String.class, String.class}, "", MODULE_NAME);
        log("bindModuleToCode complete");

        testPrimitivesNoParamsNoRet();
        log("testPrimitivesNoParamsNoRet ok");
        testPrimitivesReturnOnly();
        log("testPrimitivesReturnOnly ok");
        testPrimitivesAcceptOnly();
        log("testPrimitivesAcceptOnly ok");
        testEchoFunctions();
        log("testEchoFunctions ok");
        testArithmeticFunctions();
        log("testArithmeticFunctions ok");
        testArrays();
        log("testArrays ok");
        testHandles();
        log("testHandles ok");
        testGlobalVariable();
        log("testGlobalVariable ok");
        testCallables();
        log("testCallables ok");
        testErrorHandling();
        log("testErrorHandling ok");
        testAnyType();
        log("testAnyType ok");
        testMultipleReturnValues();
        log("testMultipleReturnValues ok");
    }

    private static void testPrimitivesNoParamsNoRet() throws Exception {
        invokeStatic("no_op", new Class[]{});
        invokeStatic("print_hello", new Class[]{});
    }

    private static void testPrimitivesReturnOnly() throws Exception {
        assertEquals((byte) 42, ((Number) invokeStatic("return_int8", new Class[]{})).byteValue(), "return_int8");
        assertEquals((short) 1000, ((Number) invokeStatic("return_int16", new Class[]{})).shortValue(), "return_int16");
        assertEquals(100000, ((Number) invokeStatic("return_int32", new Class[]{})).intValue(), "return_int32");
        assertEquals(9223372036854775807L, ((Number) invokeStatic("return_int64", new Class[]{})).longValue(), "return_int64");
        assertEquals(255L, ((Number) invokeStatic("return_uint8", new Class[]{})).longValue(), "return_uint8");
        assertEquals(65535L, ((Number) invokeStatic("return_uint16", new Class[]{})).longValue(), "return_uint16");
        assertEquals(4294967295L, ((Number) invokeStatic("return_uint32", new Class[]{})).longValue(), "return_uint32");
        Object uint64 = invokeStatic("return_uint64", new Class[]{});
        assertTrue(uint64 instanceof BigInteger, "return_uint64 should return BigInteger");
        assertEquals(new BigInteger("18446744073709551615"), uint64, "return_uint64");

        assertClose(3.14159f, ((Number) invokeStatic("return_float32", new Class[]{})).floatValue(), 1e-5f, "return_float32");
        assertClose(3.141592653589793, ((Number) invokeStatic("return_float64", new Class[]{})).doubleValue(), 1e-12, "return_float64");
        assertEquals(true, (Boolean) invokeStatic("return_bool_true", new Class[]{}), "return_bool_true");
        assertEquals(false, (Boolean) invokeStatic("return_bool_false", new Class[]{}), "return_bool_false");
        assertEquals("Hello from test plugin", invokeStatic("return_string8", new Class[]{}), "return_string8");
        assertTrue(invokeStatic("return_null", new Class[]{}) == null, "return_null should be null");
    }

    private static void testPrimitivesAcceptOnly() throws Exception {
        invokeStatic("accept_int8", new Class[]{byte.class}, (byte) 42);
        invokeStatic("accept_int16", new Class[]{short.class}, (short) 1000);
        invokeStatic("accept_int32", new Class[]{int.class}, 100000);
        invokeStatic("accept_int64", new Class[]{long.class}, 9223372036854775807L);
        invokeStatic("accept_float32", new Class[]{float.class}, 3.14159f);
        invokeStatic("accept_float64", new Class[]{double.class}, 3.141592653589793);
        invokeStatic("accept_bool", new Class[]{boolean.class}, true);
        invokeStatic("accept_bool", new Class[]{boolean.class}, false);
        invokeStatic("accept_string8", new Class[]{String.class}, "test string");
    }

    private static void testEchoFunctions() throws Exception {
        assertEquals(123L, ((Number) invokeStatic("echo_int64", new Class[]{long.class}, 123L)).longValue(), "echo_int64");
        assertClose(3.14, ((Number) invokeStatic("echo_float64", new Class[]{double.class}, 3.14)).doubleValue(), 1e-12, "echo_float64");
        assertEquals("hello", invokeStatic("echo_string8", new Class[]{String.class}, "hello"), "echo_string8");
        assertEquals(true, invokeStatic("echo_bool", new Class[]{boolean.class}, true), "echo_bool");
    }

    private static void testArithmeticFunctions() throws Exception {
        assertEquals(7L, ((Number) invokeStatic("add_int64", new Class[]{long.class, long.class}, 3L, 4L)).longValue(), "add_int64");
        assertClose(4.0, ((Number) invokeStatic("add_float64", new Class[]{double.class, double.class}, 1.5, 2.5)).doubleValue(), 1e-12, "add_float64");
        assertEquals("Hello World", invokeStatic("concat_strings", new Class[]{String.class, String.class}, "Hello", " World"), "concat_strings");
    }

    private static void testArrays() throws Exception {
        long[] arr1d = (long[]) invokeStatic("return_int64_array_1d", new Class[]{});
        assertArrayEquals(new long[]{1L, 2L, 3L}, arr1d, "return_int64_array_1d");

        long[][] arr2d = (long[][]) invokeStatic("return_int64_array_2d", new Class[]{});
        assertArrayEquals(new long[]{1L, 2L}, arr2d[0], "return_int64_array_2d[0]");
        assertArrayEquals(new long[]{3L, 4L}, arr2d[1], "return_int64_array_2d[1]");

        long[][][] arr3d = (long[][][]) invokeStatic("return_int64_array_3d", new Class[]{});
        assertArrayEquals(new long[]{1L, 2L}, arr3d[0][0], "return_int64_array_3d[0][0]");
        assertArrayEquals(new long[]{3L, 4L}, arr3d[0][1], "return_int64_array_3d[0][1]");
        assertArrayEquals(new long[]{5L, 6L}, arr3d[1][0], "return_int64_array_3d[1][0]");
        assertArrayEquals(new long[]{7L, 8L}, arr3d[1][1], "return_int64_array_3d[1][1]");

        long[][] ragged = (long[][]) invokeStatic("return_ragged_array", new Class[]{});
        assertArrayEquals(new long[]{1L, 2L, 3L}, ragged[0], "return_ragged_array[0]");
        assertArrayEquals(new long[]{4L}, ragged[1], "return_ragged_array[1]");
        assertArrayEquals(new long[]{5L, 6L}, ragged[2], "return_ragged_array[2]");

        String[] stringArray = (String[]) invokeStatic("return_string_array", new Class[]{});
        assertArrayEquals(new String[]{"one", "two", "three"}, stringArray, "return_string_array");

        long sum = ((Number) invokeStatic("sum_int64_array", new Class[]{long[].class}, (Object) new long[]{1L, 2L, 3L, 4L, 5L})).longValue();
        assertEquals(15L, sum, "sum_int64_array");

        long[] echoed = (long[]) invokeStatic("echo_int64_array", new Class[]{long[].class}, (Object) new long[]{10L, 20L, 30L});
        assertArrayEquals(new long[]{10L, 20L, 30L}, echoed, "echo_int64_array");

        String joined = (String) invokeStatic("join_strings", new Class[]{String[].class}, (Object) new String[]{"one", "two", "three"});
        assertEquals("one, two, three", joined, "join_strings");
    }

    private static void testHandles() throws Exception {
        Object handle = handleClass.getMethod("createTestHandle").invoke(null);
        assertTrue(handle != null, "createTestHandle returned null");

        long id = ((Number) handleClass.getMethod("getId").invoke(handle)).longValue();
        assertTrue(id > 0, "TestHandle id should be > 0");

        String data = (String) handleClass.getMethod("getData").invoke(handle);
        assertEquals("test_data", data, "TestHandle.getData");

        handleClass.getMethod("setData", String.class).invoke(handle, "new_value");
        String updated = (String) handleClass.getMethod("getData").invoke(handle);
        assertEquals("new_value", updated, "TestHandle.setData");

        handleClass.getMethod("append_to_data", String.class).invoke(handle, "_suffix");
        String appended = (String) handleClass.getMethod("getData").invoke(handle);
        assertEquals("new_value_suffix", appended, "TestHandle.append_to_data");

        handleClass.getMethod("close").invoke(handle);
    }

    private static void testGlobalVariable() throws Exception {
        invokeStatic("setG_name", new Class[]{String.class}, "default_name");
        assertEquals("default_name", invokeStatic("getG_name", new Class[]{}), "getG_name default");
        invokeStatic("setG_name", new Class[]{String.class}, "test_value");
        assertEquals("test_value", invokeStatic("getG_name", new Class[]{}), "setG_name");
    }

    private static void testCallables() throws Exception {
        MetaFFIRuntime jvmRuntime = new MetaFFIRuntime("jvm");
        jvmRuntime.loadRuntimePlugin();
        try {
            Method addMethod = HostCompilerE2ETest.class.getDeclaredMethod("callbackAdd", long.class, long.class);
            Caller addCaller = MetaFFIRuntime.makeMetaFFICallable(addMethod);
            long addResult = ((Number) invokeStatic("call_callback_add", new Class[]{Caller.class}, addCaller)).longValue();
            assertEquals(7L, addResult, "call_callback_add");

            Method stringMethod = HostCompilerE2ETest.class.getDeclaredMethod("callbackString", String.class);
            Caller stringCaller = MetaFFIRuntime.makeMetaFFICallable(stringMethod);
            String callbackResult = (String) invokeStatic("call_callback_string", new Class[]{Caller.class}, stringCaller);
            assertEquals("echoed: test", callbackResult, "call_callback_string");

            Caller returned = (Caller) invokeStatic("return_adder_callback", new Class[]{});
            Object[] returnedResult = returned.call(10L, 20L);
            assertTrue(returnedResult != null && returnedResult.length == 1, "return_adder_callback result shape");
            assertEquals(30L, ((Number) returnedResult[0]).longValue(), "return_adder_callback");
        } finally {
            jvmRuntime.releaseRuntimePlugin();
        }
    }

    private static void testErrorHandling() throws Exception {
        Throwable err1 = invokeExpectException("throw_error", new Class[]{});
        assertTrue(err1.getMessage() != null && err1.getMessage().contains("Test error thrown intentionally"),
                "throw_error message");

        Throwable err2 = invokeExpectException("throw_with_message", new Class[]{String.class}, "Custom error message");
        assertTrue(err2.getMessage() != null && err2.getMessage().contains("Custom error message"),
                "throw_with_message message");

        invokeStatic("error_if_negative", new Class[]{long.class}, 42L);
        Throwable err3 = invokeExpectException("error_if_negative", new Class[]{long.class}, -1L);
        assertTrue(err3.getMessage() != null && err3.getMessage().toLowerCase(Locale.ROOT).contains("negative"),
                "error_if_negative message");
    }

    private static void testAnyType() throws Exception {
        Object intRes = invokeStatic("accept_any", new Class[]{Object.class}, 42L);
        assertEquals(142L, ((Number) intRes).longValue(), "accept_any(int64)");

        Object floatRes = invokeStatic("accept_any", new Class[]{Object.class}, 3.14);
        assertClose(6.28, ((Number) floatRes).doubleValue(), 1e-9, "accept_any(float64)");

        Object strRes = invokeStatic("accept_any", new Class[]{Object.class}, "hello");
        assertEquals("echoed: hello", strRes, "accept_any(string8)");

        int[] int32Res = (int[]) invokeStatic("accept_any", new Class[]{Object.class}, (Object) new int[]{1, 2, 3});
        assertArrayEquals(new int[]{4, 5, 6}, int32Res, "accept_any(int32[])");

        long[] int64Res = (long[]) invokeStatic("accept_any", new Class[]{Object.class}, (Object) new long[]{1L, 2L, 3L});
        assertArrayEquals(new long[]{10L, 20L, 30L}, int64Res, "accept_any(int64[])");
    }

    private static void testMultipleReturnValues() throws Exception {
        Object twoValues = invokeStatic("return_two_values", new Class[]{});
        assertEquals(42L, ((Number) getField(twoValues, "num")).longValue(), "return_two_values.num");
        assertEquals("answer", getField(twoValues, "text"), "return_two_values.text");

        Object threeValues = invokeStatic("return_three_values", new Class[]{});
        assertEquals(1L, ((Number) getField(threeValues, "int_val")).longValue(), "return_three_values.int_val");
        assertClose(2.5, ((Number) getField(threeValues, "float_val")).doubleValue(), 1e-12, "return_three_values.float_val");
        assertEquals(true, getField(threeValues, "bool_val"), "return_three_values.bool_val");

        Object swapped = invokeStatic("swap_values", new Class[]{long.class, String.class}, 123L, "hello");
        assertEquals("hello", getField(swapped, "swapped_text"), "swap_values.swapped_text");
        assertEquals(123L, ((Number) getField(swapped, "swapped_num")).longValue(), "swap_values.swapped_num");
    }

    public static long callbackAdd(long a, long b) {
        return a + b;
    }

    public static String callbackString(String value) {
        return "echoed: " + value;
    }

    private static Object invokeStatic(String name, Class<?>[] paramTypes, Object... args) throws Exception {
        Method method = hostClass.getMethod(name, paramTypes);
        try {
            return method.invoke(null, args);
        } catch (InvocationTargetException e) {
            throw (e.getCause() != null) ? new RuntimeException(e.getCause()) : e;
        }
    }

    private static Throwable invokeExpectException(String name, Class<?>[] paramTypes, Object... args) throws Exception {
        Method method = hostClass.getMethod(name, paramTypes);
        try {
            method.invoke(null, args);
            throw new AssertionError("Expected exception from " + name);
        } catch (InvocationTargetException e) {
            return (e.getCause() != null) ? e.getCause() : e;
        }
    }

    private static Object getField(Object instance, String fieldName) throws Exception {
        Field field = instance.getClass().getField(fieldName);
        return field.get(instance);
    }

    private static void loadHostClasses(Path classesDir, Path apiJar) throws Exception {
        URL[] urls = new URL[]{
                classesDir.toUri().toURL(),
                apiJar.toUri().toURL()
        };
        hostClassLoader = new URLClassLoader(urls, HostCompilerE2ETest.class.getClassLoader());
        hostClass = Class.forName(HOST_FQN, true, hostClassLoader);
        handleClass = Class.forName(HOST_FQN + "$TestHandle", true, hostClassLoader);
    }

    private static void compileGeneratedSources(Path sourceRoot, Path classesDir, Path apiJar) throws IOException {
        JavaCompiler compiler = ToolProvider.getSystemJavaCompiler();
        if (compiler == null) {
            throw new IllegalStateException("JDK is required to run this test (ToolProvider.getSystemJavaCompiler() returned null)");
        }

        List<Path> javaFiles = new ArrayList<>();
        Files.walk(sourceRoot).filter(p -> p.toString().endsWith(".java")).forEach(javaFiles::add);
        if (javaFiles.isEmpty()) {
            throw new IllegalStateException("No generated Java files found under " + sourceRoot);
        }

        Files.createDirectories(classesDir);

        DiagnosticCollector<JavaFileObject> diagnostics = new DiagnosticCollector<>();
        try (StandardJavaFileManager fileManager = compiler.getStandardFileManager(diagnostics, null, null)) {
            Iterable<? extends JavaFileObject> compilationUnits =
                    fileManager.getJavaFileObjectsFromFiles(javaFiles.stream().map(Path::toFile).collect(Collectors.toList()));

            List<String> options = new ArrayList<>();
            options.add("-d");
            options.add(classesDir.toString());
            options.add("-classpath");
            options.add(apiJar.toString());

            JavaCompiler.CompilationTask task = compiler.getTask(null, fileManager, diagnostics, options, null, compilationUnits);
            Boolean ok = task.call();
            if (!Boolean.TRUE.equals(ok)) {
                StringBuilder sb = new StringBuilder("Failed to compile generated host sources:\n");
                for (Diagnostic<?> d : diagnostics.getDiagnostics()) {
                    sb.append(d.toString()).append("\n");
                }
                throw new IllegalStateException(sb.toString());
            }
        }
    }

    private static void runMetaffi(Path metaffiExe, Path idlPath, Path outputDir) throws IOException, InterruptedException {
        List<String> command = Arrays.asList(
                metaffiExe.toString(),
                "-c",
                "--idl",
                idlPath.toString(),
                "-h",
                "jvm"
        );

        ProcessBuilder pb = new ProcessBuilder(command);
        pb.directory(outputDir.toFile());
        pb.redirectErrorStream(true);
        Process proc = pb.start();
        String output = readAll(proc.getInputStream());
        boolean finished = proc.waitFor(120, TimeUnit.SECONDS);
        if (!finished) {
            proc.destroyForcibly();
            throw new IllegalStateException("metaffi timed out after 120 seconds");
        }
        int exitCode = proc.exitValue();
        if (exitCode != 0) {
            throw new IllegalStateException("metaffi failed with code " + exitCode + ":\n" + output);
        }
    }

    private static String readAll(InputStream input) throws IOException {
        return new String(input.readAllBytes(), StandardCharsets.UTF_8);
    }

    private static void resetDirectory(Path dir) throws IOException {
        if (Files.exists(dir)) {
            try (java.util.stream.Stream<Path> walk = Files.walk(dir)) {
                walk.sorted((a, b) -> b.compareTo(a)).forEach(path -> {
                    try {
                        Files.deleteIfExists(path);
                    } catch (IOException e) {
                        throw new RuntimeException("Failed to delete " + path, e);
                    }
                });
            }
        }
        Files.createDirectories(dir);
    }

    private static String requireEnv(String name) {
        String value = System.getenv(name);
        if (value == null || value.isEmpty()) {
            throw new IllegalStateException(name + " environment variable is not set");
        }
        return value;
    }

    private static void requireExists(Path path, String message) {
        if (!Files.exists(path)) {
            throw new IllegalStateException(message + ": " + path);
        }
    }

    private static Path resolveMetaffiExecutable(String metaffiHome) {
        boolean isWindows = System.getProperty("os.name").toLowerCase(Locale.ROOT).contains("win");
        String exeName = isWindows ? "metaffi.exe" : "metaffi";
        return Paths.get(metaffiHome, exeName);
    }

    private static Path resolveTestPluginPath(String metaffiHome) {
        String osName = System.getProperty("os.name").toLowerCase(Locale.ROOT);
        String libName;
        if (osName.contains("win")) {
            libName = "xllr.test.dll";
        } else if (osName.contains("mac")) {
            libName = "libxllr.test.dylib";
        } else {
            libName = "xllr.test.so";
        }
        return Paths.get(metaffiHome, "test", libName);
    }

    private static void assertTrue(boolean condition, String message) {
        if (!condition) {
            throw new AssertionError(message);
        }
    }

    private static void assertEquals(Object expected, Object actual, String message) {
        if (!Objects.equals(expected, actual)) {
            throw new AssertionError(message + ": expected=" + expected + ", actual=" + actual);
        }
    }

    private static void assertEquals(long expected, long actual, String message) {
        if (expected != actual) {
            throw new AssertionError(message + ": expected=" + expected + ", actual=" + actual);
        }
    }

    private static void assertClose(double expected, double actual, double tol, String message) {
        if (Double.isNaN(actual) || Math.abs(expected - actual) > tol) {
            throw new AssertionError(message + ": expected=" + expected + ", actual=" + actual);
        }
    }

    private static void assertClose(float expected, float actual, float tol, String message) {
        if (Float.isNaN(actual) || Math.abs(expected - actual) > tol) {
            throw new AssertionError(message + ": expected=" + expected + ", actual=" + actual);
        }
    }

    private static void assertArrayEquals(long[] expected, long[] actual, String message) {
        if (!Arrays.equals(expected, actual)) {
            throw new AssertionError(message + ": expected=" + Arrays.toString(expected) + ", actual=" + Arrays.toString(actual));
        }
    }

    private static void assertArrayEquals(int[] expected, int[] actual, String message) {
        if (!Arrays.equals(expected, actual)) {
            throw new AssertionError(message + ": expected=" + Arrays.toString(expected) + ", actual=" + Arrays.toString(actual));
        }
    }

    private static void assertArrayEquals(String[] expected, String[] actual, String message) {
        if (!Arrays.equals(expected, actual)) {
            throw new AssertionError(message + ": expected=" + Arrays.toString(expected) + ", actual=" + Arrays.toString(actual));
        }
    }

    private static void log(String message) {
        System.out.println("[HostCompilerE2ETest] " + message);
    }

    private static void startWatchdog(int seconds) {
        Thread watchdog = new Thread(() -> {
            try {
                Thread.sleep(seconds * 1000L);
            } catch (InterruptedException ignored) {
                return;
            }
            System.err.println("[HostCompilerE2ETest] Timeout after " + seconds + " seconds");
            System.exit(1);
        });
        watchdog.setDaemon(true);
        watchdog.start();
    }
}
