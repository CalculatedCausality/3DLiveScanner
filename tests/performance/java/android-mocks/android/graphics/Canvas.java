package android.graphics;

// Timestamp marker only; not a font rendering/placement test.
public class Canvas {
    final Bitmap bitmap;
    public Canvas(Bitmap value) { bitmap = value; }
    public void drawText(String text, float x, float y, Paint paint) { bitmap.pixels[0] = 0xffffffff; }
}
