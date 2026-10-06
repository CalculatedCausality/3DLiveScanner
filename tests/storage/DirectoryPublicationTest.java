import com.lvonasek.utils.IO;
import java.io.*;
import java.nio.file.*;
import java.util.*;

/** Real directory copy/readback/publication. Host hooks inject filesystem races/failures. */
public class DirectoryPublicationTest extends TestSupport {
  @SuppressWarnings("removal")
  static void injected(Path root, String mode) throws Exception {
    Path source = root.resolve("working");
    Path destination = root.resolve("library/scan.dataset");
    write(source.resolve("frame.bin"), "complete source");
    SecurityManager previous = System.getSecurityManager();
    class FailureHook extends SecurityManager {
      boolean inside;
      boolean fired;
      public void checkPermission(java.security.Permission permission) {}
      public void checkRead(String file) { trigger(file, false); }
      public void checkWrite(String file) { trigger(file, true); }
      void trigger(String file, boolean writeAccess) {
        if (inside || fired) return;
        inside = true;
        try {
          Path path = Paths.get(file);
          boolean stagedFile = path.getFileName().toString().equals("frame.bin")
              && path.getParent().getFileName().toString().startsWith(".storage-");
          if (mode.equals("corrupt") && !writeAccess && stagedFile && Files.isRegularFile(path)) {
            fired = true;
            Files.write(path, "corrupted copy!".getBytes("UTF-8"));
          } else if (mode.equals("output") && writeAccess && stagedFile && !Files.exists(path)) {
            fired = true;
            Files.createDirectory(path); // Opening a directory as the output file must fail.
          } else if ((mode.equals("changing") || mode.equals("size")) && writeAccess && stagedFile && !Files.exists(path)) {
            fired = true;
            if (mode.equals("size")) Files.write(source.resolve("frame.bin"), "short".getBytes("UTF-8"));
            else write(source.resolve("new-frame.pcl"), "concurrent source update");
          } else if (mode.equals("collision") && writeAccess && path.equals(destination)) {
            fired = true;
            write(destination.resolve("existing.obj"), "other scan");
          }
        } catch (IOException error) { throw new UncheckedIOException(error); }
        finally { inside = false; }
      }
    }
    FailureHook hook = new FailureHook();
    try {
      System.setSecurityManager(hook);
      fails(() -> IO.publishDirectoryChecked(source.toFile(), destination.toFile()));
    } finally { System.setSecurityManager(previous); }
    check(hook.fired, "Failure hook did not run");
    check(text(source.resolve("frame.bin")).equals(mode.equals("size") ? "short" : "complete source"), "Destroyed source on failure");
    if (mode.equals("collision"))
      check(text(destination.resolve("existing.obj")).equals("other scan"), "Overwrote collision");
    else check(!Files.exists(destination), "Published an incomplete/mutated snapshot");
    noScratch(root);
  }

  public static void main(String[] args) throws Exception {
    test("directory publication verifies nested/empty files and retains source", root -> {
      Path source = root.resolve("private/capture");
      byte[] data = new byte[200000]; new Random(12).nextBytes(data);
      Files.createDirectories(source.resolve("textures/empty"));
      Files.write(source.resolve("00000000.pcl"), data);
      Files.write(source.resolve("textures/zero"), new byte[0]);
      write(source.resolve("state.txt"), "1 360 640 180 320 500 500");
      Path destination = root.resolve("library/scan.dataset");
      IO.publishDirectoryChecked(source.toFile(), destination.toFile());
      check(Arrays.equals(data, Files.readAllBytes(destination.resolve("00000000.pcl"))), "Changed payload");
      check(Files.isDirectory(destination.resolve("textures/empty")), "Lost empty directory");
      check(Files.size(destination.resolve("textures/zero")) == 0, "Lost empty file");
      check(Arrays.equals(data, Files.readAllBytes(source.resolve("00000000.pcl"))), "Removed source");
      fails(() -> IO.publishDirectoryChecked(source.toFile(), destination.toFile()));
      noScratch(root);
    });
    test("directory publication rejects missing/recursive/existing destinations", root -> {
      Path source = Files.createDirectory(root.resolve("working"));
      fails(() -> IO.publishDirectoryChecked(source.toFile(), source.resolve("nested").toFile()));
      fails(() -> IO.publishDirectoryChecked(source.toFile(), source.toFile()));
      fails(() -> IO.publishDirectoryChecked(root.resolve("absent").toFile(), root.resolve("out").toFile()));
      Path existing = Files.createDirectory(root.resolve("existing"));
      fails(() -> IO.publishDirectoryChecked(source.toFile(), existing.toFile()));
      noScratch(root);
    });
    test("directory publication and archives reject linked resources", root -> {
      Path source = Files.createDirectory(root.resolve("working"));
      Path outside = write(root.resolve("outside"), "private data").toPath();
      Files.createSymbolicLink(source.resolve("link"), outside);
      fails(() -> IO.publishDirectoryChecked(source.toFile(), root.resolve("library/out").toFile()));
      Path archive = write(root.resolve("archive.zip"), "previous archive").toPath();
      for (Path target : Arrays.asList(outside, source, write(source.resolve("frame.bin"), "capture").toPath())) {
        Files.delete(source.resolve("link"));
        Files.createSymbolicLink(source.resolve("link"), target);
        fails(() -> IO.zipDirectory(source.toFile(), archive.toString()));
      }
      check(text(archive).equals("previous archive"), "Changed archive after linked source failure");
      check(text(outside).equals("private data"), "Touched linked resource");
      noScratch(root);
    });
    for (String mode : new String[]{"corrupt", "output", "changing", "size", "collision"})
      test("directory publication retains source on " + mode, root -> injected(root, mode));
    finish();
  }
}
