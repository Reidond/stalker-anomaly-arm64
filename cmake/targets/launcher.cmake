# The SwiftUI launcher (src/Platform/Launcher): the app's first screen, shown before the engine starts. Owner: mod-launcher session.
# Swift is enabled here, the target itself is defined in src/Platform/Launcher/CMakeLists.txt (after every module file, so that the
# ModManagerCore and ModDownloads targets are known by then). Swift is not part of the Windows build, which does not use CMake.
enable_language(Swift)
