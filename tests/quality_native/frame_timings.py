#!/usr/bin/env python3
"""Check bounded timing windows, stage separation, lifecycle resets and snapshots."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
from java_tools import java_command, javac_command

SOURCE = ROOT / 'common/utils/com/lvonasek/gles/FrameTimings.java'
HARNESS = r'''
import com.lvonasek.gles.FrameTimings;
public class TimingTest {
  static long ms(long value) { return value * 1000000; }
  static void frame(FrameTimings t, long start, long nativeStart, long nativeEnd,
      long drawEnd, long swapEnd, boolean capturing, boolean ok) {
    t.recordNativeDraw(ms(nativeStart), ms(nativeEnd), capturing);
    t.recordFrame(ms(start), ms(drawEnd), ms(swapEnd), ok);
  }
  public static void main(String[] args) throws Exception {
    FrameTimings t = new FrameTimings();
    System.out.println(t.snapshot());
    frame(t, 1000, 1005, 1025, 1030, 1040, true, true);
    frame(t, 1100, 1110, 1140, 1150, 1160, true, true);
    frame(t, 1200, 1205, 1210, 1220, 1225, false, false);
    // A callback that skips JNI must not reuse the preceding frame's timing.
    t.recordFrame(ms(1250), ms(1260), ms(1270), true);
    System.out.println(t.snapshot());
    t.reset();
    for (int i = 0; i < 300; ++i) {
      long start = 1000 + 1000 * i;
      frame(t, start, start, start + i + 1, start + i + 2, start + i + 3, true, true);
    }
    System.out.println(t.snapshot());
    t.reset();
    System.out.println(t.snapshot());
    frame(t, 1000000, 1000001, 1000002, 1000003, 1000004, true, true);
    System.out.println(t.snapshot());
    // Race read-only snapshots against the GL-style writer. Each JSON must be
    // internally consistent even as the bounded ring wraps repeatedly.
    Thread writer = new Thread(() -> {
      for (int i = 0; i < 20000; ++i) {
        long start = 2000000 + 1000L * i;
        frame(t, start, start + 1, start + 2, start + 3, start + 4, i % 2 == 0, true);
      }
    });
    writer.start();
    for (int i = 0; i < 50; ++i) System.out.println(t.snapshot());
    writer.join();
    System.out.println(t.snapshot());
  }
}
'''

with tempfile.TemporaryDirectory(prefix='quality-timings-') as temp:
    work = Path(temp)
    harness = work / 'TimingTest.java'
    harness.write_text(HARNESS)
    subprocess.run(javac_command() + ['-d', str(work), str(SOURCE), str(harness)], check=True, timeout=30)
    output = subprocess.check_output(java_command() + ['-cp', str(work), 'TimingTest'], text=True, timeout=30)
    results = [json.loads(line) for line in output.splitlines()]

empty, stages, wrapped, reset, resumed = results[:5]
assert empty['frames_total'] == reset['frames_total'] == 0
assert empty['last_frame_age_ms'] == reset['last_frame_age_ms'] == -1
assert stages['all']['frames'] == 4
assert stages['all']['native_draw_ms']['samples'] == 3
assert stages['all']['swap_errors'] == stages['preview']['swap_errors'] == 1
capture = stages['capturing']
assert capture['frames'] == 2
assert capture['native_draw_ms']['mean'] == 25
assert capture['callback_ms']['mean'] == 40
assert capture['java_other_ms']['mean'] == 15
assert capture['egl_swap_ms']['mean'] == 10
assert capture['start_interval_ms']['samples'] == 1
assert capture['start_interval_ms']['mean'] == 100
assert capture['outside_frame_ms']['mean'] == 60
assert stages['preview']['start_interval_ms']['samples'] == 0
assert wrapped['frames_total'] == 300
assert wrapped['all']['frames'] == 256
assert wrapped['all']['native_draw_ms']['mean'] == 172.5
assert wrapped['all']['native_draw_ms']['p95'] == 288
assert wrapped['all']['native_draw_ms']['max'] == 300
assert wrapped['all']['native_draw_ms']['over_100ms'] == 200
assert resumed['all']['start_interval_ms']['samples'] == 0
assert resumed['all']['native_draw_ms']['samples'] == 1
for snapshot in results[5:]:
    assert snapshot['all']['frames'] <= 256
    assert snapshot['all']['frames'] == snapshot['capturing']['frames'] + snapshot['preview']['frames']
    assert snapshot['all']['native_draw_ms']['samples'] == snapshot['all']['frames']
assert results[-1]['frames_total'] == 20001
print('PASS frame timings: stage attribution, missing JNI, mode transitions, bounded wrap, reset and concurrent snapshots')
