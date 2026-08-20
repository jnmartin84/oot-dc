#include "segment_symbols.h"
#include "console_logo_state.h"
#include "file_select_state.h"
#include "map_select_state.h"
#include "setup_state.h"
#include "title_setup_state.h"
#include "z_game_dlftbls.h"
#include "play_state.h"

// Linker symbol declarations (used in the table below) - NOT needed for PC
#ifndef LINUX
#define DEFINE_GAMESTATE(typeName, enumName, name) DECLARE_OVERLAY_SEGMENT(name)
#define DEFINE_GAMESTATE_INTERNAL(typeName, enumName)

#include "tables/gamestate_table.h"

#undef DEFINE_GAMESTATE
#undef DEFINE_GAMESTATE_INTERNAL
#endif

// Gamestate Overlay Table definition
#ifdef LINUX

// PC: All gamestates compiled in, no overlay loading needed
#define DEFINE_GAMESTATE_INTERNAL(typeName, enumName)                                                     \
    {                                                                                                     \
        NULL, ROM_FILE_UNSET,          NULL, NULL, NULL, typeName##_Init, typeName##_Destroy, NULL, NULL, \
        0,    sizeof(typeName##State),                                                                    \
    },

#define DEFINE_GAMESTATE(typeName, enumName, name)                                                        \
    {                                                                                                     \
        NULL, ROM_FILE_UNSET,          NULL, NULL, NULL, typeName##_Init, typeName##_Destroy, NULL, NULL, \
        0,    sizeof(typeName##State),                                                                    \
    },

#else

#define DEFINE_GAMESTATE_INTERNAL(typeName, enumName)                                                     \
    {                                                                                                     \
        NULL, ROM_FILE_UNSET,          NULL, NULL, NULL, typeName##_Init, typeName##_Destroy, NULL, NULL, \
        0,    sizeof(typeName##State),                                                                    \
    },

#define DEFINE_GAMESTATE(typeName, enumName, name) \
    {                                              \
        NULL,                                      \
        ROM_FILE(ovl_##name),                      \
        _ovl_##name##SegmentStart,                 \
        _ovl_##name##SegmentEnd,                   \
        NULL,                                      \
        typeName##_Init,                           \
        typeName##_Destroy,                        \
        NULL,                                      \
        NULL,                                      \
        0,                                         \
        sizeof(typeName##State),                   \
    },

#endif

GameStateOverlay gGameStateOverlayTable[] = {
#include "tables/gamestate_table.h"
};

#undef DEFINE_GAMESTATE
#undef DEFINE_GAMESTATE_INTERNAL