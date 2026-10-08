# Third-party notices

Original URSoccerLab code is Apache-2.0 unless a component states otherwise.
The native client uses separately licensed dependencies:

| Component | License | Source |
| --- | --- | --- |
| Dear ImGui 1.91.9b | MIT | https://github.com/ocornut/imgui |
| cJSON 1.7.19 | MIT | https://github.com/DaveGamble/cJSON |
| SDL2 | zlib | https://github.com/libsdl-org/SDL |
| FFmpeg | LGPL/GPL depending on build options | https://ffmpeg.org/legal.html |

CMake fetches pinned ImGui and cJSON archives retaining upstream license files.
SDL2 and FFmpeg come from the build environment. Review the actual FFmpeg build
configuration before redistributing it; include applicable licenses/notices and
fulfill the corresponding source and relinking requirements. This source preview
has not produced a portable dependency bundle.
