package com.lvonasek.utils;

import android.util.Log;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.List;
import java.util.UUID;
import java.util.Map;
import java.util.TreeMap;
import java.util.Arrays;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

public class IO {

    private static final int BUFFER_SIZE = 65536;
    private static final String TAG = "arcore_app";

    /** Compatibility entry point: failure must stop callers before they remove the source. */
    public static void copy(File src, File dst) {
        try {
            copyChecked(src, dst);
        } catch (IOException e) {
            throw new IllegalStateException("Unable to copy " + src + " to " + dst, e);
        }
    }

    public static void copyChecked(File src, File dst) throws IOException {
        copyToFile(new FileInputStream(src), dst);
    }

    /** Owns and closes input, including on failure. Publishes only after both streams close. */
    public static void copyToFile(InputStream input, File destination) throws IOException {
        File temporary = null;
        try {
            try (InputStream in = input) {
                if (in == null) throw new IOException("Missing input stream");
                temporary = File.createTempFile(".copy-", ".tmp", destination.getAbsoluteFile().getParentFile());
                try (OutputStream out = new BufferedOutputStream(new FileOutputStream(temporary))) {
                    transfer(in, out, new byte[BUFFER_SIZE]);
                }
            }
            replace(temporary, destination);
        } finally {
            if (temporary != null) temporary.delete(); // Only this operation's uncommitted file.
        }
    }

    private static void transfer(InputStream in, OutputStream out, byte[] buffer) throws IOException {
        int count;
        while ((count = in.read(buffer)) != -1) {
            // Some content providers return zero without reaching EOF. Do not spin or truncate.
            if (count == 0) {
                int value = in.read();
                if (value == -1) break;
                out.write(value);
            } else {
                out.write(buffer, 0, count);
            }
        }
    }

    /** Same-filesystem rename; never delete the destination to make a failed rename succeed.
     * This is failure-safe publication, not a power-loss durability guarantee. */
    private static void replace(File temporary, File destination) throws IOException {
        if (!temporary.renameTo(destination)) {
            throw new IOException("Unable to publish " + destination);
        }
    }

    public static File createStagingDirectory(File parent) throws IOException {
        File directory = new File(parent, ".storage-" + UUID.randomUUID());
        if (!directory.mkdir()) throw new IOException("Unable to create staging directory in " + parent);
        return directory;
    }

    /** Copy a quiescent directory across filesystems, verify bytes, then publish
     * by a same-filesystem directory rename. Never removes the source or replaces
     * an existing destination. This is not a power-loss-atomic directory protocol. */
    public static void publishDirectoryChecked(File source, File destination) throws IOException {
        File root = source.getCanonicalFile();
        File target = destination.getCanonicalFile();
        if (target.equals(root) || target.getPath().startsWith(root.getPath() + File.separator))
            throw new IOException("Publication destination is inside source");
        if (target.exists()) throw new IOException("Destination already exists: " + target);
        TreeMap<String, FileStamp> before = directorySnapshot(root);
        File staging = createStagingDirectory(target.getParentFile());
        try {
            for (Map.Entry<String, FileStamp> entry : before.entrySet()) {
                File input = resolveContainedFile(root, entry.getKey());
                File output = resolveContainedFile(staging, entry.getKey());
                if (entry.getValue().directory) {
                    if (!output.mkdirs() && !output.isDirectory())
                        throw new IOException("Unable to create " + output);
                } else {
                    // The sorted inventory creates every parent directory before its files.
                    copyVerified(input, output, entry.getValue().length);
                }
            }
            if (!before.equals(directorySnapshot(root))) throw new IOException("Source changed during publication");
            if (target.exists() || !staging.renameTo(target))
                throw new IOException("Unable to publish " + target);
        } finally {
            deleteRecursive(staging); // Only this operation's unpublished staging directory.
        }
    }

    private static final class FileStamp {
        final boolean directory;
        final long length;
        final long modified;
        FileStamp(File file) {
            directory = file.isDirectory();
            length = directory ? 0 : file.length();
            modified = file.lastModified();
        }
        @Override public boolean equals(Object other) {
            if (!(other instanceof FileStamp)) return false;
            FileStamp stamp = (FileStamp) other;
            return directory == stamp.directory && length == stamp.length && modified == stamp.modified;
        }
        @Override public int hashCode() {
            return (directory ? 1 : 0) ^ Long.valueOf(length).hashCode() ^ Long.valueOf(modified).hashCode();
        }
    }

    private static TreeMap<String, FileStamp> directorySnapshot(File root) throws IOException {
        if (!root.isDirectory()) throw new IOException("Missing source directory: " + root);
        TreeMap<String, FileStamp> snapshot = new TreeMap<>();
        ArrayList<File> folders = new ArrayList<>();
        folders.add(root);
        for (int index = 0; index < folders.size(); index++) {
            File[] children = folders.get(index).listFiles();
            if (children == null) throw new IOException("Unable to enumerate source directory");
            for (File child : children) {
                // Do not follow aliases, escapes or directory cycles into unrelated data.
                if (!child.getCanonicalFile().equals(child.getAbsoluteFile()))
                    throw new IOException("Linked source entry: " + child);
                if (!child.isFile() && !child.isDirectory())
                    throw new IOException("Unsupported source entry: " + child);
                String relative = child.getPath().substring(root.getPath().length() + 1);
                snapshot.put(relative, new FileStamp(child));
                if (child.isDirectory()) folders.add(child);
            }
        }
        return snapshot;
    }

    private static MessageDigest sha256() {
        try { return MessageDigest.getInstance("SHA-256"); }
        catch (NoSuchAlgorithmException impossible) { throw new IllegalStateException(impossible); }
    }

    private static void copyVerified(File source, File destination, long expectedLength) throws IOException {
        MessageDigest written = sha256();
        byte[] buffer = new byte[BUFFER_SIZE];
        long length = 0;
        try (InputStream in = new BufferedInputStream(new FileInputStream(source));
             FileOutputStream file = new FileOutputStream(destination);
             BufferedOutputStream out = new BufferedOutputStream(file)) {
            int count;
            while ((count = in.read(buffer)) != -1) {
                if (count == 0) continue;
                out.write(buffer, 0, count);
                written.update(buffer, 0, count);
                length += count;
            }
            out.flush();
            file.getFD().sync();
        }
        if (length != expectedLength) throw new IOException("Source size changed during copy");
        MessageDigest readback = sha256();
        try (InputStream in = new BufferedInputStream(new FileInputStream(destination))) {
            int count;
            while ((count = in.read(buffer)) != -1) readback.update(buffer, 0, count);
        }
        if (destination.length() != expectedLength || !Arrays.equals(written.digest(), readback.digest()))
            throw new IOException("Published copy verification failed");
    }

    /** Resolve archive/model references against the canonical root, including symlink parents. */
    public static File resolveContainedFile(File root, String name) throws IOException {
        if (name == null || name.isEmpty() || new File(name).isAbsolute()
                || name.indexOf('\\') >= 0 || name.indexOf(':') >= 0 || name.indexOf('\0') >= 0) {
            throw new IOException("Invalid relative file name: " + name);
        }
        File canonicalRoot = root.getCanonicalFile();
        File file = new File(canonicalRoot, name).getCanonicalFile();
        String prefix = canonicalRoot.getPath();
        if (!prefix.endsWith(File.separator)) prefix += File.separator;
        if (file.equals(canonicalRoot) || !file.getPath().startsWith(prefix)) {
            throw new IOException("File escapes directory: " + name);
        }
        return file;
    }

    public static void deleteRecursive(File fileOrDirectory) {
        try {
            if (fileOrDirectory.isDirectory())
                for (File child : fileOrDirectory.listFiles())
                    deleteRecursive(child);

            if (fileOrDirectory.delete())
                Log.d(TAG, fileOrDirectory + " deleted");
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    /** Imports into an absent or empty directory only. Existing scans are never merged/overwritten.
     * The input is owned by this method. A failed import leaves the destination untouched. */
    public static boolean unzip(String path, InputStream input) {
        File staging = null;
        File archive = null;
        try {
            File destination;
            try (InputStream in = input) {
                if (in == null) throw new IOException("Missing archive stream");
                destination = new File(path).getCanonicalFile();
                requireEmptyDestination(destination);
                File parent = destination.getParentFile();
                staging = createStagingDirectory(parent);
                archive = File.createTempFile(".import-", ".zip", parent);
                try (OutputStream out = new BufferedOutputStream(new FileOutputStream(archive))) {
                    transfer(in, out, new byte[BUFFER_SIZE]);
                }
            }
            // ZipFile requires a central directory; ZipInputStream alone accepts truncated archives
            // (and even arbitrary non-ZIP input) as a successful end of stream.
            try (ZipFile zip = new ZipFile(archive)) {
                byte[] buffer = new byte[BUFFER_SIZE];
                HashSet<String> entries = new HashSet<>();
                Enumeration<? extends ZipEntry> items = zip.entries();
                while (items.hasMoreElements()) {
                    ZipEntry entry = items.nextElement();
                    File file = resolveContainedFile(staging, entry.getName());
                    if (!entries.add(file.getPath())) throw new IOException("Duplicate ZIP entry: " + entry.getName());
                    File directory = entry.isDirectory() ? file : file.getParentFile();
                    if (!directory.isDirectory() && !directory.mkdirs()) {
                        throw new IOException("Unable to create " + directory);
                    }
                    CRC32 crc = new CRC32();
                    long size = 0;
                    try (InputStream in = zip.getInputStream(entry);
                         OutputStream out = entry.isDirectory() ? null : new BufferedOutputStream(new FileOutputStream(file))) {
                        int count;
                        while ((count = in.read(buffer)) != -1) {
                            if (out != null) out.write(buffer, 0, count);
                            crc.update(buffer, 0, count);
                            size += count;
                        }
                    }
                    // ZipFile streams do not themselves verify CRC on every supported runtime.
                    if (size != entry.getSize() || crc.getValue() != entry.getCrc()) {
                        throw new IOException("Corrupt ZIP entry: " + entry.getName());
                    }
                }
            }
            requireEmptyDestination(destination);
            replace(staging, destination);
            return true;
        } catch (Exception e) {
            e.printStackTrace();
            return false;
        } finally {
            if (archive != null) archive.delete();
            if (staging != null) deleteRecursive(staging);
        }
    }

    private static void requireEmptyDestination(File destination) throws IOException {
        if (destination.exists()) {
            String[] children = destination.list();
            if (children == null || children.length != 0) {
                throw new IOException("Import destination is not empty: " + destination);
            }
        }
    }

    /** Retains the legacy flat archive format and rejects ambiguous duplicate basenames. */
    public static void zip(List<String> files, String zip) throws Exception {
        ArrayList<String> names = new ArrayList<>();
        for (String file : files) names.add(new File(file).getName());
        writeZip(files, names, new File(zip));
    }

    /** Preserve nested model resources; flat models keep exactly the same ZIP entry names. */
    public static void zipDirectory(File directory, String zip) throws IOException {
        File root = directory.getCanonicalFile();
        File destination = new File(zip).getCanonicalFile();
        ArrayList<String> files = new ArrayList<>();
        ArrayList<String> names = new ArrayList<>();
        for (Map.Entry<String, FileStamp> entry : directorySnapshot(root).entrySet()) {
            File source = resolveContainedFile(root, entry.getKey());
            if (source.equals(destination)) throw new IOException("Archive is inside its source directory");
            if (!entry.getValue().directory) {
                files.add(source.getPath());
                names.add(entry.getKey().replace(File.separatorChar, '/'));
            }
        }
        writeZip(files, names, destination);
    }

    private static void writeZip(List<String> files, List<String> names, File destination) throws IOException {
        HashSet<String> unique = new HashSet<>();
        for (int i = 0; i < files.size(); i++) {
            File source = new File(files.get(i));
            if (!source.isFile() || source.getCanonicalFile().equals(destination.getCanonicalFile())) {
                throw new IOException("Invalid ZIP source: " + source);
            }
            if (!unique.add(names.get(i))) throw new IOException("Duplicate ZIP entry: " + names.get(i));
        }
        File temporary = File.createTempFile(".zip-", ".tmp", destination.getAbsoluteFile().getParentFile());
        try {
            try (ZipOutputStream out = new ZipOutputStream(new BufferedOutputStream(new FileOutputStream(temporary)))) {
                out.setLevel(9);
                byte[] buffer = new byte[BUFFER_SIZE];
                for (int i = 0; i < files.size(); i++) {
                    try (InputStream in = new BufferedInputStream(new FileInputStream(files.get(i)))) {
                        out.putNextEntry(new ZipEntry(names.get(i)));
                        transfer(in, out, buffer);
                        out.closeEntry();
                    }
                }
            }
            replace(temporary, destination);
        } finally {
            temporary.delete();
        }
    }
}
