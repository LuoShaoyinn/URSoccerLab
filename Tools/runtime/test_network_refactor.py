#!/usr/bin/env python3
"""Exercise isolated TCP/state/camera/admin pipelines in the source runtime.

Never cooks or rebuilds the AppImage. Run with py_example/.venv/bin/python.
Tests JPEG and raw streaming, two robot ports, multiple clients, commands,
admin operations, malformed/fragmented requests, and reconnects.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "py_example/src"))
from ursoccerlab.gains import detect_gains
from ursoccerlab.media import camera_to_rgb
from ursoccerlab.tcp import AdminClient, FrameConn, RobotClient
from PIL import Image


def stop(process: subprocess.Popen) -> None:
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()


def receive_json(connection: FrameConn) -> dict:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        for kind, payload in connection.recv_frames():
            assert kind == 0
            return json.loads(payload)
        time.sleep(0.002)
    raise TimeoutError("admin reply not received")


def run(ue: Path, output: Path, codec: str) -> dict:
    output.mkdir(parents=True, exist_ok=True)
    original = ROOT / "py_example/examples/standing/scene.json"
    config = json.loads(original.read_text())
    config["field"]["map_image"] = str((original.parent / config["field"]["map_image"]).resolve())
    # Small raw frames keep this a protocol/behavior test rather than a bandwidth benchmark.
    config["camera_freq"] = 12
    config["vision"] = {"rgb": {"compression": codec, "rate_hz": 12, "jpeg_quality": 85}}
    config["render"]["resolution_x"] = 640
    config["render"]["resolution_y"] = 480
    config["robots"][0]["privilege"] = {"all_pos": True}
    scene = output / "scene.json"
    scene.write_text(json.dumps(config, indent=2) + "\n")
    ue_log = output / "ue.log"
    ue_log.unlink(missing_ok=True)
    environment = dict(os.environ)
    environment["LD_LIBRARY_PATH"] = str(ROOT / "Plugins/UnrealRoboticsLab/Binaries/Linux") + ":" + environment.get("LD_LIBRARY_PATH", "")
    command = [sys.executable, str(ROOT / "Tools/runtime/run_scene.py"), "--ue", str(ue),
               "--scene-config", str(scene), "--sim-extra-arg=-FORCELOGFLUSH",
               f"--sim-extra-arg=-abslog={ue_log}"]
    peers = []
    with (output / "launcher.log").open("w") as log:
        process = subprocess.Popen(command, cwd=ROOT, env=environment, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            deadline = time.monotonic() + 75
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f"runtime exited; see {output}")
                if ue_log.exists() and " listening on port 10001" in ue_log.read_text(errors="replace"):
                    break
                time.sleep(0.1)
            else:
                raise TimeoutError(f"listeners not ready; see {ue_log}")
            admin = AdminClient("127.0.0.1")
            other_admin = AdminClient("127.0.0.1")
            peers.extend((admin, other_admin))
            first_pose = admin.get_pose("robot_rp0")
            assert first_pose["ok"], first_pose
            assert other_admin.get_pose("robot_rp1")["ok"]
            # Admin sockets are multiplexed independently and dispatch safely to Unreal/MuJoCo.
            fragment = FrameConn("127.0.0.1", 11000)
            peers.append(fragment)
            payload = json.dumps({"command": "get_pose", "args": {"actor_id": "robot_rp0"}}).encode()
            frame = struct.pack(">IB", len(payload) + 1, 0) + payload
            fragment.sock.setblocking(True)
            fragment.sock.sendall(frame[:2])
            time.sleep(0.02)
            fragment.sock.sendall(frame[2:])
            fragment.sock.setblocking(False)
            assert receive_json(fragment)["ok"]
            # Malformed framing must close only this admin connection.
            invalid = socket.create_connection(("127.0.0.1", 11000), timeout=3)
            invalid.sendall(b"\xff\xff\xff\xff\x00")
            try:
                assert invalid.recv(1) == b""
            except ConnectionResetError:
                pass
            finally:
                invalid.close()
            assert admin.get_pose("robot_rp0")["ok"]
            for client, actor in ((admin, "robot_rp0"), (other_admin, "robot_rp1")):
                assert client.lock_pose(actor)["ok"]
                assert client.unlock_pose(actor)["ok"]
            assert admin.reset("robot_rp0")["ok"]
            assert other_admin.reset("robot_rp1")["ok"]
            assert not admin._request("set_pose", {"actor_id": "robot_rp0", "joint_qpos": [99]})["ok"]
            # Open video clients only once the admin probes are complete. A raw
            # video client intentionally left unread would hit TCP backpressure.
            robots = [RobotClient("127.0.0.1", port) for port in (10000, 10001, 10000)]
            peers.extend(robots)
            counts = [{"states": 0, "frames": 0, "sim_start": None, "sim_end": None} for _ in robots]
            targets = [None, None]
            observed_commands = [False, False]
            cameras = [set() for _ in robots]
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                for index, robot in enumerate(robots):
                    for kind, data in robot.recv():
                        if kind == "state":
                            counts[index]["states"] += 1
                            if counts[index]["sim_start"] is None:
                                counts[index]["sim_start"] = data["sim_time"]
                            counts[index]["sim_end"] = data["sim_time"]
                            if index < 2:
                                if targets[index] is None:
                                    names = list(data.get("actuators", {}))
                                    assert names
                                    robot.set_controller_params(**detect_gains(names), actuator_mode="position")
                                    targets[index] = {name: 0.025 if index == 0 else -0.025 for name in names}
                                values = list(data.get("actuators", {}).values())
                                wanted = 0.025 if index == 0 else -0.025
                                observed_commands[index] |= any(abs(value - wanted) < 1e-5 for value in values)
                        elif kind in ("rgb", "camera"):
                            for camera in data:
                                assert camera["codec"] == codec, camera["codec"]
                                rgb = camera_to_rgb(camera)
                                assert rgb.size > 0
                                counts[index]["frames"] += 1
                                cameras[index].add(camera["camera_name"])
                                if counts[index]["frames"] == 1:
                                    Image.fromarray(rgb).save(output / f"client_{index}.png")
                    if index < 2 and targets[index]:
                        robot.send_command(targets[index])
                time.sleep(0.01)
            for count, names in zip(counts, cameras):
                assert count["states"] > 10 and count["frames"] >= 2, counts
                assert count["sim_end"] > count["sim_start"], count
                assert len(names) == 2, names
            assert all(observed_commands), observed_commands
            # Reconnect after video traffic; the other robot remains reachable.
            robots[2].close()
            replacement = RobotClient("127.0.0.1", 10000)
            peers.append(replacement)
            deadline = time.monotonic() + 5
            received = set()
            while time.monotonic() < deadline and received != {"state", "rgb"}:
                for kind, data in replacement.recv():
                    if kind in ("state", "rgb"):
                        received.add(kind)
                time.sleep(0.01)
            assert received == {"state", "rgb"}, received
            assert other_admin.get_pose("robot_rp1")["ok"]
            result = {"codec": codec, "clients": counts, "commands_applied": observed_commands,
                      "camera_names": [sorted(names) for names in cameras],
                      "admin_fragmentation": True, "admin_malformed_isolation": True,
                      "admin_operations": True, "reconnect": True}
            (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
            return result
        finally:
            for peer in peers:
                peer.close()
            stop(process)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ue", type=Path, default=Path.home() / "software/Unreal_Engine_5.7.4/Engine/Binaries/Linux/UnrealEditor")
    parser.add_argument("--out", type=Path, default=ROOT / "Saved/Tests/network-refactor-runtime")
    args = parser.parse_args()
    for codec in ("jpeg", "raw"):
        result = run(args.ue, args.out.resolve() / codec, codec)
        print(json.dumps(result), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
