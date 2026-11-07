idf_component_register(SRCS "main.c"
                       REQUIRES reader lock store sync clock provision power door_fsm sleep
                       PRIV_REQUIRES esp_timer nvs_flash esp_hw_support esp_driver_gpio esp_pm)

# --- Dev master card (build-time only) ------------------------------------
# A file "master_card.local" in the project root injects a compile-time UID
# that bypasses the allow-list (DOOR_DENY_UNKNOWN -> DOOR_ALLOW).  The file
# is git-ignored; no file = no master card, zero runtime overhead.
set(_mc_file "${PROJECT_DIR}/master_card.local")
if(EXISTS "${_mc_file}")
    # Production safety gate: a master card must never ship.
    if(CONFIG_SECURE_BOOT OR CONFIG_SECURE_FLASH_ENC_ENABLED)
        message(FATAL_ERROR
            "master_card.local exists but Secure Boot / Flash Encryption is ON.\n"
            "A dev master card must never be present in a production image.\n"
            "Delete the file or switch to the dev profile.")
    endif()

    # Re-run cmake when the file changes.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_mc_file}")

    file(STRINGS "${_mc_file}" _mc_lines)
    set(_mc_uid "")
    foreach(_line ${_mc_lines})
        string(REGEX REPLACE "^[ \t]+" "" _line "${_line}")
        if(_line MATCHES "^#" OR _line STREQUAL "")
            continue()
        endif()
        string(REPLACE " " "" _line "${_line}")
        string(TOUPPER "${_line}" _mc_uid)
        break()
    endforeach()

    if(_mc_uid STREQUAL "")
        message(STATUS "master_card.local present but empty — no master card")
    elseif(NOT _mc_uid MATCHES "^[0-9A-F]{8}$")
        message(FATAL_ERROR
            "master_card.local: '${_mc_uid}' is not exactly 8 hex digits (a 4-byte UID)")
    else()
        message(STATUS "Dev master card UID: ${_mc_uid}")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE
            DOOR_MASTER_UID_HEX="${_mc_uid}")
    endif()
endif()
