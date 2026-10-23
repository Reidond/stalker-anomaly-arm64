import Foundation

// ModManagerCore host test, run by ctest as `modmanager_core`. MODMANAGER_TEST_FILTER=<substring> runs matching tests only;
// MODMANAGER_TEST_ARTIFACTS=<dir> keeps the wizard renders (PNG).

registerCoreTests()
registerInstallTests()
registerFomodTests()
exit(await runAllTests())
