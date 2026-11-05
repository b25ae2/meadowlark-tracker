# Install rules and packages for the meadowlark app (included from CMakeLists.txt when the
# SDL front end is built).
#
#   cmake --build platforms/desktop/build --target package
#
# Linux: a .deb (bin/meadowlark, a desktop entry and icons; its dependencies from the
# libraries it links, via dpkg-shlibdeps). macOS: Meadowlark.app in a .dmg, with the
# libraries it links from outside the system copied into the bundle and the bundle signed
# ad hoc. The development binary in the build directory stays as it is either way.

set(MEADOWLARK_VERSION "${VER_MINI_VER_MAJOR}.${VER_MINI_VER_MINOR}.${VER_MINI_VER_PATCH}")
if(NOT VER_MINI_VER_BETA STREQUAL "255")
  set(MEADOWLARK_VERSION "${MEADOWLARK_VERSION}~beta${VER_MINI_VER_BETA}")
endif()
set(PACKAGING_DIR ${CMAKE_CURRENT_SOURCE_DIR}/packaging)

if(APPLE)
  set(APP_DIR Meadowlark.app)

  # The bundle runs where its oldest bundled library does: Homebrew builds them for the
  # macOS it runs on.
  set(SDL3_LOCATION)
  foreach(target SDL3::SDL3-shared SDL3::SDL3)
    if(TARGET ${target} AND NOT SDL3_LOCATION)
      foreach(property IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION_RELWITHDEBINFO IMPORTED_LOCATION_NOCONFIG IMPORTED_LOCATION)
        get_target_property(location ${target} ${property})
        if(location AND NOT SDL3_LOCATION)
          set(SDL3_LOCATION ${location})
        endif()
      endforeach()
    endif()
  endforeach()
  set(MEADOWLARK_MIN_MACOS)
  if(SDL3_LOCATION)
    execute_process(COMMAND otool -l ${SDL3_LOCATION} OUTPUT_VARIABLE SDL3_LOAD_COMMANDS ERROR_QUIET)
    if(SDL3_LOAD_COMMANDS MATCHES "minos ([0-9.]+)")
      set(MEADOWLARK_MIN_MACOS ${CMAKE_MATCH_1})
    endif()
  endif()
  if(NOT MEADOWLARK_MIN_MACOS)
    execute_process(COMMAND sw_vers -productVersion OUTPUT_VARIABLE MEADOWLARK_MIN_MACOS OUTPUT_STRIP_TRAILING_WHITESPACE)
  endif()
  message(STATUS "Meadowlark.app needs macOS ${MEADOWLARK_MIN_MACOS} (SDL3: ${SDL3_LOCATION})")
  configure_file(${PACKAGING_DIR}/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)

  # meadowlark.icns from the 1024 px icon.
  set(ICONSET ${CMAKE_CURRENT_BINARY_DIR}/meadowlark.iconset)
  set(ICNS ${CMAKE_CURRENT_BINARY_DIR}/meadowlark.icns)
  set(ICON_COMMANDS)
  foreach(size 16 32 128 256 512)
    math(EXPR double "${size} * 2")
    list(APPEND ICON_COMMANDS
      COMMAND sips -z ${size} ${size} ${PACKAGING_DIR}/meadowlark.png --out ${ICONSET}/icon_${size}x${size}.png >/dev/null
      COMMAND sips -z ${double} ${double} ${PACKAGING_DIR}/meadowlark.png --out ${ICONSET}/icon_${size}x${size}@2x.png >/dev/null)
  endforeach()
  add_custom_command(OUTPUT ${ICNS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ICONSET}
    ${ICON_COMMANDS}
    COMMAND iconutil -c icns ${ICONSET} -o ${ICNS}
    DEPENDS ${PACKAGING_DIR}/meadowlark.png
    COMMENT "Making meadowlark.icns")
  add_custom_target(meadowlark_icns ALL DEPENDS ${ICNS})

  install(TARGETS meadowlark RUNTIME DESTINATION ${APP_DIR}/Contents/MacOS)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/Info.plist DESTINATION ${APP_DIR}/Contents)
  install(FILES ${ICNS} DESTINATION ${APP_DIR}/Contents/Resources)
  install(FILES ${ROOT_DIR}/LICENSE ${ROOT_DIR}/NOTICE.md DESTINATION ${APP_DIR}/Contents/Resources)
  # Homebrew's libraries into Contents/Frameworks, then one signature over the result (the
  # copies' own signatures break when their load paths are rewritten).
  install(CODE "
    set(app \"\${CMAKE_INSTALL_PREFIX}/${APP_DIR}\")
    include(BundleUtilities)
    set(BU_CHMOD_BUNDLE_ITEMS ON)
    fixup_bundle(\"\${app}\" \"\" \"/opt/homebrew/lib;/usr/local/lib\")
    execute_process(COMMAND codesign --force --deep --sign - \"\${app}\" RESULT_VARIABLE signed)
    if(NOT signed EQUAL 0)
      message(FATAL_ERROR \"codesign failed for \${app}\")
    endif()
  ")

  set(CPACK_GENERATOR DragNDrop)
  set(CPACK_DMG_VOLUME_NAME Meadowlark)
  set(CPACK_SYSTEM_NAME macos-${CMAKE_SYSTEM_PROCESSOR})
else()
  include(GNUInstallDirs)
  install(TARGETS meadowlark RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES ${PACKAGING_DIR}/org.meadowlark.tracker.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  foreach(size 64 128 256 512)
    install(FILES ${PACKAGING_DIR}/meadowlark-${size}.png
      DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
      RENAME org.meadowlark.tracker.png)
  endforeach()
  install(FILES ${ROOT_DIR}/LICENSE RENAME copyright DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/meadowlark)
  install(FILES ${ROOT_DIR}/NOTICE.md ${CMAKE_CURRENT_SOURCE_DIR}/README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/meadowlark)

  set(CPACK_GENERATOR DEB)
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_SECTION sound)
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Meadowlark")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
endif()

set(CPACK_PACKAGE_NAME meadowlark)
set(CPACK_PACKAGE_VENDOR Meadowlark)
set(CPACK_PACKAGE_VERSION ${MEADOWLARK_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Polyend Tracker Mini firmware (Community Edition) as a desktop app")
set(CPACK_PACKAGE_DESCRIPTION "Meadowlark runs the Polyend Tracker Community Edition firmware, built as a
Tracker Mini, in a window: keyboard and gamepad input, PipeWire or Core Audio sound and
input, MIDI, and CLAP and LV2 instrument plugins.")
set(CPACK_STRIP_FILES OFF) # function names in crash backtraces
include(CPack)
