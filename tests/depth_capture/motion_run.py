#!/usr/bin/env python3
"""Actual modern pose/origin methods and calibrated depth under known motion."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
def definition(path,signature):
    text=(ROOT/path).read_text();start=text.index(signature);opening=text.index('{',start);depth=0
    for end in range(opening,len(text)):
        depth+=(text[end]=='{')-(text[end]=='}')
        if not depth:return text[start:end+1]
    raise AssertionError(signature)
source=(ROOT/'tests/depth_capture/motion_test.cc.in').read_text()
source=source.replace('// PRODUCTION_ORIGIN',definition('common/arcore/arcore.cc','glm::mat4 ARCore::GetZeroTransform()'))
source=source.replace('// PRODUCTION_POSE',definition('common/arcore/service.cc','std::vector<glm::mat4> ARCoreService::GetPose(glm::mat4 projection, glm::mat4 view)'))
with tempfile.TemporaryDirectory(prefix='depth-motion-',dir='/tmp/opencode') as tmp:
    work=Path(tmp);(work/'test.cc').write_text(source)
    subprocess.run(['g++','-std=c++11','-O1','-g','-DSCANNER_MODERN=1','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(ROOT/'common'),'-I'+str(ROOT/'third_party/glm'),
                    str(work/'test.cc'),'-o',str(work/'test')],check=True,timeout=120)
    subprocess.run([str(work/'test')],check=True,timeout=45,
                   env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
    source=(ROOT/'tests/depth_capture/raw_source_test.cc.in').read_text()
    source=source.replace('// PRODUCTION_POINT',definition('common/arcore/arcore.cc','glm::vec4 ARCore::ToPoint('))
    source=source.replace('// PRODUCTION_CAPTURE',definition('common/arcore/arcore.cc','void ARCore::UpdateFeaturePoints()'))
    (work/'raw.cc').write_text(source)
    subprocess.run(['g++','-std=c++11','-O1','-g','-DSCANNER_MODERN=1','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(ROOT/'common'),'-I'+str(ROOT/'third_party/glm'),
                    str(work/'raw.cc'),'-o',str(work/'raw')],check=True,timeout=120)
    subprocess.run([str(work/'raw')],check=True,timeout=45,
                   env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
    source=(ROOT/'tests/depth_capture/uv_dimensions_test.cc.in').read_text()
    source=source.replace('// PRODUCTION_TRANSFORM',definition('common/arcore/camera.cc','glm::vec2 ARCoreCamera::Transform('))
    source=source.replace('// PRODUCTION_AXIS',definition('common/arcore/camera.cc','glm::ivec2 ARCoreCamera::Axis('))
    (work/'uv.cc').write_text(source)
    subprocess.run(['g++','-std=c++11','-O1','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(ROOT/'third_party/glm'),str(work/'uv.cc'),'-o',str(work/'uv')],check=True,timeout=120)
    subprocess.run([str(work/'uv')],check=True,timeout=30,
                   env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
    source=(ROOT/'tests/depth_capture/reference_test.cc.in').read_text()
    for marker,signature in [('REFERENCE','bool ARCore::UpdateReferenceFrame('),('ORIGIN','glm::mat4 ARCore::GetZeroTransform()'),('ANCHORS','std::vector<glm::vec3> ARCore::GetActiveAnchors()')]:
        source=source.replace('// PRODUCTION_'+marker,definition('common/arcore/arcore.cc',signature))
    (work/'reference.cc').write_text(source)
    subprocess.run(['g++','-std=c++11','-O1','-g','-DSCANNER_MODERN=1','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(ROOT/'common'),'-I'+str(ROOT/'third_party/glm'),
                    str(work/'reference.cc'),'-o',str(work/'reference')],check=True,timeout=120)
    subprocess.run([str(work/'reference')],check=True,timeout=45,
                   env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
    source=source[:source.index('int main()')]+(ROOT/'tests/depth_capture/source_fusion_test.cc.in').read_text()
    (work/'fusion.cc').write_text(source)
    subprocess.run(['g++','-std=c++11','-O1','-g','-DSCANNER_MODERN=1','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(ROOT/'common'),'-I'+str(ROOT/'third_party/glm'),
                    '-I'+str(ROOT/'tests/reconstruction'),'-I'+str(ROOT/'third_party/tango_3d_reconstruction/include'),
                    str(ROOT/'reconstruction/core.cc'),str(work/'fusion.cc'),'-o',str(work/'fusion')],check=True,timeout=180)
    subprocess.run([str(work/'fusion')],check=True,timeout=120,
                   env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
