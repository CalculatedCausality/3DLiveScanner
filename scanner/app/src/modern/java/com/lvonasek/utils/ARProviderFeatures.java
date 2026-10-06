package com.lvonasek.utils;

import android.app.Activity;
import android.content.Context;

/** Modern builds have no Huawei or retired GVR classes or native libraries. */
final class ARProviderFeatures {
    static final boolean LEGACY_ENABLED = false;

    static boolean isHuaweiSupported(Context context) {
        return false;
    }

    static boolean isHuaweiToFSupported(Activity activity) {
        return false;
    }
}
