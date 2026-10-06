package com.lvonasek.arcore3dscanner.ui;

import android.graphics.Color;
import android.os.Bundle;
import android.preference.CheckBoxPreference;
import android.preference.ListPreference;
import android.preference.Preference;
import android.preference.PreferenceActivity;
import android.preference.PreferenceScreen;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;

import com.lvonasek.arcore3dscanner.R;

public class Settings extends PreferenceActivity {
  @Override
  public void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    setTheme(android.R.style.Theme_Material_NoActionBar_Fullscreen);
    overridePendingTransition(android.R.anim.slide_in_left, android.R.anim.fade_out);
    addPreferencesFromResource(R.xml.settings);
    findPreference("privacy_policy_link").setOnPreferenceClickListener(preference->{
      AbstractActivity.openURL(this,"https://lvonasek.github.io/policy-3dls.html");
      return true;
    });
    Preference tpu=findPreference("pref_tpu_depth_test");
    boolean supported="modern".equals(com.lvonasek.arcore3dscanner.BuildConfig.FLAVOR)
            && android.os.Build.VERSION.SDK_INT>=29;
    findPreference("pref_coverage_preview").setEnabled("modern".equals(com.lvonasek.arcore3dscanner.BuildConfig.FLAVOR));
    tpu.setEnabled(supported);
    tpu.setOnPreferenceChangeListener((preference,value)->{
      com.lvonasek.arcore3dscanner.main.JNI.setExperimentalDepth(Boolean.TRUE.equals(value));
      return true;
    });
  }

  @Override
  protected void onPause() {
    super.onPause();
    overridePendingTransition(android.R.anim.fade_in, R.anim.slide_out_left);
  }

  @Override
  protected void onResume() {
    super.onResume();
    setStyle(getWindow());

    //set depth sensor settings
    ((CheckBoxPreference)findPreference(getString(R.string.pref_depth))).setChecked(AbstractActivity.isTofOn(this));
    findPreference(getString(R.string.pref_depth)).setEnabled(AbstractActivity.isTofSupported(this));

    //update items
    keepUpdated((ListPreference) findPreference(getString(R.string.pref_resolution)));

    //visual updates
    for (String key : new String[]{"src_hardware", "src_realtime", "src_parameters",
            "src_postprocess", "src_visualisations"}) {
      findPreference(key).setOnPreferenceClickListener(fixBackground);
    }
  }

  private void keepUpdated(ListPreference pref) {
    if (pref.isEnabled()) {
      CharSequence[] texts = pref.getEntries();
      CharSequence[] values = pref.getEntryValues();
      pref.setOnPreferenceChangeListener((preference, newValue) -> {
        updateSummary(pref, texts, values, (String)newValue);
        return true;
      });
      updateSummary(pref, texts, values, pref.getValue());
    } else {
      pref.setSummary("");
    }
  }

  private void updateSummary(ListPreference pref, CharSequence[] texts, CharSequence[] values, String value) {
    for (int i = 0; i < values.length; i++) {
      if (values[i].toString().compareTo(value) == 0) {
        pref.setSummary(texts[i]);
        return;
      }
    }
    pref.setSummary(value);
  }

  protected void setStyle(Window window) {
    int lFlags = window.getDecorView().getSystemUiVisibility();
    window.clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
    window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    window.setBackgroundDrawable(getDrawable(com.lvonasek.arcore3dscanner.R.drawable.background_settings));
    window.setStatusBarColor(Color.argb(255, 48, 48, 48));
    window.setNavigationBarColor(Color.argb(255, 32, 32, 32));
    window.getDecorView().setSystemUiVisibility(lFlags & ~View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR);
    AbstractActivity.setOrientation(true, this);
  }

  protected Preference.OnPreferenceClickListener fixBackground = preference -> {
    PreferenceScreen a = (PreferenceScreen) preference;
    setStyle(a.getDialog().getWindow());
    return false;
  };
}
