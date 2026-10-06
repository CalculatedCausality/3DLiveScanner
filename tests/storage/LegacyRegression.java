import com.lvonasek.arcore3dscanner.main.Exporter;
import com.lvonasek.arcore3dscanner.ui.AbstractActivity;
import com.lvonasek.utils.IO;
import java.io.*;
import java.nio.file.*;
import java.util.*;

/** Uses only pre-existing public APIs so the same assertions also run against git HEAD. */
public class LegacyRegression extends TestSupport {
    public static void main(String[] args) throws Exception {
        test("copying a file to itself preserves its bytes", root -> {
            File file = write(root.resolve("scan.ply"), "original scan");
            IO.copy(file, file);
            check(text(file.toPath()).equals("original scan"), "Self-copy truncated scan");
        });
        test("copy failure is propagated", root -> {
            File destination = write(root.resolve("scan.ply"), "existing scan");
            fails(() -> IO.copy(root.resolve("missing").toFile(), destination));
            check(text(destination.toPath()).equals("existing scan"), "Changed destination");
        });
        test("failed ZIP preserves previous archive", root -> {
            File source = write(root.resolve("scan.obj"), "mesh");
            File archive = write(root.resolve("share.zip"), "previous archive");
            fails(() -> IO.zip(Arrays.asList(source.toString(), root.resolve("missing").toString()), archive.toString()));
            check(text(archive.toPath()).equals("previous archive"), "Truncated previous archive");
        });
        test("ZIP sibling-prefix escape is rejected", root -> {
            Path destination = Files.createDirectory(root.resolve("import"));
            byte[] archive = zip("../import-escape/model.obj", "escaped");
            check(!IO.unzip(destination.toString(), new ByteArrayInputStream(archive)), "Accepted path escape");
            check(!Files.exists(root.resolve("import-escape/model.obj")), "Wrote outside import directory");
        });
        test("non-ZIP input is not successful import", root -> {
            Path destination = Files.createDirectory(root.resolve("import"));
            check(!IO.unzip(destination.toString(), new ByteArrayInputStream(new byte[]{1, 2, 3})), "Accepted garbage");
        });
        test("missing central directory is rejected", root -> {
            Path destination = Files.createDirectory(root.resolve("import"));
            byte[] archive = zip("scan.obj", "mesh");
            int end = -1;
            for (int i = 0; i < archive.length - 3; i++) {
                if (archive[i] == 'P' && archive[i + 1] == 'K' && archive[i + 2] == 1 && archive[i + 3] == 2) { end = i; break; }
            }
            check(end > 0, "No central directory fixture");
            check(!IO.unzip(destination.toString(), new ByteArrayInputStream(Arrays.copyOf(archive, end))), "Accepted truncated ZIP");
            check(empty(destination), "Left partial model visible");
        });
        test("CRC failure rolls back all imported files", root -> {
            Path destination = Files.createDirectory(root.resolve("import"));
            byte[] archive = corrupt(zip("scan.obj", "mesh", "texture.png", "TEXTURE_PAYLOAD"), "TEXTURE_PAYLOAD");
            check(!IO.unzip(destination.toString(), new ByteArrayInputStream(archive)), "Accepted CRC mismatch");
            check(empty(destination), "Left partial imported files");
        });
        test("import refuses to overwrite existing scan", root -> {
            Path destination = Files.createDirectory(root.resolve("import"));
            write(destination.resolve("scan.obj"), "existing scan");
            check(!IO.unzip(destination.toString(), new ByteArrayInputStream(zip("scan.obj", "replacement"))), "Overwrote existing scan");
            check(text(destination.resolve("scan.obj")).equals("existing scan"), "Lost existing model");
        });
        test("missing texture prevents export and preserves source", root -> {
            File model = write(root.resolve("source/model.obj"), "mtllib model.mtl\nv 0 0 0\n");
            write(root.resolve("source/model.mtl"), "map_Kd texture.png\n");
            fails(() -> Exporter.export(model, "scan"));
            check(model.isFile() && Files.isRegularFile(root.resolve("source/model.mtl")), "Moved source before complete export");
            check(!new File(AbstractActivity.root, "scan.obj").exists(), "Published incomplete scan");
            noScratch(root);
        });
        test("restructure preserves pre-existing temp directory", root -> {
            Path library = AbstractActivity.root.toPath();
            write(library.resolve("temp/recoverable-scan"), "user data");
            write(library.resolve("scan.ply"), "point cloud");
            Exporter.makeStructure(library.toString());
            check(text(library.resolve("temp/recoverable-scan")).equals("user data"), "Deleted prior temp scan");
            check(text(library.resolve("scan.ply/scan.ply")).equals("point cloud"), "Failed restructuring");
        });
        test("share archives are independent snapshots", root -> {
            File model = write(root.resolve("model/scan.obj"), "first");
            String first = Exporter.compressModel(model.getParentFile());
            byte[] snapshot = Files.readAllBytes(Paths.get(first));
            write(model.toPath(), "second version");
            String second = Exporter.compressModel(model.getParentFile());
            check(!first.equals(second), "Reused in-flight share path");
            check(Arrays.equals(snapshot, Files.readAllBytes(Paths.get(first))), "Overwrote earlier share");
        });
        finish();
    }
}
