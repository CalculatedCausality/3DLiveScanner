package com.lvonasek.arcore3dscanner.quality;

import android.app.Application;
import android.content.SharedPreferences;
import android.preference.PreferenceManager;

/** First-run defaults from the user-validated original-engine configuration. */
public final class QualityApplication extends Application {
  @Override public void onCreate() {
    super.onCreate();
    SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(this);
    SharedPreferences.Editor edit = prefs.edit();
    if (!prefs.contains("pref_resolution")) edit.putString("pref_resolution", "0.02");
    if (!prefs.contains("pref_limit")) edit.putString("pref_limit", "4");
    if (!prefs.contains("pref_noise")) edit.putString("pref_noise", "9");
    if (!prefs.contains("pref_clear")) edit.putBoolean("pref_clear", true);
    if (!prefs.contains("pref_offset")) edit.putBoolean("pref_offset", true);
    if (!prefs.contains("pref_later")) edit.putBoolean("pref_later", true);
    edit.putBoolean("pref_tpu_depth_test", false).putBoolean("pref_coverage_preview", false).apply();
  }
}
