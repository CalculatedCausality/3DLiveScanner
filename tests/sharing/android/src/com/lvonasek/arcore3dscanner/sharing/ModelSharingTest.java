package com.lvonasek.arcore3dscanner.sharing;

import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Looper;

import androidx.appcompat.app.AppCompatActivity;

import org.junit.Test;
import org.junit.Before;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.LooperMode;
import org.robolectric.annotation.Implementation;
import org.robolectric.annotation.Implements;
import org.robolectric.shadows.ShadowDialog;
import org.robolectric.shadows.ShadowToast;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;

import static org.junit.Assert.*;

/** Android 33 real framework Intent/ClipData/FileProvider under Robolectric's boundary shadows. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 33)
@LooperMode(LooperMode.Mode.PAUSED)
public class ModelSharingTest {
  @Implements(value = ModelPackage.class, isInAndroidSdk = false)
  public static class FailingCompression {
    @Implementation protected static ModelPackage.Result prepare(File selected, File cache) {
      throw new IllegalStateException("Injected compression failure");
    }
  }

  @Before public void resetProviderCacheForRobolectricSandbox() throws Exception {
    // Robolectric changes app cache directories between tests; AndroidX retains its
    // static authority cache outside the framework sandbox. Real apps keep one root.
    java.lang.reflect.Field field = androidx.core.content.FileProvider.class.getDeclaredField("sCache");
    field.setAccessible(true);
    ((java.util.Map<?, ?>) field.get(null)).clear();
  }

  public static class Host extends AppCompatActivity {
    boolean failLaunch;
    @Override public void onCreate(Bundle state) {
      setTheme(com.google.android.material.R.style.Theme_MaterialComponents_DayNight);
      super.onCreate(state);
    }
    @Override public void startActivity(Intent intent) {
      if (failLaunch) throw new ActivityNotFoundException("No receiver");
      super.startActivity(intent);
    }
  }

  private File ply(Host host) throws Exception {
    File source = new File(host.getFilesDir(), "cloud.ply");
    Files.write(source.toPath(), "ply\nformat ascii 1.0\nend_header\n".getBytes(StandardCharsets.UTF_8));
    return source;
  }

  private void await(BooleanSupplier condition) throws Exception {
    long deadline = System.nanoTime() + 10_000_000_000L;
    while (!condition.getAsBoolean() && System.nanoTime() < deadline) {
      Thread.sleep(10);
      Shadows.shadowOf(Looper.getMainLooper()).idle();
    }
    assertTrue("Background share completed", condition.getAsBoolean());
  }

  @Test public void chooserAndSendGrantReadableContentUri() throws Exception {
    try (ActivityController<Host> controller = Robolectric.buildActivity(Host.class).setup()) {
      Host host = controller.get();
      File source = ply(host);
      ModelPackage.Result result = ModelPackage.prepare(source, host.getCacheDir());
      Intent chooser = ModelSharing.createChooser(host, result);
      assertEquals(Intent.ACTION_CHOOSER, chooser.getAction());
      Intent send = chooser.getParcelableExtra(Intent.EXTRA_INTENT);
      assertNotNull(send);
      assertEquals(Intent.ACTION_SEND, send.getAction());
      assertEquals("application/octet-stream", send.getType());
      Uri uri = send.getParcelableExtra(Intent.EXTRA_STREAM);
      assertEquals("content", uri.getScheme());
      assertEquals("com.lvonasek.arcore3dscanner.provider", uri.getAuthority());
      assertTrue(uri.getPath().startsWith("/model_shares/"));
      for (Intent intent : new Intent[]{send, chooser}) {
        assertEquals(Intent.FLAG_GRANT_READ_URI_PERMISSION,
            intent.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION);
        assertEquals(0, intent.getFlags() & Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        assertEquals(uri, intent.getClipData().getItemAt(0).getUri());
      }
      try (java.io.InputStream stream = host.getContentResolver().openInputStream(uri)) {
        assertArrayEquals(Files.readAllBytes(source.toPath()), stream.readAllBytes());
      }
      Uri another = ((Intent) ModelSharing.createChooser(host,
          ModelPackage.prepare(source, host.getCacheDir())).getParcelableExtra(Intent.EXTRA_INTENT))
          .getParcelableExtra(Intent.EXTRA_STREAM);
      assertNotEquals(uri, another);
      assertTrue(source.isFile());
    }
  }

  @Test public void refusesFileUris() {
    try {
      ModelSharing.createSendIntent(Uri.parse("file:///private/model.obj"), "application/zip", "model");
      fail("File URI accepted");
    } catch (IllegalArgumentException expected) {
      assertTrue(expected.getMessage().contains("content URI"));
    }
  }

  @Test public void singleOperationRestoresProgressAndAllowsNextShare() throws Exception {
    try (ActivityController<Host> controller = Robolectric.buildActivity(Host.class).setup()) {
      Host host = controller.get();
      File source = ply(host);
      AtomicBoolean launched = new AtomicBoolean();
      assertTrue(ModelSharing.share(host, source, () -> launched.set(true)));
      android.app.Dialog progress = ShadowDialog.getLatestDialog();
      assertTrue(progress.isShowing());
      assertFalse(ModelSharing.share(host, source));
      assertTrue(ShadowToast.getTextOfLatestToast().contains("already"));
      await(launched::get);
      assertFalse(progress.isShowing());
      assertEquals(Intent.ACTION_CHOOSER, Shadows.shadowOf(host).getNextStartedActivity().getAction());
      launched.set(false);
      assertTrue(ModelSharing.share(host, source, () -> launched.set(true)));
      await(launched::get);
      assertEquals(Intent.ACTION_CHOOSER, Shadows.shadowOf(host).getNextStartedActivity().getAction());
    }
  }

  @Test public void missingAssetAndMissingReceiverSurfaceErrorsAndReleaseGate() throws Exception {
    try (ActivityController<Host> controller = Robolectric.buildActivity(Host.class).setup()) {
      Host host = controller.get();
      File obj = new File(host.getFilesDir(), "broken.obj");
      Files.write(obj.toPath(), "mtllib absent.mtl\n".getBytes(StandardCharsets.UTF_8));
      assertTrue(ModelSharing.share(host, obj));
      android.app.Dialog progress = ShadowDialog.getLatestDialog();
      await(() -> !progress.isShowing());
      android.app.Dialog error = ShadowDialog.getLatestDialog();
      assertTrue(error.isShowing());
      assertTrue(((android.widget.TextView) error.findViewById(android.R.id.message)).getText().toString().contains("absent.mtl"));
      assertNull(Shadows.shadowOf(host).getNextStartedActivity());
      error.dismiss();

      host.failLaunch = true;
      AtomicBoolean launched = new AtomicBoolean();
      assertTrue(ModelSharing.share(host, ply(host), () -> launched.set(true)));
      android.app.Dialog progress2 = ShadowDialog.getLatestDialog();
      await(() -> !progress2.isShowing());
      assertFalse(launched.get());
      error = ShadowDialog.getLatestDialog();
      assertTrue(((android.widget.TextView) error.findViewById(android.R.id.message)).getText().toString().contains("No receiver"));
      error.dismiss();
      host.failLaunch = false;
      assertTrue(ModelSharing.share(host, ply(host), () -> launched.set(true)));
      await(launched::get);
    }
  }

  @Test public void destroyedHostDoesNotLaunchAndReleasesGate() throws Exception {
    ActivityController<Host> controller = Robolectric.buildActivity(Host.class).setup();
    Host host = controller.get();
    AtomicBoolean launched = new AtomicBoolean();
    assertTrue(ModelSharing.share(host, ply(host), () -> launched.set(true)));
    controller.pause().stop().destroy();
    // Wait until the worker posts completion; queued UI work is explicitly drained.
    await(() -> Thread.getAllStackTraces().keySet().stream()
        .noneMatch(thread -> thread.isAlive() && thread.getName().equals("model-sharing")));
    Shadows.shadowOf(Looper.getMainLooper()).idle();
    assertFalse(launched.get());
    assertNull(Shadows.shadowOf(host).getNextStartedActivity());
    try (ActivityController<Host> next = Robolectric.buildActivity(Host.class).setup()) {
      assertTrue(ModelSharing.share(next.get(), ply(next.get()), () -> launched.set(true)));
      await(launched::get);
    }
  }

  @Test public void datasetExplainsExportRequirementWithoutLaunching() throws Exception {
    try (ActivityController<Host> controller = Robolectric.buildActivity(Host.class).setup()) {
      Host host = controller.get();
      File dataset = new File(host.getFilesDir(), "scan.dataset");
      assertTrue(dataset.mkdir());
      assertTrue(ModelSharing.share(host, dataset));
      android.app.Dialog progress = ShadowDialog.getLatestDialog();
      await(() -> !progress.isShowing());
      android.app.Dialog error = ShadowDialog.getLatestDialog();
      String message = ((android.widget.TextView) error.findViewById(android.R.id.message)).getText().toString();
      assertTrue(message.contains("Export it to OBJ or PLY"));
      assertTrue(message.contains("Raw dataset sharing is not supported"));
      assertNull(Shadows.shadowOf(host).getNextStartedActivity());
      assertTrue(dataset.isDirectory());
    }
  }

  @Test
  @Config(shadows = FailingCompression.class,
      instrumentedPackages = "com.lvonasek.arcore3dscanner.sharing.ModelPackage")
  public void sharingCatchesUncheckedCompressionFailure() throws Exception {
    try (ActivityController<Host> controller = Robolectric.buildActivity(Host.class).setup()) {
      Host host = controller.get();
      android.widget.Button viewerShare = new android.widget.Button(host);
      host.setContentView(viewerShare);
      AtomicBoolean launched = new AtomicBoolean();
      for (int attempt = 0; attempt < 2; attempt++) {
        assertTrue(ModelSharing.share(host, ply(host), () -> launched.set(true)));
        android.app.Dialog progress = ShadowDialog.getLatestDialog();
        await(() -> !progress.isShowing());
        android.app.Dialog error = ShadowDialog.getLatestDialog();
        assertTrue(error.isShowing());
        String message = ((android.widget.TextView) error.findViewById(android.R.id.message))
            .getText().toString();
        assertTrue(message.contains("Injected compression failure"));
        assertFalse(launched.get());
        assertFalse(host.isFinishing());
        assertTrue(viewerShare.isShown());
        assertTrue(viewerShare.isEnabled());
        assertNull(Shadows.shadowOf(host).getNextStartedActivity());
        error.dismiss();
      }
    }
  }
}
