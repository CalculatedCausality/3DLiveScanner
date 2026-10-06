package android.os;
import java.util.concurrent.ConcurrentLinkedQueue;
public class Handler {
  private static final ConcurrentLinkedQueue<Runnable> queue = new ConcurrentLinkedQueue<>();
  public Handler(Looper looper) {}
  public boolean post(Runnable runnable) { queue.add(runnable); return true; }
  public static void drain() {
    Runnable runnable;
    while ((runnable = queue.poll()) != null) runnable.run();
  }
}
