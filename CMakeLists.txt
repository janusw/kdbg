cmake_minimum_required(VERSION 2.8.12)

set(KDBG_VERSION 2.5.6)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/kdbg/version.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/kdbg/version.h)

find_package(ECM 1.7.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${ECM_KDE_MODULE_DIR})

include(KDEInstallDirs)
include(KDECMakeSettings)
# include(KDECompilerSettings)
# include(FeatureSummary)

find_package(Qt5 REQUIRED COMPONENTS Core)

find_package(KF5 REQUIRED
	     COMPONENTS Completion Config ConfigWidgets CoreAddons I18n
			IconThemes WidgetsAddons XmlGui KDELibs4Support)

#include(ManualStuff.cmake)
#include(ConfigureChecks.cmake)

add_subdirectory(kdbg)
add_subdirectory(po)
