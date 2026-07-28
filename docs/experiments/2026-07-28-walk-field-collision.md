# Walk policy rerun with field collision

## Scope

Rerun the existing external mos-brain Pi Plus walking-policy client after
adding the field-level MuJoCo ground plane in commit `24694b0`.

This is a protocol and contact integration check, not a gait validation: the
checkpoint was trained against the older mos-brain Pi model rather than the
current project MJCF.

## Revision and inputs

- Project revision: `24694b0` (`feat: add MuJoCo field collision`)
- Map: `/Game/Levels/URS_SoccerField`
- Scene: `Config/URS_scene.json`
- Robot: `robot_rp0` on TCP port `10000`
- Policy: `refs/mos-brain/simulation/mujoco/assets/policies/pi_plus_model_40000.pt`
- Command: `vx=0.35 m/s`, wall-clock duration `8 s`

## Procedure

1. Started Unreal offscreen with `-URSSceneConfig=Config/URS_scene.json`.
2. Waited for `[URS TCP] Robot 'robot_rp0' listening on port 10000`.
3. Ran `py_example/demos/walk_policy.py` in `py_example/.venv-walk`.
4. Recorded the left-eye stream to `py_example/out/walk_field_collision_20260728/walk_rp0.mp4`.

## Result

- The client resolved all 20 position actuators and completed the command.
- Reported forward displacement: `+0.432 m`.
- Video artifact: H.264, `640x480`, 81 frames at 15 FPS, encoded duration `5.4 s`.
- The simulator advanced slower than real time, so the 8-second wall-clock
  policy interval produced 5.4 seconds of camera simulation time.
- All inspected frames were black. Therefore the video does **not** validate
  the UE render path. It reproduces the existing camera startup/capture issue
  seen by the vision smoke test.

## Interpretation

The run exercises TCP motor commands and the new MuJoCo ground contact; it
does not establish that the old policy is a valid gait or that camera capture
is working. The next focused task is to make camera publication wait for a
nonblank rendered frame, then rerun this exact command.
