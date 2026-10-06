package android.widget;
import android.graphics.Bitmap;
public class ImageView {
  public Bitmap bitmap;
  public void setImageBitmap(Bitmap bitmap) {
    if (bitmap.recycled) throw new AssertionError("Published a recycled bitmap");
    this.bitmap = bitmap;
  }
}
