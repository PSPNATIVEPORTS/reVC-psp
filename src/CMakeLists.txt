find_package(Threads REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG ON)

file(GLOB_RECURSE ${PROJECT}_SOURCES "*.cpp" "*.h" "*.rc")
if(PSP OR N3DS)
    # only the PSP platform layer (the 3DS port shares it)
    list(FILTER ${PROJECT}_SOURCES EXCLUDE REGEX "/skel/(win|glfw|sdl2|android)/")
    list(FILTER ${PROJECT}_SOURCES EXCLUDE REGEX "/extras/custompipes")
    list(FILTER ${PROJECT}_SOURCES EXCLUDE REGEX "/audio/(oal|eax)/")
    list(FILTER ${PROJECT}_SOURCES EXCLUDE REGEX "/audio/sampman_(oal|miles)\\.cpp")
else()
    list(FILTER ${PROJECT}_SOURCES EXCLUDE REGEX "/skel/psp/")
endif()
if(NOT N3DS)
    list(FILTER ${PROJECT}_SOURCES EXCLUDE REGEX "/skel/3ds/")
endif()
list(REMOVE_ITEM ${PROJECT}_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/extras/GitSHA1.cpp")

function(header_directories RETURN_LIST)
    file(GLOB_RECURSE ALL_SRCS *.h *.cpp *.c)
    set(RELDIRS)
    foreach(SRC ${ALL_SRCS})
        file(RELATIVE_PATH RELSRC "${CMAKE_CURRENT_SOURCE_DIR}" "${SRC}")
        get_filename_component(RELDIR "${RELSRC}" DIRECTORY)
        list(APPEND RELDIRS ${RELDIR})
    endforeach()
    list(REMOVE_DUPLICATES RELDIRS)
    # the PSP SDK stand-ins of the 3DS port must not hide the real one
    list(FILTER RELDIRS EXCLUDE REGEX "skel/3ds")
    set(${RETURN_LIST} ${RELDIRS} PARENT_SCOPE)
endfunction()

header_directories(${PROJECT}_INCLUDES)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/extras/GitSHA1.cpp.in" "${CMAKE_CURRENT_BINARY_DIR}/extras/GitSHA1.cpp" @ONLY)
list(APPEND ${PROJECT}_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/extras/GitSHA1.cpp")

if(NOT ANDROID)
    add_executable(${EXECUTABLE} WIN32 ${${PROJECT}_SOURCES})
elseif(ANDROID)
    add_library(${EXECUTABLE} SHARED ${${PROJECT}_SOURCES})
endif()

target_link_libraries(${EXECUTABLE} PRIVATE
    librw::librw
    Threads::Threads
)

if(ANDROID)
    target_link_libraries(${EXECUTABLE} PRIVATE
            log
            GLESv3
            OpenSLES
    )
endif()

target_include_directories(${EXECUTABLE}
    PRIVATE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${${PROJECT}_INCLUDES}>
)

target_compile_definitions(${EXECUTABLE}
    PRIVATE
        $<IF:$<CONFIG:DEBUG>,DEBUG,NDEBUG>
        LIBRW
        CMAKE_NO_AUTOLINK
)

if(LIBRW_PLATFORM_D3D9)
    target_compile_definitions(${EXECUTABLE}
        PUBLIC
            USE_D3D9
    )
endif()

if(ANDROID)
    target_compile_definitions(${EXECUTABLE} PRIVATE
            ANDROID
            _ANDROID
            LINUX
            _LINUX
            POSIX
            _POSIX
            NO_HOOK_MALLOC
            AL_LIBTYPE_STATIC
            GNUC
            RW_GL3
            LIBRW_SDL2
            LIBRW
            AUDIO_OAL
            USE_OUR_VERSIONING
            HAVE_EFFICIENT_UNALIGNED_ACCESS
            VECTORIZE_SINCOS
        )
else()
    target_compile_definitions(${EXECUTABLE} PRIVATE CMAKE_BUILD)
    target_compile_definitions(${EXECUTABLE} PRIVATE USE_OUR_VERSIONING)
endif()

if(${PROJECT}_AUDIO STREQUAL "OAL")
    if(NOT TARGET OpenAL::OpenAL)
        if(APPLE)
            find_package(OpenAL CONFIG QUIET
                PATHS
                    "/opt/homebrew/opt/openal-soft"
                    "/usr/local/opt/openal-soft"
            )
        endif()
        if(NOT TARGET OpenAL::OpenAL)
            find_package(OpenAL REQUIRED)
        endif()
    endif()
    if(TARGET OpenAL::OpenAL)
        target_link_libraries(${EXECUTABLE} PRIVATE OpenAL::OpenAL)
    else()
        target_include_directories(${EXECUTABLE} PRIVATE ${OPENAL_INCLUDE_DIR})
        target_link_libraries(${EXECUTABLE} PRIVATE ${OPENAL_LIBRARY})
        target_compile_definitions(${EXECUTABLE} PRIVATE ${OPENAL_DEFINITIONS})
    endif()
    target_compile_definitions(${EXECUTABLE} PRIVATE AUDIO_OAL)
    # Nintendo Switch: devkitPro's OpenAL is built with an SDL2 audio backend,
    # so libopenal.a references SDL_* symbols that must be linked in after it.
    if(NINTENDO_SWITCH)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
        target_link_libraries(${EXECUTABLE} PRIVATE PkgConfig::SDL2)
    endif()
elseif(${PROJECT}_AUDIO STREQUAL "MSS")
    find_package(MilesSDK REQUIRED)
    target_compile_definitions(${EXECUTABLE} PRIVATE AUDIO_MSS)
    target_link_libraries(${EXECUTABLE} PRIVATE MilesSDK::MilesSDK)
endif()

if(${PROJECT}_AUDIO STREQUAL "OAL")
    if(NOT TARGET MPG123::libmpg123)
        find_package(mpg123 REQUIRED)
    endif()
    target_link_libraries(${EXECUTABLE} PRIVATE
        MPG123::libmpg123
    )
endif()

if(PSP)
    set_target_properties(${EXECUTABLE} PROPERTIES C_EXTENSIONS ON CXX_EXTENSIONS ON)
    target_compile_definitions(${EXECUTABLE} PRIVATE PSP_PORT _GNU_SOURCE)
    target_compile_options(${EXECUTABLE} PRIVATE
        -G0 -ffast-math -fsingle-precision-constant -fno-exceptions -fno-rtti
        -fno-strict-aliasing -Wno-narrowing)
    target_link_libraries(${EXECUTABLE} PRIVATE
        pspgum_vfpu pspvfpu pspgu pspge pspdisplay pspctrl psppower psprtc
        pspvram pspfpu psputility pspaudio pspaudiocodec)
    # memory guard in skel/psp/psp.cpp
    target_link_options(${EXECUTABLE} PRIVATE
        -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=memalign
        -Wl,--wrap=sinf -Wl,--wrap=cosf -Wl,--wrap=sceKernelCreateThread -Wl,--wrap=sceGeListUpdateStallAddr)
    if(REVC_PSP_LITE)
        set(PSP_TITLE "GTA Vice City - reVC PSP-1000")
        set(PSP_MEMSIZE 0)
    else()
        set(PSP_TITLE "GTA: Vice City")
        set(PSP_MEMSIZE 1)
    endif()
    # the XMB pictures: res/psp-art (not part of the source: game artwork,
    # ICON1.PMF is the animated icon) when it's there, else res/psp
    set(PSP_ART "${PROJECT_SOURCE_DIR}/res/psp")
    set(PSP_ANIM NULL)
    if(EXISTS "${PROJECT_SOURCE_DIR}/res/psp-art/PIC1.PNG")
        set(PSP_ART "${PROJECT_SOURCE_DIR}/res/psp-art")
    endif()
    if(EXISTS "${PROJECT_SOURCE_DIR}/res/psp-art/ICON1.PMF")
        set(PSP_ANIM "${PROJECT_SOURCE_DIR}/res/psp-art/ICON1.PMF")
    endif()
    set(PSP_MUSIC NULL)
    if(EXISTS "${PROJECT_SOURCE_DIR}/res/psp-art/SND0.AT3")
        set(PSP_MUSIC "${PROJECT_SOURCE_DIR}/res/psp-art/SND0.AT3")
    endif()
    create_pbp_file(
        TARGET ${EXECUTABLE}
        TITLE "${PSP_TITLE}"
        MEMSIZE ${PSP_MEMSIZE}
        ICON_PATH "${PSP_ART}/ICON0.PNG"
        BACKGROUND_PATH "${PSP_ART}/PIC1.PNG"
        ANIM_PATH "${PSP_ANIM}"
        MUSIC_PATH "${PSP_MUSIC}"
        PREVIEW_PATH NULL
        BUILD_PRX
    )
endif()
if(N3DS)
    set_target_properties(${EXECUTABLE} PROPERTIES C_EXTENSIONS ON CXX_EXTENSIONS ON SUFFIX ".elf")
    target_compile_definitions(${EXECUTABLE} PRIVATE PSP_PORT _GNU_SOURCE)
    target_include_directories(${EXECUTABLE} BEFORE PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/skel/3ds/compat" "${CMAKE_CURRENT_SOURCE_DIR}/skel/3ds")
    target_compile_options(${EXECUTABLE} PRIVATE
        -O2 -ffast-math -fsingle-precision-constant -fno-exceptions -fno-rtti
        -fno-strict-aliasing -Wno-narrowing
        # 4 byte enums like every other platform (saves, data structures);
        # arm-none-eabi would make them as small as possible
        -fno-short-enums)
    # the files that use libctru structures keep its enum size
    # (not n3ds.cpp / n3dsbuttons.cpp: they use game structures)
    set(N3DS_CTRU_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/skel/3ds/pspcompat.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/skel/3ds/mp3.cpp")
    set_source_files_properties(${N3DS_CTRU_SOURCES} PROPERTIES COMPILE_OPTIONS "-fshort-enums")
    target_link_libraries(${EXECUTABLE} PRIVATE citro3d ctru m)
    # memory guard in skel/psp/psp.cpp
    target_link_options(${EXECUTABLE} PRIVATE
        -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=memalign
        -Wl,--wrap=sinf -Wl,--wrap=cosf -Wl,--wrap=open -Wl,--wrap=fopen -Wl,--wrap=stat -Wl,--wrap=chdir -Wl,--wrap=exit -Wl,--wrap=abort -Wl,--wrap=__cxa_throw -Wl,--no-enum-size-warning -Wl,-Map=reVC.map)
    set(N3DS_RES "${PROJECT_SOURCE_DIR}/res/3ds")
    add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
        COMMAND "${DEVKITPRO}/tools/bin/smdhtool" --create "GTA: Vice City" "GTA Vice City" "GTA Vice City"
                "${N3DS_RES}/icon.png" "${CMAKE_CURRENT_BINARY_DIR}/reVC.smdh"
        COMMAND "${DEVKITPRO}/tools/bin/3dsxtool" "$<TARGET_FILE:${EXECUTABLE}>" "${CMAKE_CURRENT_BINARY_DIR}/reVC.3dsx"
                --smdh=${CMAKE_CURRENT_BINARY_DIR}/reVC.smdh
        # installable .cia (bannertool + makerom); the banner is FBI's
        # floating box with our logo on it (res/3ds/banner.cgfx, made with
        # utils/3dsbanner/fbi_box.py; FBI's MIT licence: banner_LICENSE.txt)
        COMMAND "${DEVKITPRO}/tools/bin/bannertool" makebanner -ci "${N3DS_RES}/banner.cgfx" -a "${N3DS_RES}/banner.wav"
                -o "${CMAKE_CURRENT_BINARY_DIR}/banner.bnr"
        COMMAND "${DEVKITPRO}/tools/bin/bannertool" makesmdh -s "GTA: Vice City" -l "GTA Vice City" -p "GTA Vice City"
                -i "${N3DS_RES}/icon.png" -o "${CMAKE_CURRENT_BINARY_DIR}/icon.icn"
        COMMAND "${DEVKITPRO}/tools/bin/makerom" -f cia -o "${CMAKE_CURRENT_BINARY_DIR}/reVC.cia" -target t -exefslogo
                -elf "$<TARGET_FILE:${EXECUTABLE}>" -rsf "${N3DS_RES}/reVC.rsf"
                -banner "${CMAKE_CURRENT_BINARY_DIR}/banner.bnr" -icon "${CMAKE_CURRENT_BINARY_DIR}/icon.icn"
        VERBATIM)
endif()
if(${PROJECT}_WITH_OPUS)
    find_package(opusfile REQUIRED)
    target_link_libraries(${EXECUTABLE} PRIVATE
        opusfile::opusfile
    )
    target_compile_definitions(${EXECUTABLE} PRIVATE AUDIO_OPUS)
endif()
if(${PROJECT}_WITH_LIBSNDFILE)
    find_package(SndFile REQUIRED)
    target_link_libraries(${EXECUTABLE} PRIVATE
        SndFile::SndFile
    )
    target_compile_definitions(${EXECUTABLE} PRIVATE AUDIO_OAL_USE_SNDFILE)
endif()

target_compile_definitions(${EXECUTABLE} PRIVATE )

option(${PROJECT}_WITH_SANITIZERS "Use UB sanitizers (better crash log)" OFF)
option(${PROJECT}_WITH_ASAN "Use Address sanitizer (better crash log)" OFF)
option(${PROJECT}_DISABLE_WARNINGS "Disable build warnings" ON)

if(${PROJECT}_DISABLE_WARNINGS)
    target_compile_options(${EXECUTABLE} PRIVATE
            -w)
endif()

if(${PROJECT}_WITH_SANITIZERS)
    target_compile_options(${EXECUTABLE} PUBLIC
            -fsanitize=undefined,float-divide-by-zero,integer,implicit-conversion,implicit-integer-truncation,implicit-integer-arithmetic-value-change,local-bounds,nullability
            -g3 -fno-omit-frame-pointer)
    target_link_options(${EXECUTABLE} PUBLIC -fsanitize=undefined,float-divide-by-zero,integer,implicit-conversion,implicit-integer-truncation,implicit-integer-arithmetic-value-change,local-bounds,nullability)
endif()

if(${PROJECT}_WITH_ASAN)
    target_compile_options(${EXECUTABLE} PUBLIC -fsanitize=address -g3 -fno-omit-frame-pointer)
    target_link_options(${EXECUTABLE} PUBLIC -fsanitize=address)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    if(ANDROID)
        target_compile_options(${EXECUTABLE}
            PRIVATE
                -pthread
                -std=c++11
                -fno-strict-aliasing
                -fexceptions
                -Wcast-align
                -Wuninitialized
                -Winit-self
                -Wstrict-aliasing
                -Wno-reorder
                -Wno-unknown-pragmas
                -Wno-unused-function
                -Wno-unused-but-set-variable
                -Wno-unused-value
                -Wno-unused-command-line-argument
                -faligned-new
        )
    endif()
    target_compile_options(${EXECUTABLE}
        PRIVATE
            "-Wall"
    )
    if (NOT LIBRW_PLATFORM_PS2)
        target_compile_options(${EXECUTABLE}
            PRIVATE
                -Wextra
                -Wdouble-promotion
                -Wpedantic
        )
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(${EXECUTABLE}
        PUBLIC
            /Zc:sizedDealloc-
    )
endif()

if(NINTENDO_SWITCH)
    set(${PROJECT}_C_CXX_EXTENSIONS ON)
else()
    set(${PROJECT}_C_CXX_EXTENSIONS OFF)
endif()

if(LIBRW_PLATFORM_GL3 AND LIBRW_GL3_GFXLIB STREQUAL "GLFW")
	include(CheckSymbolExists)

	set(CMAKE_REQUIRED_LIBRARIES glfw)
	set(CMAKE_REQUIRED_DEFINITIONS -DGLFW_EXPOSE_NATIVE_X11)
	check_symbol_exists(glfwGetX11Display "GLFW/glfw3.h;GLFW/glfw3native.h" GLFW_HAS_X11)
	unset(CMAKE_REQUIRED_DEFINITIONS)
	unset(CMAKE_REQUIRED_LIBRARIES)

	if (GLFW_HAS_X11)
		find_package(X11 REQUIRED)
		target_link_libraries(${EXECUTABLE} PRIVATE X11::X11)
		target_compile_definitions(${EXECUTABLE} PRIVATE GET_KEYBOARD_INPUT_FROM_X11)
	endif (GLFW_HAS_X11)
endif()

set_target_properties(${EXECUTABLE}
    PROPERTIES
        C_STANDARD 11
        C_EXTENSIONS ${${PROJECT}_C_CXX_EXTENSIONS}
        C_STANDARD_REQUIRED ON
        CXX_STANDARD 11
        CXX_EXTENSIONS ${${PROJECT}_C_CXX_EXTENSIONS}
        CXX_STANDARD_REQUIRED ON
)

if(APPLE AND NOT ANDROID)
    set(${PROJECT}_MACOS_ICON "${PROJECT_SOURCE_DIR}/res/images/reVC.icns")
    set(${PROJECT}_MACOS_NOTICES "${PROJECT_SOURCE_DIR}/res/macos/ThirdPartyNotices.txt")
    target_sources(${EXECUTABLE} PRIVATE
        "${${PROJECT}_MACOS_ICON}"
        "${${PROJECT}_MACOS_NOTICES}"
    )
    set_source_files_properties(
        "${${PROJECT}_MACOS_ICON}"
        "${${PROJECT}_MACOS_NOTICES}"
        PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    set_target_properties(${EXECUTABLE} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/res/macos/Info.plist"
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/../Frameworks"
        INSTALL_RPATH_USE_LINK_PATH FALSE
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "io.github.mrxenginner.reVC"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "$<$<CONFIG:Release>:Automatic>"
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "$<$<CONFIG:Release>:Apple Development>"
        "XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME[sdk=macosx*]" "$<IF:$<CONFIG:Debug>,NO,YES>"
        "XCODE_ATTRIBUTE_INSTALL_PATH[sdk=macosx*]" "$(LOCAL_APPS_DIR)"
        XCODE_ATTRIBUTE_SKIP_INSTALL "NO"
    )
    add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/LICENSE.md"
            "$<TARGET_BUNDLE_CONTENT_DIR:${EXECUTABLE}>/Resources/LICENSE"
        COMMAND ${CMAKE_COMMAND} -E remove_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:${EXECUTABLE}>/Resources/gamefiles"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/gamefiles"
            "$<TARGET_BUNDLE_CONTENT_DIR:${EXECUTABLE}>/Resources/gamefiles"
        COMMENT "Copying gamefiles into the app bundle"
    )

    set(macos_dependency_dylibs_available TRUE)
    foreach(dependency IN LISTS ${PROJECT}_MACOS_DEPENDENCIES)
        if(NOT DEFINED ${PROJECT}_MACOS_${dependency}_DYLIBS)
            set(macos_dependency_dylibs_available FALSE)
            break()
        endif()
    endforeach()
    if(macos_dependency_dylibs_available)
        set(shell_dollar "$")
        set(codesign_command
            "identity=${shell_dollar}EXPANDED_CODE_SIGN_IDENTITY; test -n \"${shell_dollar}identity\" || identity=-; /usr/bin/codesign --force --sign \"${shell_dollar}identity\" \"${shell_dollar}1\""
        )
        set(app_codesign_command
            "identity=${shell_dollar}EXPANDED_CODE_SIGN_IDENTITY; test -n \"${shell_dollar}identity\" || identity=-; if test \"${shell_dollar}2\" = YES; then /usr/bin/codesign --force --sign \"${shell_dollar}identity\" --options runtime \"${shell_dollar}1\"; else /usr/bin/codesign --force --sign \"${shell_dollar}identity\" \"${shell_dollar}1\"; fi"
        )
        if(CMAKE_GENERATOR STREQUAL "Xcode")
            set(app_hardened "$<IF:$<CONFIG:Debug>,NO,YES>")
        else()
            set(app_hardened NO)
        endif()
        add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_CONTENT_DIR:${EXECUTABLE}>/Frameworks"
        )

        function(reVC_bundle_macos_dylib target filename)
            set(destination
                "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks/${filename}"
            )
            list(LENGTH ARGN source_count)
            if(source_count GREATER 1)
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND /usr/bin/lipo -create ${ARGN} -output "${destination}"
                )
            else()
                list(GET ARGN 0 source)
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${source}" "${destination}"
                )
            endif()
            foreach(source IN LISTS ARGN)
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND install_name_tool -change
                        "${source}" "@rpath/${filename}" "$<TARGET_FILE:${target}>"
                )
            endforeach()
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND /bin/sh -c
                    "${codesign_command}"
                    codesign "${destination}"
                VERBATIM
            )
        endfunction()

        foreach(dependency IN LISTS ${PROJECT}_MACOS_DEPENDENCIES)
            reVC_bundle_macos_dylib(${EXECUTABLE}
                ${${PROJECT}_MACOS_${dependency}_FILENAME}
                ${${PROJECT}_MACOS_${dependency}_DYLIBS})
        endforeach()
        add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
            COMMAND /bin/sh -c
                "${app_codesign_command}"
                codesign "$<TARGET_BUNDLE_DIR:${EXECUTABLE}>"
                "${app_hardened}"
            COMMENT "Bundling macOS dependencies"
            VERBATIM
        )
    endif()
endif()

if(${PROJECT}_INSTALL)
    install(
        TARGETS ${EXECUTABLE}
        EXPORT ${EXECUTABLE}-targets
        BUNDLE DESTINATION "."
        RUNTIME DESTINATION "."
    )
    if(MSVC)
        install(FILES $<TARGET_PDB_FILE:${EXECUTABLE}> DESTINATION "." OPTIONAL)
    endif()
endif()

reVC_platform_target(${EXECUTABLE} INSTALL)
