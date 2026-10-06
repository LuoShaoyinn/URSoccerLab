"""Generate the local single-node nDisplay atlas used by URSoccerLab."""

from __future__ import annotations

import json
import math
from pathlib import Path


def write_ndisplay_config(view_count: int, output: Path, guest_config: dict | None = None) -> tuple[int, int]:
    """Reserve robot and guest 640x480 nDisplay views; return atlas dimensions."""
    guest = guest_config or {}
    guest_count = guest.get("max_guests", 4) if guest.get("enabled", True) else 0
    guest_width, guest_height = guest.get("width", 640), guest.get("height", 480)
    if not isinstance(guest_count, int) or isinstance(guest_count, bool) or not 0 <= guest_count <= 4:
        raise ValueError("guest_inspector.max_guests must be an integer in [1,4]")
    if any(not isinstance(n, int) or isinstance(n, bool) or n % 2 or n < 64 or n > maximum for n, maximum in ((guest_width, 1920), (guest_height, 1080))):
        raise ValueError("guest_inspector requires even width [64,1920] and height [64,1080]")
    cell_width, cell_height = max(640, guest_width), max(480, guest_height)
    robot_count = view_count
    view_count += guest_count
    columns = math.ceil(math.sqrt(view_count * 4 / 3))
    rows = math.ceil(view_count / columns)
    viewports = {}
    for index in range(view_count):
        viewports[f"camera_{index:02d}" if index < robot_count else f"guest_{index-robot_count:02d}"] = {
            "camera": "DefaultViewPoint",
            "bufferRatio": 1,
            "gPUIndex": -1,
            "allowCrossGPUTransfer": False,
            "isShared": False,
            "region": {
                "x": (index % columns) * cell_width,
                "y": (index // columns) * cell_height,
                "w": 640 if index < robot_count else guest_width,
                "h": 480 if index < robot_count else guest_height,
            },
            "projectionPolicy": {"type": "camera", "parameters": {}},
        }

    width = columns * cell_width
    height = rows * cell_height
    config = {
        "nDisplay": {
            "description": f"URS production {view_count}-camera atlas",
            "version": "5.00",
            "assetPath": "",
            "misc": {
                "bFollowLocalPlayerCamera": False,
                "bExitOnEsc": True,
                "bOverrideViewportsFromExternalConfig": True,
                "bOverrideTransformsFromExternalConfig": True,
            },
            "scene": {
                "xforms": {},
                "cameras": {
                    "DefaultViewPoint": {
                        "interpupillaryDistance": 6.4,
                        "swapEyes": False,
                        "stereoOffset": "none",
                        "parentId": "",
                        "location": {"x": 0, "y": 0, "z": 0},
                        "rotation": {"pitch": 0, "yaw": 0, "roll": 0},
                    }
                },
                "screens": {},
            },
            "cluster": {
                "primaryNode": {
                    "id": "node_0",
                    "ports": {
                        "ClusterSync": 41001,
                        "ClusterEventsJson": 41003,
                        "ClusterEventsBinary": 41004,
                    },
                },
                "sync": {
                    "renderSyncPolicy": {"type": "none", "parameters": {}},
                    "inputSyncPolicy": {
                        "type": "ReplicatePrimary",
                        "parameters": {},
                    },
                },
                "network": {
                    "ConnectRetriesAmount": "10",
                    "ConnectRetryDelay": "100",
                    "GameStartBarrierTimeout": "30000",
                    "FrameStartBarrierTimeout": "30000",
                    "FrameEndBarrierTimeout": "30000",
                    "RenderSyncBarrierTimeout": "30000",
                },
                "nodes": {
                    "node_0": {
                        "host": "127.0.0.1",
                        "sound": False,
                        "fullScreen": False,
                        "window": {"x": 0, "y": 0, "w": width, "h": height},
                        "postprocess": {},
                        "viewports": viewports,
                        "outputRemap": {
                            "bEnable": False,
                            "dataSource": "mesh",
                            "staticMeshAsset": "",
                            "externalFile": "",
                        },
                    }
                },
            },
            "customParameters": {},
            "diagnostics": {
                "simulateLag": False,
                "minLagTime": 0.01,
                "maxLagTime": 0.3,
            },
        }
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    return width, height
