# K1 walking

The checkpoint requires the K1 joint armatures used by the MOS-Brain reference. Create a separate robot package from the distributed external resources:

```bash
python walk/k1_walk/prepare_robot.py
./dist/URSoccerLab.AppImage walk/k1_walk/scene.json
```

In a second terminal, with this example's dependencies and the distributed client wheel installed:

```bash
python walk/k1_walk/k1_walk.py --vx 0.3 --duration 15
```

The default checkpoint is `models/policies/k1_model_46000.pt`. The preparation script leaves the original robot package intact; if the generated package already exists, remove or rename it yourself before regenerating it. The scene references `external/robots/booster_k1_armature`.

The controller adapts the MOS-Brain MuJoCo reference policy (22 joints, 78 observations, 50 Hz). On the URSoccerLab demo server, an eight-second 0.3 m/s trial with these armatures remained upright and moved approximately 1.9 m. This is a single trial, not a general robustness evaluation.

The controller resets `robot_rp0` to its scene pose and holds it upright until the controller is ready. The simulator admin port must be available (default 11000); use `--admin-port` and `--actor-id` if configured differently.
