package com.lvonasek.arcore3dscanner.quality;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.preference.PreferenceManager;
import com.lvonasek.arcore3dscanner.main.JNI;
import com.lvonasek.arcore3dscanner.main.Main;
import java.io.File;
import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;
import org.json.JSONObject;

/** Shell-permission-protected, read-only diagnostics. Never starts a camera. */
public final class QualityStatusReceiver extends BroadcastReceiver {
  @Override public void onReceive(Context context, Intent intent) {
    if (!(context.getPackageName()+".QUALITY_STATUS").equals(intent.getAction())) return;
    try {
      SharedPreferences prefs=PreferenceManager.getDefaultSharedPreferences(context);
      File capture=new File(context.getFilesDir(),"capture-dataset");
      File library=new File(context.getFilesDir(),"quality-scans");
      JSONObject result=new JSONObject();
      result.put("package",context.getPackageName());
      result.put("native",JNI.getCaptureDiagnostics());
      result.put("frame_timings",new JSONObject(Main.getFrameTimingSnapshot()));
      result.put("capture",capture.getAbsolutePath());
      result.put("library",library.getAbsolutePath());
      result.put("limit",prefs.getString("pref_limit","4"));
      result.put("resolution",prefs.getString("pref_resolution","0.02"));
      result.put("clearing",prefs.getBoolean("pref_clear",true));
      result.put("offset",prefs.getBoolean("pref_offset",true));
      File state=new File(capture,"state.txt");
      if (state.isFile() && state.length()<=4096) {
        try (FileInputStream input=new FileInputStream(state)) {
          byte[] bytes=new byte[4096];int count=input.read(bytes);
          if(count>0)result.put("capture_state",new String(bytes,0,count,StandardCharsets.US_ASCII).trim());
        }
      }
      setResultCode(1);setResultData(result.toString());
    } catch (Throwable error) {
      setResultCode(0);setResultData(error.getClass().getSimpleName()+": "+error.getMessage());
    }
  }
}
