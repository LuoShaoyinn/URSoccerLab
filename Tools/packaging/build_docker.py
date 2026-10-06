#!/usr/bin/env python3
"""Build, cook and package the simulator using Docker (or its Podman CLI shim)."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
ENGINE = Path(os.environ.get('URS_UE', str(Path.home()/'software/Unreal_Engine_5.7.4'))).resolve()
IMAGE = 'ursoccerlab-packager:24.04'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", nargs="?", default="all", choices=["all", "cook", "appdir", "image"])
    args = parser.parse_args()
    if not (ENGINE/'Engine/Build/BatchFiles/RunUAT.sh').is_file():
        raise SystemExit('Set URS_UE to your Unreal Engine 5.7.4 directory.')
    docker = shutil.which('docker')
    if not docker: raise SystemExit('Docker is required.')
    with open(docker, 'rb') as stream:
        podman = b'podman' in stream.read(1024)
    command = [docker]
    env = dict(os.environ)
    cache = Path.home()/'.cache/urs-containers'
    if podman:
        temporary = cache/'tmp'; temporary.mkdir(parents=True, exist_ok=True)
        env['TMPDIR'] = str(temporary)
        command += ['--storage-driver', 'overlay', '--root', str(cache/'overlay'),
                    '--runroot', str(Path(os.environ.get('XDG_RUNTIME_DIR', '/tmp'))/'urs-containers-overlay')]
    subprocess.run(command+['build', '-t', IMAGE, str(ROOT/'Tools/packaging')], env=env, check=True)
    user_home = cache/'user'; user_home.mkdir(parents=True, exist_ok=True)
    run = command+['run', '--rm']
    if podman: run += ['--userns=keep-id']
    run += ['--user', f'{os.getuid()}:{os.getgid()}', '--env', 'HOME=/home/urs',
            '--env', f'URS_UE={ENGINE}', '--volume', f'{user_home}:/home/urs',
            '--volume', f'{ROOT}:{ROOT}', '--volume', f'{ENGINE}:{ENGINE}',
            '--workdir', str(ROOT), IMAGE, 'python3', 'Tools/packaging/package_appimage.py', args.phase]
    subprocess.run(run, env=env, check=True)


if __name__ == '__main__': main()
