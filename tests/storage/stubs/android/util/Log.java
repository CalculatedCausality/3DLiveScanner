package android.util;

public final class Log {
    public static int d(String tag, String message) { return 0; }
    public static int e(String tag, String message) { return 0; }
    public static int e(String tag, String message, Throwable error) {
        System.err.println(message + ": " + error);
        return 0;
    }
}
