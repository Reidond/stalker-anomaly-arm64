# xrGame: the game (AI, objects, UI, scripts). The project also compiles 58 files of src/xrServerEntities (server
# entities, object factory, script engine): xrServerEntities has no project of its own. Source list:
# src/xrGame/xrGame.vcxproj (the vs2022 copy lists nearly the same files). Owners: xrgame-triage session, then the
# xrgame-* shard sessions. Opt-in: -DANOMALY_BUILD_XRGAME=ON (or ANOMALY_BUILD_ALL_MODULES).
#
# No precompiled header yet: every xrGame file starts with #include "stdafx.h" (or pch_script.h), so the
# project's /Yu semantics are preserved by the includes themselves. The triage session adds target_precompile_headers.
anomaly_module_begin(xrGame "game logic")

anomaly_vcxproj_sources(xrgame_sources "${ANOMALY_SRC_DIR}/xrGame/xrGame.vcxproj")

anomaly_add_module(xrGame
    SOURCES ${xrgame_sources}
    INCLUDE_DIRS
        "${ANOMALY_SRC_DIR}/xrGame"
        "${ANOMALY_SRC_DIR}/xrServerEntities"
        "${ANOMALY_SRC_DIR}/3rd party/imgui"
        "${ANOMALY_SRC_DIR}/3rd party/cximage"
    DEFINES XRGAME_EXPORTS
    LINK luabind LuaJIT::luajit ode xrAPI xrCDB xrNetServer xrPhysics xrXMLParser
)
