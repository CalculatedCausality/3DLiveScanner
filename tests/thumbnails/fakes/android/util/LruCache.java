package android.util;
import java.util.LinkedHashMap;
import java.util.Map;
public class LruCache<K, V> {
  private final LinkedHashMap<K, V> values = new LinkedHashMap<>(16, .75f, true);
  private final int max;
  private int bytes;
  public LruCache(int max) { this.max = max; }
  protected int sizeOf(K key, V value) { return 1; }
  public V get(K key) { return values.get(key); }
  public V put(K key, V value) {
    V old = values.put(key, value);
    bytes += sizeOf(key, value) - (old == null ? 0 : sizeOf(key, old));
    while (bytes > max && !values.isEmpty()) {
      Map.Entry<K, V> first = values.entrySet().iterator().next();
      bytes -= sizeOf(first.getKey(), first.getValue());
      values.remove(first.getKey());
    }
    return old;
  }
  public void evictAll() { values.clear(); bytes = 0; }
  public int size() { return bytes; }
}
