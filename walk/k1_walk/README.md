# K1 walking

From the repository root, start the simulator with `walk/k1_walk/scene.json`, then run `python walk/k1_walk/k1_walk.py --vx 0.3 --duration 15` in an environment with this example's dependencies and the distributed client wheel installed. The default checkpoint is `models/policies/k1_model_46000.pt`.

This adapts the MOS-Brain MuJoCo reference policy (22 joints, 78 observations, 50 Hz). The simulator robot model and state velocity conventions may differ from the reference; gait performance requires an actual simulator run.
