// SPDX-License-Identifier: GPL-3.0-or-later
extern "C" __declspec(dllimport) int danzi_launcher_main(int argc, wchar_t** argv);

int wmain(int argc, wchar_t** argv) {
    return danzi_launcher_main(argc, argv);
}
