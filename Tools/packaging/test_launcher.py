"""Validate the packaged argument boundary and atlas without running Unreal."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import os
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'Tools/runtime'))
from ndisplay_config import write_ndisplay_config


class LauncherTests(unittest.TestCase):
    def test_arguments_and_layout(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); app=root/'app'; (app/'usr/bin').mkdir(parents=True)
            (app/'usr/share/ursoccerlab').mkdir(parents=True)
            (app/'URSoccerLab/Binaries/Linux').mkdir(parents=True)
            shutil.copy2(ROOT/'Tools/packaging/AppRun', app/'AppRun'); (app/'AppRun').chmod(0o755)
            shutil.copy2(ROOT/'Tools/packaging/atlas.jq',app/'usr/share/ursoccerlab/atlas.jq')
            (app/'usr/bin/jq').symlink_to(shutil.which('jq'))
            executable=app/'URSoccerLab/Binaries/Linux/URSoccerLab'
            executable.write_text('#!/bin/sh\nfor arg in "$@"; do printf "%s\\n" "$arg"; done\n'); executable.chmod(0o755)
            scene=root/'scene with spaces.json'
            env=dict(os.environ,XDG_DATA_HOME=str(root/'data'))
            for mode in ['stereo_rgb','rgbd']:
                config={'robots':[{'actor_id':'r'}],'vision':{'mode':mode},'guest_inspector':{'max_guests':2,'width':1280,'height':720}}
                scene.write_text(json.dumps(config,separators=(',',':')))
                r=subprocess.run([str(app/'AppRun'),str(scene)],env=env,capture_output=True,text=True)
                self.assertEqual(r.returncode,0,r.stderr)
                argv=r.stdout.splitlines();self.assertIn('-URSSceneConfig='+str(scene),argv)
                path=next(a.split('=',1)[1] for a in argv if a.startswith('-dc_cfg='))
                actual=json.loads(Path(path).read_text());expected=root/'expected.json'
                write_ndisplay_config(2 if mode=='stereo_rgb' else 1,expected,config['guest_inspector'])
                a=actual['nDisplay']['cluster']['nodes']['node_0'];b=json.loads(expected.read_text())['nDisplay']['cluster']['nodes']['node_0']
                self.assertEqual(a['viewports'],b['viewports']);self.assertEqual(a['window'],b['window'])
                for args in [[],[str(scene),'-NoSound'],['-URSSceneConfig='+str(scene)]]:
                    self.assertEqual(subprocess.run([str(app/'AppRun'),*args],env=env,capture_output=True).returncode,2)
            scene.write_text('{invalid')
            self.assertEqual(subprocess.run([str(app/'AppRun'),str(scene)],env=env,capture_output=True).returncode,2)


if __name__=='__main__':unittest.main()
