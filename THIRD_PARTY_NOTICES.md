# Third-party notices

Original URSoccerLab code is Apache-2.0 unless a component states otherwise.
The native client uses separately licensed dependencies:

| Component | License | Source |
| --- | --- | --- |
| cJSON 1.7.19 | MIT | https://github.com/DaveGamble/cJSON |
| wxWidgets | wxWindows Library Licence | https://www.wxwidgets.org/about/licence/ |
| FFmpeg | LGPL/GPL depending on build options | https://ffmpeg.org/legal.html |

CMake fetches pinned cJSON sources retaining their upstream license.
wxWidgets and FFmpeg come from the build environment. Review the actual FFmpeg build
configuration before redistributing it; include applicable licenses/notices and
fulfill the corresponding source and relinking requirements. This source preview
has not produced a portable dependency bundle.
