"""Generate the local single-node nDisplay atlas used by URSoccerLab."""

from __future__ import annotations

import json
import math
from pathlib import Path


def write_ndisplay_config(view_count: int, output: Path) -> tuple[int, int]:
    """Write a tightly packed 640x480 RGB atlas and return its dimensions."""
    columns = math.ceil(math.sqrt(view_count * 4 / 3))
    rows = math.ceil(view_count / columns)
    viewports = {}
    for index in range(view_count):
        viewports[f"camera_{index:02d}"] = {
            "camera": "DefaultViewPoint",
            "bufferRatio": 1,
            "gPUIndex": -1,
            "allowCrossGPUTransfer": False,
            "isShared": False,
            "region": {
                "x": (index % columns) * 640,
                "y": (index // columns) * 480,
                "w": 640,
                "h": 480,
            },
            "projectionPolicy": {"type": "camera", "parameters": {}},
        }

    width = columns * 640
    height = rows * 480
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
