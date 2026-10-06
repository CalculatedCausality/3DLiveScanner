package com.lvonasek.arcore3dscanner.debug;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.preference.PreferenceManager;
import com.lvonasek.arcore3dscanner.main.JNI;

/** Debug-only, shell-permission-protected switch. Does not start the camera. */
public final class TpuTestReceiver extends BroadcastReceiver {
  @Override public void onReceive(Context context, Intent intent) {
    if (!"com.lvonasek.arcore3dscanner.SET_TPU_TEST".equals(intent.getAction())) return;
    SharedPreferences preferences=PreferenceManager.getDefaultSharedPreferences(context);
    if(intent.hasExtra("coverage_preview")||intent.hasExtra("capture_first")||intent.hasExtra("space_clearing")||intent.hasExtra("geometry_offset")) {
      SharedPreferences.Editor editor=preferences.edit();
      if(intent.hasExtra("coverage_preview"))editor.putBoolean("pref_coverage_preview",intent.getBooleanExtra("coverage_preview",false));
      if(intent.hasExtra("capture_first"))editor.putBoolean("pref_later",intent.getBooleanExtra("capture_first",true));
      if(intent.hasExtra("space_clearing"))editor.putBoolean("pref_clear",intent.getBooleanExtra("space_clearing",true));
      if(intent.hasExtra("geometry_offset"))editor.putBoolean("pref_offset",intent.getBooleanExtra("geometry_offset",false));
      if(!editor.commit()){setResultCode(0);setResultData("Capture preference write failed");return;}
    }
    if(intent.hasExtra("enabled")) {
      boolean enabled=intent.getBooleanExtra("enabled",false);
      if(enabled && android.os.Build.VERSION.SDK_INT<29) {
        setResultCode(0);setResultData("API 29 required");return;
      }
      if(!preferences.edit().putBoolean("pref_tpu_depth_test",enabled).commit()) {
        setResultCode(0);setResultData("Preference write failed");return;
      }
      JNI.setExperimentalDepth(enabled);
    }
    boolean enabled=preferences.getBoolean("pref_tpu_depth_test",false);
    if(intent.getBooleanExtra("selftest",false)) {
      new Thread(()->{
        try { android.util.Log.i("TPU_APP_TEST",JNI.testExperimentalDepthRuntime()); }
        catch(Throwable failure) { android.util.Log.e("TPU_APP_TEST","FAIL runtime test",failure); }
      },"TPU-app-runtime-test").start();
    }
    setResultCode(1);
    setResultData((enabled?"enabled":"disabled")+"; "+JNI.getExperimentalDepthStatus()
            +"; coverage_preview="+preferences.getBoolean("pref_coverage_preview",false)
            +"; capture_first="+preferences.getBoolean("pref_later",true)
            +"; space_clearing="+preferences.getBoolean("pref_clear",true)
            +"; geometry_offset="+preferences.getBoolean("pref_offset",false)
            +(intent.getBooleanExtra("diagnostics",false)?"; "+JNI.getCaptureDiagnostics():""));
  }
}
