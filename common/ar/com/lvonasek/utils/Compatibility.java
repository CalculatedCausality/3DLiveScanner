package com.lvonasek.utils;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ResolveInfo;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;

import com.google.ar.core.ArCoreApk;
import com.google.ar.core.CameraConfig;
import com.google.ar.core.Config;
import com.google.ar.core.Session;

public class Compatibility {

    public static boolean hasToFSensor(Activity activity) {
        try {
            CameraManager manager = (CameraManager) activity.getSystemService(Context.CAMERA_SERVICE);
            for (String cameraId : manager.getCameraIdList()) {
                CameraCharacteristics characteristics = manager.getCameraCharacteristics(cameraId);
                Integer facing = characteristics.get(CameraCharacteristics.LENS_FACING);
                if (facing != null) {
                    if (facing == CameraCharacteristics.LENS_FACING_BACK) {
                        int[] ch = characteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
                        if (ch != null) {
                            for (int c : ch) {
                                if (c == CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_DEPTH_OUTPUT) {
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
        return false;
    }

    public static boolean isARSupported(Context context) {
        ArCoreApk.Availability availability = ArCoreApk.getInstance().checkAvailability(context);
        if (availability == ArCoreApk.Availability.SUPPORTED_INSTALLED) {
            return true;
        }
        if (availability == ArCoreApk.Availability.SUPPORTED_APK_TOO_OLD) {
            return true;
        }
        if (availability == ArCoreApk.Availability.SUPPORTED_NOT_INSTALLED) {
            return true;
        }
        return ARProviderFeatures.isHuaweiSupported(context);
    }

    public static boolean isARCoreSupportedAndUpToDate(Activity activity) {
        // Make sure ARCore is installed and supported on this device.
        ArCoreApk.Availability availability = ArCoreApk.getInstance().checkAvailability(activity);
        switch (availability) {
            case SUPPORTED_INSTALLED:
                break;
            case SUPPORTED_APK_TOO_OLD:
            case SUPPORTED_NOT_INSTALLED:
                try {
                    // Request ARCore installation or update if needed.
                    ArCoreApk.InstallStatus installStatus =
                            ArCoreApk.getInstance().requestInstall(activity, /*userRequestedInstall=*/ true);
                    switch (installStatus) {
                        case INSTALL_REQUESTED:
                            return false;
                        case INSTALLED:
                            break;
                    }
                } catch (Exception e) {
                    return false;
                }
                break;
            case UNKNOWN_ERROR:
            case UNKNOWN_CHECKING:
            case UNKNOWN_TIMED_OUT:
            case UNSUPPORTED_DEVICE_NOT_CAPABLE:
                return false;
        }
        return true;
    }

    public static boolean isDaydreamSupported(Context context)
    {
        if (!isLegacyVRSupported()) return false;
        Intent mainIntent = new Intent(Intent.ACTION_MAIN, null);
        mainIntent.addCategory(Intent.CATEGORY_LAUNCHER);
        for (ResolveInfo info : context.getPackageManager().queryIntentActivities( mainIntent, 0))
            if (info.activityInfo.packageName.compareTo("com.google.android.vr.home") == 0)
                return true;
        return false;
    }

    public static boolean isGoogleDepthSupported(Activity activity) {
        Session session = null;
        try {
            if (!isPlayStoreSupported(activity)) {
                return false;
            }

            session = new Session(activity);
            return session.isDepthModeSupported(Config.DepthMode.RAW_DEPTH_ONLY);
        } catch (Exception e) {
            e.printStackTrace();
        } finally {
            releaseProbeSession(session);
        }
        return false;
    }

    public static boolean isGoogleToFSupported(Activity activity) {
        Session session = null;
        try {
            if (!isPlayStoreSupported(activity)) {
                return false;
            }

            session = new Session(activity);
            for (CameraConfig config : session.getSupportedCameraConfigs()) {
                if (config.getDepthSensorUsage() == CameraConfig.DepthSensorUsage.REQUIRE_AND_USE) {
                    return true;
                }
            }
        } catch (Exception e) {
            e.printStackTrace();
        } finally {
            releaseProbeSession(session);
        }
        return false;
    }

    public static boolean isHuaweiToFSupported(Activity activity) {
        return ARProviderFeatures.isHuaweiToFSupported(activity);
    }

    // Only for locally owned probe sessions, which are never resumed.
    // Cleanup failures must not change a capability result; Errors still propagate.
    private static void releaseProbeSession(Session session) {
        if (session != null) {
            try {
                session.close();
            } catch (RuntimeException e) {
                e.printStackTrace();
            }
        }
    }

    public static boolean isPlayStoreSupported(Context context)
    {
        Intent mainIntent = new Intent(Intent.ACTION_MAIN, null);
        mainIntent.addCategory(Intent.CATEGORY_LAUNCHER);
        for (ResolveInfo info : context.getPackageManager().queryIntentActivities( mainIntent, 0))
            if (info.activityInfo.packageName.compareTo("com.android.vending") == 0)
                return true;
        return false;
    }

    public static boolean shouldUseHuawei(Activity activity) {
        return ARProviderFeatures.LEGACY_ENABLED &&
                (isHuaweiToFSupported(activity) || !isPlayStoreSupported(activity));
    }

    public static boolean isLegacyVRSupported() {
        return ARProviderFeatures.LEGACY_ENABLED;
    }
}
