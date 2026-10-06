import com.lvonasek.arcore3dscanner.ui.AbstractActivity;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

class TestSupport {
    interface Test { void run(Path root) throws Exception; }
    interface Action { void run() throws Exception; }
    static int passed;
    static int failed;

    static void test(String name, Test test) throws Exception {
        Path root = Files.createTempDirectory("storage-case-");
        AbstractActivity.root = Files.createDirectory(root.resolve("library")).toFile();
        try {
            test.run(root);
            passed++;
            System.out.println("PASS " + name);
        } catch (Throwable error) {
            failed++;
            System.err.println("FAIL " + name + ": " + error);
        } finally {
            try (java.util.stream.Stream<Path> paths = Files.walk(root)) {
                paths.sorted(Comparator.reverseOrder()).forEach(path -> {
                    try { Files.delete(path); } catch (IOException e) { throw new UncheckedIOException(e); }
                });
            }
        }
    }

    static void finish() {
        System.out.println(passed + " passed, " + failed + " failed");
        if (failed != 0) throw new AssertionError("Storage regressions failed");
    }

    static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    static void fails(Action action) throws Exception {
        try { action.run(); }
        catch (IOException | IllegalStateException expected) { return; }
        throw new AssertionError("Operation reported success instead of failure");
    }

    static File write(Path path, String text) throws IOException {
        Files.createDirectories(path.getParent());
        Files.write(path, text.getBytes(StandardCharsets.UTF_8));
        return path.toFile();
    }

    static String text(Path path) throws IOException {
        return new String(Files.readAllBytes(path), StandardCharsets.UTF_8);
    }

    static byte[] zip(String... pairs) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream out = new ZipOutputStream(bytes)) {
            for (int i = 0; i < pairs.length; i += 2) {
                byte[] data = pairs[i + 1].getBytes(StandardCharsets.UTF_8);
                ZipEntry entry = new ZipEntry(pairs[i]);
                CRC32 crc = new CRC32();
                crc.update(data);
                entry.setMethod(ZipEntry.STORED);
                entry.setSize(data.length);
                entry.setCrc(crc.getValue());
                out.putNextEntry(entry);
                out.write(data);
                out.closeEntry();
            }
        }
        return bytes.toByteArray();
    }

    static byte[] corrupt(byte[] zip, String payload) {
        byte[] needle = payload.getBytes(StandardCharsets.UTF_8);
        for (int i = 0; i <= zip.length - needle.length; i++) {
            boolean found = true;
            for (int j = 0; j < needle.length; j++) found &= zip[i + j] == needle[j];
            if (found) { zip[i] ^= 1; return zip; }
        }
        throw new AssertionError("Payload not found");
    }

    static void noScratch(Path root) throws IOException {
        try (java.util.stream.Stream<Path> paths = Files.walk(root)) {
            check(paths.noneMatch(path -> path.getFileName().toString().matches("\\.(copy|zip|import|storage)-.*")),
                    "Operation leaked staging artifacts");
        }
    }

    static boolean empty(Path root) throws IOException {
        try (java.util.stream.Stream<Path> paths = Files.list(root)) { return !paths.findAny().isPresent(); }
    }
}
