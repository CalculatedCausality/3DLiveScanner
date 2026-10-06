# APK audit tools

Run from the repository root with Python 3. Choose the audit for the APK's flavor.

| Tool | Input and purpose |
| --- | --- |
| `check_quality_apk.py` | Quality APK: verify isolated ID/label and byte-identical original native libraries |
| `check_modern_apk.py` | Experimental modern APK: check native dependency isolation and static 16 KiB layout |
| `check_apk_16kb.py` | Native ELF/APK layout audit; see `--help` for inputs and options |

```sh
python3 tools/check_quality_apk.py path/to/app-quality-debug.apk --sdk path/to/android-sdk
python3 tools/check_modern_apk.py path/to/app-modern-debug.apk
python3 tools/check_apk_16kb.py --help
```

The quality audit requires the exact baseline APK in `artifacts/` and Android
build-tools 35.0.0's `aapt`. It defaults to the historical local SDK path;
pass `--sdk` when using another environment.

Original-library preservation and APK ZIP alignment do not make the quality
flavor 16 KiB-kernel compatible. Static audits do not verify signatures,
capture quality, smoothness or complete save/export behavior. See the
[build guide](../docs/scanner-build-variants.md) and
[current candidate](../docs/quality-modernization.md).
