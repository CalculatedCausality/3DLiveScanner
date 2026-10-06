"""Host-side lifecycle regression test; requires Python 3 and a JDK (no Gradle).

Run: python3 common/ar/tests/test_compatibility_lifecycle.py
Set JAVA_HOME when java/javac are not on PATH. Small SDK doubles exercise the
actual Compatibility.java control flow; they do not validate native SDK behavior.
"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCES = {
    "android/content/Context.java": """
        package android.content;
        public class Context {
            public static final String CAMERA_SERVICE = "camera";
            public Object getSystemService(String name) {
                return new android.hardware.camera2.CameraManager();
            }
            public android.content.pm.PackageManager getPackageManager() {
                return new android.content.pm.PackageManager();
            }
        }
    """,
    "android/app/Activity.java": """
        package android.app;
        public class Activity extends android.content.Context {}
    """,
    "android/content/Intent.java": """
        package android.content;
        public class Intent {
            public static final String ACTION_MAIN = "main", CATEGORY_LAUNCHER = "launcher";
            public Intent(String action, Object uri) {}
            public void addCategory(String category) {}
        }
    """,
    "android/content/pm/ResolveInfo.java": """
        package android.content.pm;
        public class ResolveInfo {
            public ActivityInfo activityInfo = new ActivityInfo();
            public static class ActivityInfo { public String packageName = "com.android.vending"; }
        }
    """,
    "android/content/pm/PackageManager.java": """
        package android.content.pm;
        public class PackageManager {
            public java.util.List<ResolveInfo> queryIntentActivities(android.content.Intent i, int f) {
                return test.Probe.playStore ? java.util.Arrays.asList(new ResolveInfo())
                        : java.util.Collections.emptyList();
            }
        }
    """,
    "android/hardware/camera2/CameraCharacteristics.java": """
        package android.hardware.camera2;
        public class CameraCharacteristics {
            public static class Key<T> {}
            public static final Key<Integer> LENS_FACING = new Key<>();
            public static final Key<int[]> REQUEST_AVAILABLE_CAPABILITIES = new Key<>();
            public static final int LENS_FACING_BACK = 1, REQUEST_AVAILABLE_CAPABILITIES_DEPTH_OUTPUT = 8;
            @SuppressWarnings("unchecked") public <T> T get(Key<T> key) {
                return (T) (key == LENS_FACING ? (Object) Integer.valueOf(LENS_FACING_BACK)
                        : test.Probe.tof ? new int[] {8} : new int[0]);
            }
        }
    """,
    "android/hardware/camera2/CameraManager.java": """
        package android.hardware.camera2;
        public class CameraManager {
            public String[] getCameraIdList() { return new String[] {"back"}; }
            public CameraCharacteristics getCameraCharacteristics(String id) {
                return new CameraCharacteristics();
            }
        }
    """,
    "android/os/Build.java": """
        package android.os;
        public class Build { public static String DEVICE = "supported-device"; }
    """,
    "com/google/ar/core/ArCoreApk.java": """
        package com.google.ar.core;
        public class ArCoreApk {
            public enum Availability { SUPPORTED_INSTALLED, SUPPORTED_APK_TOO_OLD,
                SUPPORTED_NOT_INSTALLED, UNKNOWN_ERROR, UNKNOWN_CHECKING, UNKNOWN_TIMED_OUT,
                UNSUPPORTED_DEVICE_NOT_CAPABLE }
            public enum InstallStatus { INSTALL_REQUESTED, INSTALLED }
            public static Availability availability = Availability.UNSUPPORTED_DEVICE_NOT_CAPABLE;
            public static ArCoreApk getInstance() { return new ArCoreApk(); }
            public Availability checkAvailability(android.content.Context context) { return availability; }
            public InstallStatus requestInstall(android.app.Activity a, boolean requested) {
                return InstallStatus.INSTALLED;
            }
        }
    """,
    "com/google/ar/core/Config.java": """
        package com.google.ar.core;
        public class Config { public enum DepthMode { RAW_DEPTH_ONLY } }
    """,
    "com/google/ar/core/CameraConfig.java": """
        package com.google.ar.core;
        public class CameraConfig {
            public enum DepthSensorUsage { REQUIRE_AND_USE, DO_NOT_USE }
            public DepthSensorUsage getDepthSensorUsage() {
                test.Probe.fault("cameraConfig");
                return test.Probe.supported ? DepthSensorUsage.REQUIRE_AND_USE : DepthSensorUsage.DO_NOT_USE;
            }
        }
    """,
    "com/google/ar/core/Session.java": """
        package com.google.ar.core;
        public class Session extends test.Probe {
            public Session(android.content.Context c) { super(); }
            public boolean isDepthModeSupported(Config.DepthMode mode) { fault("query"); return supported; }
            public java.util.List<CameraConfig> getSupportedCameraConfigs() {
                fault("query"); return java.util.Arrays.asList(new CameraConfig());
            }
            public void close() { release(); }
        }
    """,
    "com/huawei/hiar/ARConfigBase.java": """
        package com.huawei.hiar;
        public class ARConfigBase {
            public static final long ENABLE_DEPTH = 1, ENABLE_MESH = 2;
            public void setEnableItem(long flags) { test.Probe.fault("enableItem"); }
        }
    """,
    "com/huawei/hiar/ARWorldTrackingConfig.java": """
        package com.huawei.hiar;
        public class ARWorldTrackingConfig extends ARConfigBase {
            public ARWorldTrackingConfig(ARSession s) { test.Probe.fault("configConstructor"); }
        }
    """,
    "com/huawei/hiar/ARSession.java": """
        package com.huawei.hiar;
        public class ARSession extends test.Probe {
            public ARSession(android.content.Context c) { super(); }
            public void configure(ARConfigBase config) { fault("configure"); }
            public boolean isSupported(ARConfigBase config) { fault("query"); return supported; }
            public void stop() { release(); }
        }
    """,
    "test/Probe.java": """
        package test;
        public class Probe {
            public static int created, released;
            public static boolean supported, playStore, tof;
            public static String failure;
            public static Throwable problem, cleanupProblem;
            public boolean closed;
            public Probe() { fault("constructor"); created++; }
            public static void raise(Throwable t) {
                if (t instanceof Error) throw (Error) t;
                if (t instanceof RuntimeException) throw (RuntimeException) t;
            }
            public static void fault(String stage) { if (stage.equals(failure)) raise(problem); }
            public void release() {
                if (closed) throw new AssertionError("double release");
                closed = true; released++; raise(cleanupProblem);
            }
            public static void reset() {
                created = released = 0;
                supported = playStore = tof = true;
                failure = ""; problem = cleanupProblem = null;
                android.os.Build.DEVICE = "supported-device";
                com.google.ar.core.ArCoreApk.availability =
                    com.google.ar.core.ArCoreApk.Availability.UNSUPPORTED_DEVICE_NOT_CAPABLE;
            }
        }
    """,
    "test/LifecycleTest.java": """
        package test;
        import com.lvonasek.utils.Compatibility;
        import com.google.ar.core.ArCoreApk;
        import java.util.function.BooleanSupplier;
        public class LifecycleTest {
            static int assertions;
            static void check(boolean condition, String message) {
                assertions++;
                if (!condition) throw new AssertionError(message);
            }
            static void counts(int created, int released) {
                check(Probe.created == created, "session creation count");
                check(Probe.released == released, "session release count");
            }
            static void expectError(BooleanSupplier call, Error error) {
                try { call.getAsBoolean(); throw new AssertionError("Error swallowed"); }
                catch (Error actual) { check(actual == error, "original Error must propagate"); }
            }
            public static void main(String[] args) {
                android.app.Activity activity = new android.app.Activity();
                BooleanSupplier[] probes = {
                    () -> Compatibility.isGoogleDepthSupported(activity),
                    () -> Compatibility.isGoogleToFSupported(activity),
                    () -> Compatibility.isARSupported(activity),
                    () -> Compatibility.isHuaweiToFSupported(activity)
                };
                String[][] stages = {
                    {"constructor", "query"},
                    {"constructor", "query", "cameraConfig"},
                    {"constructor", "configConstructor", "configure"},
                    {"constructor", "configConstructor", "enableItem", "configure", "query"}
                };
                for (int i = 0; i < probes.length; i++) {
                    BooleanSupplier call = probes[i];
                    Probe.reset();
                    // A separately held live session must never be released by a probe.
                    Probe live = new Probe();
                    check(call.getAsBoolean(), "supported"); counts(2, 1);
                    check(!live.closed, "live session untouched");
                    Probe.supported = false;
                    check(call.getAsBoolean() == (i == 2), "false result/AR configure semantics");
                    counts(3, 2); // Fresh probe on each call, no stale cached capability.
                    for (String stage : stages[i]) {
                        for (boolean fatal : new boolean[] {false, true}) {
                            Probe.reset(); Probe.failure = stage;
                            Probe.problem = fatal ? new LinkageError(stage) : new IllegalStateException(stage);
                            if (fatal) expectError(call, (Error) Probe.problem);
                            else check(!call.getAsBoolean(), "failed probe returns false");
                            int count = stage.equals("constructor") ? 0 : 1;
                            counts(count, count);
                        }
                    }
                    for (boolean supported : new boolean[] {false, true}) {
                        Probe.reset(); Probe.supported = supported;
                        Probe.cleanupProblem = new IllegalStateException("cleanup");
                        check(call.getAsBoolean() == (supported || i == 2), "cleanup preserves result");
                        counts(1, 1);
                    }
                    Probe.reset(); Probe.failure = stages[i][1];
                    Probe.problem = new LinkageError("probe");
                    Probe.cleanupProblem = new IllegalStateException("cleanup");
                    expectError(call, (Error) Probe.problem); counts(1, 1);
                    Probe.reset(); Probe.cleanupProblem = new LinkageError("cleanup");
                    expectError(call, (Error) Probe.cleanupProblem); counts(1, 1);
                }
                for (int i = 0; i < 2; i++) {
                    Probe.reset(); Probe.playStore = false;
                    check(!probes[i].getAsBoolean(), "no Play Store"); counts(0, 0);
                }
                for (ArCoreApk.Availability a : new ArCoreApk.Availability[] {
                    ArCoreApk.Availability.SUPPORTED_INSTALLED,
                    ArCoreApk.Availability.SUPPORTED_APK_TOO_OLD,
                    ArCoreApk.Availability.SUPPORTED_NOT_INSTALLED}) {
                    Probe.reset(); ArCoreApk.availability = a;
                    check(probes[2].getAsBoolean(), "ARCore fast path"); counts(0, 0);
                }
                Probe.reset(); Probe.tof = false;
                check(!probes[3].getAsBoolean(), "no hardware ToF"); counts(0, 0);
                for (String device : new String[] {"HWHMA", "HWLYA", "HWEVR", "HW-01K", "HWCLT", "HWELE"}) {
                    Probe.reset(); android.os.Build.DEVICE = device;
                    check(!probes[3].getAsBoolean(), "blacklisted device"); counts(0, 0);
                }
                System.out.println("PASS: " + assertions + " lifecycle assertions");
            }
        }
    """,
}


class CompatibilityLifecycleTest(unittest.TestCase):
    def test_probe_lifecycle(self):
        self.compile_and_run("legacy")

    def test_modern_without_legacy_sdk(self):
        self.compile_and_run("modern")

    def compile_and_run(self, flavor):
        java_home = os.environ.get("JAVA_HOME")
        javac = str(Path(java_home) / "bin/javac") if java_home else "javac"
        java = str(Path(java_home) / "bin/java") if java_home else "java"
        compiler = [javac] if shutil.which(javac) else [java, "-m", "jdk.compiler/com.sun.tools.javac.Main"]
        helper = Path(__file__).resolve().parents[1] / "com/lvonasek/utils/Compatibility.java"
        provider = Path(__file__).resolve().parents[3] / (
            "scanner/app/src/" + flavor + "/java/com/lvonasek/utils/ARProviderFeatures.java")
        with tempfile.TemporaryDirectory(prefix="compatibility-lifecycle-") as directory:
            root = Path(directory)
            sources = []
            for name, content in SOURCES.items():
                if flavor == "modern":
                    # No Huawei doubles at compile time OR runtime in this test.
                    if name.startswith("com/huawei/"):
                        continue
                    if name == "test/LifecycleTest.java":
                        content = """
                            package test;
                            import com.lvonasek.utils.Compatibility;
                            import com.google.ar.core.ArCoreApk;
                            public class LifecycleTest {
                                public static void main(String[] args) {
                                    android.app.Activity activity = new android.app.Activity();
                                    Probe.reset();
                                    Probe.playStore = false;
                                    if (Compatibility.shouldUseHuawei(activity) ||
                                        Compatibility.isHuaweiToFSupported(activity) ||
                                        Compatibility.isARSupported(activity) ||
                                        Compatibility.isLegacyVRSupported() ||
                                        Compatibility.isDaydreamSupported(activity) ||
                                        Probe.created != 0)
                                        throw new AssertionError("modern tried a legacy provider");
                                    ArCoreApk.availability = ArCoreApk.Availability.SUPPORTED_INSTALLED;
                                    if (!Compatibility.isARSupported(activity))
                                        throw new AssertionError("modern ARCore support lost");
                                    Probe.playStore = true;
                                    if (!Compatibility.isGoogleDepthSupported(activity) ||
                                        Probe.created != 1 || Probe.released != 1)
                                        throw new AssertionError("modern probe cleanup lost");
                                    System.out.println("PASS: modern provider isolation without Huawei SDK");
                                }
                            }
                        """
                source = root / name
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_text(content)
                sources.append(str(source))
            compiled = subprocess.run(
                [*compiler, "-source", "8", "-target", "8", "-d", str(root), str(helper), str(provider), *sources],
                capture_output=True, text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run(
                [java, "-cp", str(root), "test.LifecycleTest"], capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())


if __name__ == "__main__":
    unittest.main()
