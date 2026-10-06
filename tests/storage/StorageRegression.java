import com.lvonasek.arcore3dscanner.main.Exporter;
import com.lvonasek.arcore3dscanner.ui.AbstractActivity;
import com.lvonasek.utils.IO;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

public class StorageRegression extends TestSupport {
    /** Inject a filesystem collision at the directory publish boundary without replacing IO code.
     * JDK 17's host-only security hook observes rename checks; all file operations remain real. */
    @SuppressWarnings("removal")
    private static void restructureCollision(Path root, boolean failRollback) throws Exception {
        Path library = AbstractActivity.root.toPath();
        Path model = library.resolve("scan.ply");
        write(model, "recoverable cloud");
        SecurityManager previous = System.getSecurityManager();
        final Path[] staged = {null};
        SecurityManager hook = new SecurityManager() {
            boolean insideHook;
            public void checkPermission(java.security.Permission permission) { }
            public void checkWrite(String file) {
                if (insideHook) return;
                insideHook = true;
                try {
                    Path path = Paths.get(file);
                    if (staged[0] == null && library.equals(path.getParent())
                            && path.getFileName().toString().startsWith(".storage-") && !Files.exists(model)) {
                        staged[0] = path;
                        write(model.resolve("collision"), "concurrent data");
                    } else if (!failRollback && staged[0] != null && path.equals(staged[0].resolve("scan.ply"))) {
                        Files.deleteIfExists(model.resolve("collision"));
                        Files.deleteIfExists(model);
                    }
                } catch (IOException e) { throw new UncheckedIOException(e); }
                finally { insideHook = false; }
            }
        };
        try {
            System.setSecurityManager(hook);
            Exporter.makeStructure(library.toString());
        } finally {
            System.setSecurityManager(previous);
        }
        check(staged[0] != null, "Publish failure hook did not run");
        if (failRollback) {
            check(text(staged[0].resolve("scan.ply")).equals("recoverable cloud"), "Deleted only model copy after rollback failure");
            check(text(model.resolve("collision")).equals("concurrent data"), "Damaged colliding data");
        } else {
            check(text(model).equals("recoverable cloud"), "Did not restore original model");
            noScratch(root);
        }
    }

    private static class FaultInput extends InputStream {
        private final byte[] bytes = new byte[200000];
        private final int failAt;
        private final boolean failClose;
        private int offset;
        boolean closed;
        FaultInput(int failAt, boolean failClose) {
            this.failAt = failAt;
            this.failClose = failClose;
            new Random(7).nextBytes(bytes);
        }
        public int read() throws IOException {
            if (offset >= failAt) throw new IOException("injected provider read failure");
            return offset == bytes.length ? -1 : bytes[offset++] & 255;
        }
        public int read(byte[] buffer, int start, int length) throws IOException {
            if (offset >= failAt) throw new IOException("injected provider read failure");
            if (offset == bytes.length) return -1;
            int count = Math.min(length, Math.min(bytes.length - offset, failAt - offset));
            System.arraycopy(bytes, offset, buffer, start, count);
            offset += count;
            return count;
        }
        public void close() throws IOException {
            closed = true;
            if (failClose) throw new IOException("injected provider close failure");
        }
    }

    public static void main(String[] args) throws Exception {
        test("large copy, replacement, and zero-byte read", root -> {
            byte[] expected = new byte[200000];
            new Random(42).nextBytes(expected);
            InputStream input = new ByteArrayInputStream(expected) {
                boolean zero = true;
                public int read(byte[] bytes, int start, int length) {
                    if (zero) { zero = false; return 0; }
                    return super.read(bytes, start, length);
                }
            };
            File destination = write(root.resolve("copy"), "old");
            IO.copyToFile(input, destination);
            check(Arrays.equals(expected, Files.readAllBytes(destination.toPath())), "Copy mismatch");
            noScratch(root);
        });
        test("partial read failure preserves destination and closes input", root -> {
            File destination = write(root.resolve("copy"), "existing");
            FaultInput input = new FaultInput(70000, false);
            fails(() -> IO.copyToFile(input, destination));
            check(input.closed, "Leaked provider stream");
            check(text(destination.toPath()).equals("existing"), "Published partial bytes");
            noScratch(root);
        });
        test("input close failure prevents publication", root -> {
            File destination = write(root.resolve("copy"), "existing");
            FaultInput input = new FaultInput(Integer.MAX_VALUE, true);
            fails(() -> IO.copyToFile(input, destination));
            check(input.closed && text(destination.toPath()).equals("existing"), "Published despite close failure");
            noScratch(root);
        });
        test("failed copy rename preserves nonempty destination", root -> {
            Path destination = Files.createDirectory(root.resolve("destination"));
            write(destination.resolve("scan.obj"), "existing");
            FaultInput input = new FaultInput(Integer.MAX_VALUE, false);
            fails(() -> IO.copyToFile(input, destination.toFile()));
            check(input.closed && text(destination.resolve("scan.obj")).equals("existing"), "Changed destination");
            noScratch(root);
        });
        test("ZIP partial-write failure preserves old archive", root -> {
            String first = write(root.resolve("first"), "first payload").toString();
            String second = write(root.resolve("second"), "second payload").toString();
            File destination = write(root.resolve("archive.zip"), "existing archive");
            // Simulate a file disappearing after validation, when the second member is opened.
            List<String> changingFiles = new ArrayList<String>(Arrays.asList(first, second)) {
                int reads;
                public String get(int index) {
                    if (index == 1 && ++reads == 2) return root.resolve("disappeared").toString();
                    return super.get(index);
                }
            };
            fails(() -> IO.zip(changingFiles, destination.toString()));
            check(text(destination.toPath()).equals("existing archive"), "Partial ZIP replaced archive");
            noScratch(root);
        });
        test("ZIP duplicate basenames and self-source are rejected", root -> {
            File one = write(root.resolve("one/model.obj"), "one");
            File two = write(root.resolve("two/model.obj"), "two");
            File destination = write(root.resolve("archive.zip"), "existing");
            fails(() -> IO.zip(Arrays.asList(one.toString(), two.toString()), destination.toString()));
            fails(() -> IO.zip(Collections.singletonList(destination.toString()), destination.toString()));
            check(text(destination.toPath()).equals("existing"), "Changed archive");
            noScratch(root);
        });
        test("ZIP failed publish leaves destination intact", root -> {
            File source = write(root.resolve("model.obj"), "mesh");
            Path destination = Files.createDirectory(root.resolve("archive.zip"));
            write(destination.resolve("existing"), "keep");
            fails(() -> IO.zip(Collections.singletonList(source.toString()), destination.toString()));
            check(text(destination.resolve("existing")).equals("keep"), "Changed directory");
            noScratch(root);
        });
        test("valid nested ZIP imports into absent and empty canonical roots", root -> {
            byte[] archive = zip("textures/", "", "textures/color.png", "texture", "scan.obj", "mesh");
            Path absent = root.resolve("absent");
            check(IO.unzip(absent.toString(), new ByteArrayInputStream(archive)), "Absent destination import failed");
            Path actual = Files.createDirectory(root.resolve("actual"));
            Path alias = Files.createSymbolicLink(root.resolve("alias"), actual);
            check(IO.unzip(alias.toString() + "/", new ByteArrayInputStream(archive)), "Canonical root alias import failed");
            check(text(absent.resolve("textures/color.png")).equals("texture"), "Texture differs");
            check(text(actual.resolve("scan.obj")).equals("mesh"), "Model differs");
            noScratch(root);
        });
        test("ZIP closes provider on parse/read/close failures", root -> {
            Path destination = Files.createDirectory(root.resolve("import"));
            for (FaultInput input : Arrays.asList(new FaultInput(70000, false), new FaultInput(Integer.MAX_VALUE, true))) {
                check(!IO.unzip(destination.toString(), input), "Import succeeded despite provider failure");
                check(input.closed && empty(destination), "Leaked stream or partial import");
            }
            ByteArrayInputStream invalid = new ByteArrayInputStream(new byte[]{1, 2, 3}) {
                public void close() { reset(); }
            };
            check(!IO.unzip(destination.toString(), invalid) && invalid.available() == 3, "Invalid ZIP stream not closed");
            noScratch(root);
        });
        test("ZIP path aliases, absolute names, and file-directory conflicts fail", root -> {
            Path destination = Files.createDirectory(root.resolve("import"));
            for (byte[] archive : Arrays.asList(
                    zip("scan.obj", "one", "./scan.obj", "two"),
                    zip("scan.obj", "mesh", "/absolute.obj", "outside"),
                    zip("a", "file", "a/texture", "nested"),
                    zip("../escape.obj", "outside"))) {
                check(!IO.unzip(destination.toString(), new ByteArrayInputStream(archive)), "Accepted unsafe ZIP");
                check(empty(destination), "Published partial unsafe ZIP");
                noScratch(root);
            }
        });
        test("OBJ export copies all resources and keeps source", root -> {
            File model = write(root.resolve("source/model.obj"), "  mtllib\tmodel.mtl\nv 0 0 0\n");
            write(root.resolve("source/model.mtl"), "map_Kd textures/color.png\nmap_Ka textures/color.png\n");
            write(root.resolve("source/textures/color.png"), "texture");
            write(root.resolve("source/model.mtl.png"), "preview");
            write(root.resolve("source/position.txt"), "GPS");
            File exported = Exporter.export(model, "scan");
            check(exported.isFile() && exported.getName().equals("scan.obj"), "Wrong model path");
            check(exported.getParentFile().getName().equals("scan.obj"), "Changed scan-directory format");
            check(text(exported.toPath()).equals(text(model.toPath())), "Model differs");
            check(text(exported.toPath().resolveSibling("textures/color.png")).equals("texture"), "Missing texture");
            check(text(exported.toPath().resolveSibling("position.txt")).equals("GPS"), "Missing GPS");
            check(Files.isRegularFile(root.resolve("source/model.mtl.png")), "Source preview removed");
            noScratch(root);
            String archive = Exporter.compressModel(exported.getParentFile());
            try (ZipFile zip = new ZipFile(archive)) {
                check(zip.getEntry("scan.obj") != null && zip.getEntry("textures/color.png") != null, "Share lost nested texture");
            }
            Path imported = root.resolve("roundtrip.obj");
            check(IO.unzip(imported.toString(), new FileInputStream(archive)), "Deflated share archive could not be imported");
            check(text(imported.resolve("textures/color.png")).equals("texture"), "Roundtrip texture differs");
        });
        test("PLY export and duplicate scan refusal", root -> {
            File model = write(root.resolve("source/model.ply"), "cloud");
            File exported = Exporter.export(model, "scan");
            check(text(exported.toPath()).equals("cloud") && model.isFile(), "PLY not preserved");
            write(model.toPath(), "replacement");
            fails(() -> Exporter.export(model, "scan"));
            check(text(exported.toPath()).equals("cloud"), "Overwrote existing scan");
            noScratch(root);
        });
        test("export rejects escaping names, materials, textures, and symlinks", root -> {
            File model = write(root.resolve("source/model.obj"), "mtllib model.mtl\n");
            write(root.resolve("source/model.mtl"), "map_Kd ../secret\n");
            File secret = write(root.resolve("secret"), "private");
            fails(() -> Exporter.export(model, "../escape"));
            fails(() -> Exporter.export(model, "scan"));
            write(model.toPath(), "mtllib ../secret\n");
            fails(() -> Exporter.export(model, "scan"));
            check(Exporter.getMtlResource(model.toString()) == null, "Thumbnail lookup exposed unsafe material path");
            write(model.toPath(), "mtllib\n");
            fails(() -> Exporter.export(model, "scan"));
            write(model.toPath(), "mtllib model.mtl\n");
            write(root.resolve("source/model.mtl"), "map_Kd linked\n");
            Files.createSymbolicLink(root.resolve("source/linked"), secret.toPath());
            fails(() -> Exporter.export(model, "scan"));
            check(text(secret.toPath()).equals("private") && model.isFile(), "Damaged source or outside file");
            noScratch(root);
        });
        test("archive failure does not return a success path", root -> {
            Path model = Files.createDirectory(root.resolve("scan.obj"));
            write(model.resolve("scan.obj"), "mesh");
            Files.createSymbolicLink(model.resolve("missing.png"), root.resolve("missing"));
            fails(() -> Exporter.compressModel(model.toFile()));
            noScratch(root);
        });
        test("restructure copies shared resources for every model", root -> {
            Path library = AbstractActivity.root.toPath();
            for (String name : Arrays.asList("first.obj", "second.obj")) write(library.resolve(name), "mtllib shared.mtl\n");
            write(library.resolve("shared.mtl"), "map_Kd texture.png\n");
            write(library.resolve("texture.png"), "texture");
            write(library.resolve("position.txt"), "GPS");
            Exporter.makeStructure(library.toString());
            for (String name : Arrays.asList("first.obj", "second.obj")) {
                check(text(library.resolve(name).resolve("texture.png")).equals("texture"), "Shared texture lost");
                check(text(library.resolve(name).resolve("position.txt")).equals("GPS"), "Shared GPS lost");
            }
            check(Files.isRegularFile(library.resolve("texture.png")), "Removed existing resource");
            noScratch(root);
        });
        test("restructure partial resource copy failure retains original", root -> {
            Path library = AbstractActivity.root.toPath();
            write(library.resolve("scan.obj"), "mtllib model.mtl\n");
            write(library.resolve("model.mtl"), "map_Kd missing.png\n");
            Exporter.makeStructure(library.toString());
            check(Files.isRegularFile(library.resolve("scan.obj")), "Moved incomplete model");
            check(Files.isRegularFile(library.resolve("model.mtl")), "Removed material");
            noScratch(root);
        });
        test("restructure publication failure rolls model back", root -> restructureCollision(root, false));
        test("restructure failed rollback retains recoverable staging", root -> restructureCollision(root, true));
        finish();
    }
}
