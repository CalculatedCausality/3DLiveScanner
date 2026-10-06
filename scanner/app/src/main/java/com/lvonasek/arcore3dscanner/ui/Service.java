package com.lvonasek.arcore3dscanner.ui;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.IBinder;
import android.preference.PreferenceManager;
import android.util.Log;

import com.lvonasek.arcore3dscanner.main.JNI;
import com.lvonasek.arcore3dscanner.R;

public class Service extends android.app.Service
{
  private static final String CHANNEL_ID = "scan_processing";
  private static final int NOTIFICATION_ID = 7321;
  public static final String SERVICE_LINK = "service_link";
  public static final String SERVICE_RUNNING = "service_running";

  public static final int SERVICE_NOT_RUNNING = 0;
  public static final int SERVICE_POSTPROCESS = 1;
  public static final int SERVICE_SAVE = 2;
  public static final int SERVICE_PHOTOGRAMMETRY = 4;

  private static Runnable action;
  private static String message;
  private static String messageNotification;
  private static AbstractActivity parent;
  private static boolean running;
  private static boolean starting;
  private static volatile boolean workerRunning;
  private static Service service;

  @Override
  public synchronized void onCreate() {
    super.onCreate();
    service = this;
    starting = false;
    message = "";
    if ((parent == null) || (action == null)) {
      stopSelf();
      service = null;
      return;
    }
    startForeground(NOTIFICATION_ID, createNotification());
    if ((getRunning(parent) == SERVICE_POSTPROCESS) || (getRunning(parent) == SERVICE_SAVE)) {
      running = true;
      new Thread(() -> {
        while(running) {
          setMessage(JNI.getEvent(Service.this.getResources()));
          try
          {
            Thread.sleep(1000);
          } catch (Exception e)
          {
            e.printStackTrace();
          }
        }
        message = "";
      }).start();
    }
    workerRunning = true;
    new Thread(() -> {
      try {
        action.run();
      } catch (Throwable throwable) {
        Log.e(AbstractActivity.TAG, "Background processing failed", throwable);
        running = false;
        SharedPreferences.Editor editor = PreferenceManager.getDefaultSharedPreferences(Service.this).edit();
        editor.putInt(SERVICE_RUNNING, SERVICE_NOT_RUNNING);
        editor.putString(SERVICE_LINK, "");
        editor.apply();
        stopForegroundWork();
      } finally {
        workerRunning = false;
      }
    }, "scan-processing").start();
  }

  @Override
  public int onStartCommand(Intent intent, int flags, int startId)
  {
    return START_NOT_STICKY;
  }

  @Override
  public IBinder onBind(Intent intent)
  {
    return null;
  }

  @Override
  public void onDestroy() {
    running = false;
    if (service == this) service = null;
    super.onDestroy();
  }

  public static synchronized void finish(String link)
  {
    running = false;
    if (service != null) service.stopForegroundWork();
    SharedPreferences.Editor e = PreferenceManager.getDefaultSharedPreferences(parent).edit();
    e.putInt(SERVICE_RUNNING, -Math.abs(getRunning(parent)));
    e.putString(SERVICE_LINK, link);
    e.commit();
    System.exit(0);
  }

  public static synchronized void forceState(AbstractActivity activity, String link, int state)
  {
    running = false;
    if (service != null) service.stopForegroundWork();
    SharedPreferences.Editor e = PreferenceManager.getDefaultSharedPreferences(activity).edit();
    e.putInt(SERVICE_RUNNING, -Math.abs(state));
    e.putString(SERVICE_LINK, link);
    e.commit();
    System.exit(0);
  }

  public static synchronized void interrupt() {
    messageNotification = null;
    message = null;
  }

  public static synchronized void process(String message, int serviceId, AbstractActivity activity, Runnable runnable)
  {
    if (starting || workerRunning || (service != null)) {
      Log.w(AbstractActivity.TAG, "Ignoring duplicate processing request");
      return;
    }
    starting = true;
    action = runnable;
    parent = activity;
    messageNotification = message;

    SharedPreferences.Editor e = PreferenceManager.getDefaultSharedPreferences(activity).edit();
    e.putInt(SERVICE_RUNNING, serviceId);
    e.putString(SERVICE_LINK, "");
    e.commit();
    Intent intent = new Intent(activity, Service.class);
    try {
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
        activity.startForegroundService(intent);
      else
        activity.startService(intent);
    } catch (RuntimeException exception) {
      starting = false;
      action = null;
      parent = null;
      PreferenceManager.getDefaultSharedPreferences(activity).edit()
              .putInt(SERVICE_RUNNING, SERVICE_NOT_RUNNING).apply();
      throw exception;
    }
  }

  public static synchronized String getLink(Context context)
  {
    SharedPreferences pref = PreferenceManager.getDefaultSharedPreferences(context);
    return pref.getString(SERVICE_LINK, "");
  }

  public static synchronized String getMessage()
  {
    if (messageNotification == null || message == null)
      return null;
    return messageNotification + "\n" + message;
  }

  public static synchronized int getRunning(Context context)
  {
    SharedPreferences pref = PreferenceManager.getDefaultSharedPreferences(context);
    return pref.getInt(SERVICE_RUNNING, SERVICE_NOT_RUNNING);
  }

  public static synchronized boolean isActive() {
    return starting || workerRunning || (service != null);
  }

  public static synchronized void clearAbandonedState(Context context) {
    SharedPreferences.Editor editor = PreferenceManager.getDefaultSharedPreferences(context).edit();
    editor.putInt(SERVICE_RUNNING, SERVICE_NOT_RUNNING);
    editor.putString(SERVICE_LINK, "");
    editor.apply();
  }

  private static synchronized void setMessage(String msg)
  {
    message = msg;
  }

  public static synchronized void setMessageNotification(String msg)
  {
    messageNotification = msg;
  }

  public static synchronized void reset(Context context)
  {
    try
    {
      if (service != null) service.stopForegroundWork();
    } catch(Exception e)
    {
      e.printStackTrace();
    }
    SharedPreferences.Editor e = PreferenceManager.getDefaultSharedPreferences(context).edit();
    e.putInt(SERVICE_RUNNING, SERVICE_NOT_RUNNING);
    e.putString(SERVICE_LINK, "");
    e.commit();
    starting = false;
    action = null;
    parent = null;
  }

  private Notification createNotification() {
    NotificationManager manager = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
      NotificationChannel channel = new NotificationChannel(
              CHANNEL_ID, getString(R.string.processing_channel), NotificationManager.IMPORTANCE_LOW);
      manager.createNotificationChannel(channel);
    }
    Intent intent = new Intent(this, FileManager.class);
    PendingIntent pendingIntent = PendingIntent.getActivity(this, 0, intent,
            PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    Notification.Builder builder = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
            ? new Notification.Builder(this, CHANNEL_ID)
            : new Notification.Builder(this);
    return builder.setSmallIcon(android.R.drawable.stat_sys_upload)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(messageNotification)
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .build();
  }

  private void stopForegroundWork() {
    stopForeground(true);
    stopSelf();
    if (service == this) service = null;
  }
}
