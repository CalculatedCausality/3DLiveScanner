# Thumbnail loader checks

```sh
export JAVA_HOME=/path/to/jdk17
export ANDROID_SDK_ROOT=/path/to/android-sdk
python3 tests/thumbnails/run.py \
  --android-jar "$ANDROID_SDK_ROOT/platforms/android-33/android.jar"
```

Runs the production loader with real bounded executor threads and filesystem operations.
Controlled Android boundaries allow deterministic blocked decodes, recycled rows, folder
changes, shutdown, cache eviction, failed compression, and null/corrupt image results.
Allocation-exhaustion checks cover worker/slot survival, subsequent good requests,
refresh recovery of the failed path, and recycling of partially transformed bitmaps.
Checks request coalescing, absolute-path identity, cache-hit short circuit, eventual service
under queue pressure, byte budgets, and bitmap ownership (including no-op transform aliases).
The optional second compile uses the real Android SDK types, with only the two application
dependencies stubbed. Host bitmap fakes verify sampling/ownership and publication logic;
they do not validate Android JPEG codecs or on-device scrolling/rendering.
