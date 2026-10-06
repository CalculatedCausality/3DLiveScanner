package android.os;

// Deterministic clock; scheduling tests invoke actual Recorder code with it.
public class SystemClock {
    public static volatile long now = 1000;
    public static long elapsedRealtime() { return now; }
}
