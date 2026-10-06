package com.lvonasek.arcore3dscanner.main;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Color;
import android.os.BatteryManager;
import android.os.Process;
import android.text.TextUtils;
import android.text.method.ScrollingMovementMethod;
import android.util.Log;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.lvonasek.arcore3dscanner.ui.AbstractActivity;

import java.io.File;

public class Indicators implements Runnable {

    private static final long MEMORY_SAFETY_MARGIN = 128L * 1024L * 1024L;
    private static final long PROCESS_MEMORY_LIMIT = 1536L * 1024L * 1024L;
    private static final long STORAGE_SAFETY_RESERVE = 1024L * 1024L * 1024L;

    private ActivityManager mActivityManager;
    private ActivityManager.MemoryInfo mMemoryInfo;
    private Main mMain;
    private File mScanStorage;
    private String mOverrideMessage;
    private String mEventText = "";
    private volatile boolean mRunning;
    private int mCriticalSamples;
    private int mStorageCheck;
    private boolean mProcessMemoryCritical;
    private boolean mStorageCritical;
    private long mProcessPss;
    private long mUsableStorage;

    private LinearLayout mLayoutInfo;
    private TextView mInfoLeft;
    private TextView mInfoRight;
    private TextView mInfoLog;
    private View mBattery;

    public Indicators(Main main) {
        mMain = main;
        mScanStorage = AbstractActivity.getTempPath();
        mRunning = true;

        mLayoutInfo = main.findViewById(com.lvonasek.arcore3dscanner.R.id.layout_info);
        mInfoLeft = main.findViewById(com.lvonasek.arcore3dscanner.R.id.info_left);
        mInfoRight = main.findViewById(com.lvonasek.arcore3dscanner.R.id.info_right);
        mInfoLog = main.findViewById(com.lvonasek.arcore3dscanner.R.id.infolog);
        mBattery = main.findViewById(com.lvonasek.arcore3dscanner.R.id.info_battery);

        mActivityManager = (ActivityManager) main.getSystemService(Activity.ACTIVITY_SERVICE);
        mMemoryInfo = new ActivityManager.MemoryInfo();
        mLayoutInfo.setVisibility(View.VISIBLE);
        mInfoLog.setMovementMethod(ScrollingMovementMethod.getInstance());
        new Thread(this).start();
    }

    public void disable() {
        mLayoutInfo.setVisibility(View.GONE);
        mInfoLog.setVisibility(View.GONE);
        mRunning = false;
    }

    public void setOverrideMessage(String message) {
        mOverrideMessage = message;
        updateText(mEventText);
    }

    public static int getBatteryPercentage(Context context) {
        IntentFilter iFilter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
        Intent batteryStatus = context.registerReceiver(null, iFilter);
        int level = batteryStatus != null ? batteryStatus.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) : -1;
        int scale = batteryStatus != null ? batteryStatus.getIntExtra(BatteryManager.EXTRA_SCALE, -1) : -1;
        if ((level < 0) || (scale <= 0)) return 0;
        float batteryPct = level / (float) scale;
        return (int) (batteryPct * 100);
    }


    @Override
    public void run()
    {
        while (mRunning) {
            try
            {
                Thread.sleep(1000);
            } catch (InterruptedException e)
            {
                e.printStackTrace();
            }
            mMain.runOnUiThread(() -> {
                if (!mRunning) return;
                //memory info
                mActivityManager.getMemoryInfo(mMemoryInfo);
                long freeMBs = mMemoryInfo.availMem / 1048576L;
                mInfoLeft.setText(freeMBs + " MB");

                boolean memoryCritical = mMemoryInfo.lowMemory
                        || (mMemoryInfo.availMem <= mMemoryInfo.threshold + MEMORY_SAFETY_MARGIN);
                if (++mStorageCheck >= 5) {
                    mUsableStorage = mScanStorage.getUsableSpace();
                    android.os.Debug.MemoryInfo processMemory =
                            mActivityManager.getProcessMemoryInfo(new int[]{Process.myPid()})[0];
                    mProcessPss = processMemory.getTotalPss() * 1024L;
                    long processLimit = Math.min(PROCESS_MEMORY_LIMIT, mMemoryInfo.totalMem * 2L / 5L);
                    mProcessMemoryCritical = mProcessPss > processLimit;
                    mStorageCritical = mUsableStorage < STORAGE_SAFETY_RESERVE;
                    mStorageCheck = 0;
                }
                memoryCritical = memoryCritical || mProcessMemoryCritical;
                if (memoryCritical || mStorageCritical) {
                    if (++mCriticalSamples >= 3) {
                        Log.w(AbstractActivity.TAG, "Pausing scan under resource pressure: availableMemory="
                                + mMemoryInfo.availMem + ", processPss=" + mProcessPss
                                + ", usableStorage=" + mUsableStorage);
                        mMain.onResourcePressure(mStorageCritical
                                ? com.lvonasek.arcore3dscanner.R.string.scan_paused_low_storage
                                : com.lvonasek.arcore3dscanner.R.string.scan_paused_low_memory);
                        mCriticalSamples = 0;
                    }
                } else {
                    mCriticalSamples = 0;
                    mMain.onResourcePressureRecovered();
                }

                //warning
                mInfoLeft.setTextColor((freeMBs < 400) || memoryCritical ? Color.RED : Color.WHITE);

                //battery state
                int bat = getBatteryPercentage(mMain);
                mInfoRight.setText(bat + "%");
                int icon = com.lvonasek.arcore3dscanner.R.drawable.ic_battery_0;
                if (bat > 10)
                    icon = com.lvonasek.arcore3dscanner.R.drawable.ic_battery_20;
                if (bat > 30)
                    icon = com.lvonasek.arcore3dscanner.R.drawable.ic_battery_40;
                if (bat > 50)
                    icon = com.lvonasek.arcore3dscanner.R.drawable.ic_battery_60;
                if (bat > 70)
                    icon = com.lvonasek.arcore3dscanner.R.drawable.ic_battery_80;
                if (bat > 90)
                    icon = com.lvonasek.arcore3dscanner.R.drawable.ic_battery_100;
                mBattery.setBackgroundResource(icon);

                //warning
                mInfoRight.setTextColor(bat < 15 ? Color.RED : Color.WHITE);

                //update info about AR
                mMain.onReconstructionState(JNI.getRecoveryState());
                updateText(JNI.getEvent(mMain.getResources()));
            });
        }
    }

    private void updateText(String text) {
        // Keep the last exposed event when an override clears; do not manufacture tracking states.
        mEventText = text;
        if (mOverrideMessage != null) {
            text = mOverrideMessage;
        }
        if (!TextUtils.equals(mInfoLog.getText(), text)) {
            mInfoLog.setText(text);
            mInfoLog.scrollTo(0, 0);
        }
        int visibility = text.length() > 0 ? View.VISIBLE : View.GONE;
        if (mInfoLog.getVisibility() != visibility) mInfoLog.setVisibility(visibility);
    }
}
