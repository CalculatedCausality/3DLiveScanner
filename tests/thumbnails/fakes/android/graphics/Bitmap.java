package android.graphics;
import java.io.IOException;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
public class Bitmap {
  public enum CompressFormat { JPEG }
  public static boolean failCompression;
  public static boolean exhaustScaling;
  public static final List<Bitmap> allocated = new ArrayList<>();
  public final String label;
  private final int width, height;
  public boolean recycled;
  public Bitmap(int width, int height, String label) {
    this.width = width; this.height = height; this.label = label;
    synchronized (allocated) { allocated.add(this); }
  }
  public int getWidth() { return width; }
  public int getHeight() { return height; }
  public int getAllocationByteCount() { return width * height * 4; }
  public void recycle() {
    if (recycled) throw new AssertionError("Double recycle");
    recycled = true;
  }
  public static Bitmap createBitmap(Bitmap source, int x, int y, int w, int h) {
    if (source.recycled) throw new AssertionError("Recycled source");
    if (x == 0 && y == 0 && w == source.width && h == source.height) return source;
    return new Bitmap(w, h, source.label);
  }
  public static Bitmap createScaledBitmap(Bitmap source, int w, int h, boolean filter) {
    if (source.recycled) throw new AssertionError("Recycled source");
    if (exhaustScaling) throw new OutOfMemoryError("Injected bitmap allocation failure");
    if (w == source.width && h == source.height) return source;
    return new Bitmap(w, h, source.label);
  }
  public boolean compress(CompressFormat format, int quality, OutputStream output) {
    try { output.write(new byte[]{1, 2, 3, 4}); }
    catch (IOException error) { return false; }
    return !failCompression;
  }
}
