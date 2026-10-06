package android.graphics;
import java.util.HashMap;
import java.util.Map;
public class BitmapFactory {
  public static class Options {
    public boolean inJustDecodeBounds;
    public int outWidth, outHeight, inSampleSize;
  }
  public static final Map<String, int[]> images = new HashMap<>();
  public static boolean failPixels;
  public static int maxDecodedDimension;
  public static Bitmap decodeFile(String path, Options options) {
    int[] dimensions = images.get(path);
    if (dimensions == null) { options.outWidth = options.outHeight = -1; return null; }
    options.outWidth = dimensions[0]; options.outHeight = dimensions[1];
    if (options.inJustDecodeBounds || failPixels) return null;
    int w = (dimensions[0] + options.inSampleSize - 1) / options.inSampleSize;
    int h = (dimensions[1] + options.inSampleSize - 1) / options.inSampleSize;
    maxDecodedDimension = Math.max(maxDecodedDimension, Math.max(w, h));
    return new Bitmap(w, h, path);
  }
}
