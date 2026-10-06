package com.lvonasek.utils;

import android.app.Activity;
import android.content.Context;
import android.os.Build;

import com.huawei.hiar.ARConfigBase;
import com.huawei.hiar.ARSession;
import com.huawei.hiar.ARWorldTrackingConfig;

/** Only compiled and packaged in the legacy scanner flavor. */
final class ARProviderFeatures {
    static final boolean LEGACY_ENABLED = true;

    static boolean isHuaweiSupported(Context context) {
        ARSession session = null;
        try {
            session = new ARSession(context);
            ARWorldTrackingConfig config = new ARWorldTrackingConfig(session);
            session.configure(config);
            return true;
        } catch (Exception e) {
            e.printStackTrace();
            return false;
        } finally {
            releaseProbeSession(session);
        }
    }

    static boolean isHuaweiToFSupported(Activity activity) {
        // Huawei Mate 20 variants, P20 Pro and P30 cannot use this depth path.
        if (Build.DEVICE.startsWith("HWHMA")) return false;
        if (Build.DEVICE.startsWith("HWLYA")) return false;
        if (Build.DEVICE.startsWith("HWEVR")) return false;
        if (Build.DEVICE.startsWith("HW-01K")) return false;
        if (Build.DEVICE.startsWith("HWCLT")) return false;
        if (Build.DEVICE.startsWith("HWELE")) return false;
        if (!Compatibility.hasToFSensor(activity)) return false;

        ARSession session = null;
        try {
            session = new ARSession(activity);
            ARWorldTrackingConfig config = new ARWorldTrackingConfig(session);
            config.setEnableItem(ARConfigBase.ENABLE_DEPTH | ARConfigBase.ENABLE_MESH);
            session.configure(config);
            return session.isSupported(config);
        } catch (Exception e) {
            e.printStackTrace();
            return false;
        } finally {
            releaseProbeSession(session);
        }
    }

    // Only locally owned, never-resumed probes. Preserve results on cleanup errors.
    private static void releaseProbeSession(ARSession session) {
        if (session != null) {
            try {
                session.stop();
            } catch (RuntimeException e) {
                e.printStackTrace();
            }
        }
    }
}
