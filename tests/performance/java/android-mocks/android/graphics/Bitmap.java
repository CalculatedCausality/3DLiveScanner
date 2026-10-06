package android.graphics;

// Array-backed boundary. Does not emulate Android alpha premultiplication/native pixels.
public class Bitmap {
    public enum Config { ARGB_8888 }
    public static int allocations, recycled;
    public final int width, height;
    public final int[] pixels;
    private boolean dead;

    private Bitmap(int w, int h) {
        width = w;
        height = h;
        pixels = new int[w * h];
        ++allocations;
    }

    public static Bitmap createBitmap(int w, int h, Config c) { return new Bitmap(w, h); }
    public int getWidth() { return width; }
    public int getHeight() { return height; }

    public void setPixels(int[] src, int offset, int stride, int x, int y, int w, int h) {
        if (dead) throw new AssertionError("recycled bitmap used");
        for (int row = 0; row < h; row++)
            System.arraycopy(src, offset + row * stride, pixels, (y + row) * width + x, w);
    }

    public void getPixels(int[] dst, int offset, int stride, int x, int y, int w, int h) {
        if (dead) throw new AssertionError("recycled bitmap used");
        for (int row = 0; row < h; row++)
            System.arraycopy(pixels, (y + row) * width + x, dst, offset + row * stride, w);
    }

    public void recycle() {
        if (dead) throw new AssertionError("double recycle");
        dead = true;
        ++recycled;
    }
}
