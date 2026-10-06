package android.content;

// Host fixture: silence recording sounds; gallery/audio paths are not exercised.
public class Context {
    public Object getSystemService(String name) { return new android.media.AudioManager(); }
}
