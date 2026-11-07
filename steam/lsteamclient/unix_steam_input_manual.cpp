#include "unix_private.h"

#if 0
#pragma makedep unix
#endif

#include <unordered_map>

#ifdef __linux__
#include <X11/keysym.h>
#endif

#ifdef __linux__
static const uint32_t vk_to_xkeysym[0xFF] =
{
    0,            /* 0x0  Undefined */
    0,            /* VK_LBUTTON */
    0,            /* VK_RBUTTON */
    XK_Cancel,    /* VK_CANCEL */
    0,            /* VK_MBUTTON */
    0,            /* VK_XBUTTON1 */
    0,            /* VK_XBUTTON2 */
    0,            /* 0x07 Undefined */
    XK_BackSpace, /* VK_BACK */
    XK_Tab,       /* VK_TAB */
    0,            /* 0x0A-0x0B  Undefined */
    0,
    XK_Clear,  /* VK_CLEAR */
    XK_Return, /* VK_RETURN */
    0,         /* 0x0E-0x0F  Undefined */
    0,
    XK_Shift_L,     /* VK_SHIFT */
    XK_Control_L,   /* VK_CONTROL */
    XK_Alt_L,       /* VK_MENU */
    XK_Pause,       /* VK_PAUSE */
    XK_Caps_Lock,   /* VK_CAPITAL */
    0,              /* VK_KANA */
    0,              /* 0x16  Undefined */
    0,              /* VK_JUNJA */
    0,              /* VK_FINAL */
    0,              /* VK_HANJA */
    0,              /* 0x1A  Undefined */
    XK_Escape,      /* VK_ESCAPE */
    XK_Henkan_Mode, /* VK_CONVERT */
    XK_Muhenkan,    /* VK_NONCONVERT */
    0,              /* VK_ACCEPT */
    0,              /* VK_MODECHANGE */
    ' ',            /* VK_SPACE */
    XK_Prior,       /* VK_PRIOR */
    XK_Next,        /* VK_NEXT */
    XK_End,         /* VK_END */
    XK_Home,        /* VK_HOME */
    XK_Left,        /* VK_LEFT */
    XK_Up,          /* VK_UP */
    XK_Right,       /* VK_RIGHT */
    XK_Down,        /* VK_DOWN */
    XK_Select,      /* VK_SELECT */
    XK_Print,       /* VK_PRINT */
    XK_Execute,     /* VK_EXECUTE */
    XK_Print,       /* VK_SNAPSHOT */
    XK_Insert,      /* VK_INSERT */
    XK_Delete,      /* VK_DELETE */
    XK_Help,        /* VK_HELP */

    '0',
    '1',
    '2',
    '3',
    '4',
    '5',
    '6',
    '7',
    '8',
    '9',
    0,
    0,
    0,
    0,
    0,
    0,
    0, /* 0x3A-0x40  Undefined */
    'A',
    'B',
    'C',
    'D',
    'E',
    'F',
    'G',
    'H',
    'I',
    'J',
    'K',
    'L',
    'M',
    'N',
    'O',
    'P',
    'Q',
    'R',
    'S',
    'T',
    'U',
    'V',
    'W',
    'X',
    'Y',
    'Z',

    XK_Super_L,      /* VK_LWIN */
    XK_Super_R,      /* VK_RWIN */
    XK_Menu,         /* VK_APPS */
    0,               /* 0x5E  Unassigned */
    0,               /* VK_SLEEP */
    XK_KP_0,         /* VK_NUMPAD0 */
    XK_KP_1,         /* VK_NUMPAD1 */
    XK_KP_2,         /* VK_NUMPAD2 */
    XK_KP_3,         /* VK_NUMPAD3 */
    XK_KP_4,         /* VK_NUMPAD4 */
    XK_KP_5,         /* VK_NUMPAD5 */
    XK_KP_6,         /* VK_NUMPAD6 */
    XK_KP_7,         /* VK_NUMPAD7 */
    XK_KP_8,         /* VK_NUMPAD8 */
    XK_KP_9,         /* VK_NUMPAD9 */
    XK_KP_Multiply,  /* VK_MULTIPLY */
    XK_KP_Add,       /* VK_ADD */
    XK_KP_Separator, /* VK_SEPARATOR */
    XK_KP_Subtract,  /* VK_SUBTRACT */
    XK_KP_Decimal,   /* VK_DECIMAL */
    XK_KP_Divide,    /* VK_DIVIDE */
    XK_F1,           /* VK_F1 */
    XK_F2,           /* VK_F2 */
    XK_F3,           /* VK_F3 */
    XK_F4,           /* VK_F4 */
    XK_F5,           /* VK_F5 */
    XK_F6,           /* VK_F6 */
    XK_F7,           /* VK_F7 */
    XK_F8,           /* VK_F8 */
    XK_F9,           /* VK_F9 */
    XK_F10,          /* VK_F10 */
    XK_F11,          /* VK_F11 */
    XK_F12,          /* VK_F12 */
    XK_F13,          /* VK_F13 */
    XK_F14,          /* VK_F14 */
    XK_F15,          /* VK_F15 */
    XK_F16,          /* VK_F16 */
    XK_F17,          /* VK_F17 */
    XK_F18,          /* VK_F18 */
    XK_F19,          /* VK_F19 */
    XK_F20,          /* VK_F20 */
    XK_F21,          /* VK_F21 */
    XK_F22,          /* VK_F22 */
    XK_F23,          /* VK_F23 */
    XK_F24,          /* VK_F24 */
    0,               /* VK_NAVIGATION_VIEW */
    0,               /* VK_NAVIGATION_MENU */
    0,               /* VK_NAVIGATION_UP */
    0,               /* VK_NAVIGATION_DOWN */
    0,               /* VK_NAVIGATION_LEFT */
    0,               /* VK_NAVIGATION_RIGHT */
    0,               /* VK_NAVIGATION_ACCEPT */
    0,               /* VK_NAVIGATION_CANCEL */
    XK_Num_Lock,     /* VK_NUMLOCK */
    XK_Scroll_Lock,  /* VK_SCROLL */
    XK_KP_Equal,     /* VK_OEM_NEC_EQUAL */
    0,               /* VK_OEM_FJ_JISHO */
    0,               /* VK_OEM_FJ_MASSHOU */
    0,               /* VK_OEM_FJ_TOUROKU */
    0,               /* VK_OEM_FJ_LOYA */
    0,               /* VK_OEM_FJ_ROYA */
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,            /* 0x97-0x9F  Unassigned */
    XK_Shift_L,   /* VK_LSHIFT */
    XK_Shift_R,   /* VK_RSHIFT */
    XK_Control_L, /* VK_LCONTROL */
    XK_Control_R, /* VK_RCONTROL */
    XK_Alt_L,     /* VK_LMENU */
    XK_Alt_R,     /* VK_RMENU */

    /* below are OEM, browser keys, etc */
};

uint32_t manual_convert_nNativeKeyCode( uint32_t win_vk )
{
    if(win_vk < sizeof(vk_to_xkeysym) / sizeof(*vk_to_xkeysym))
        return vk_to_xkeysym[win_vk];
    return 0;
}
#elif defined(__APPLE__)
/* mproton: ISteamHTMLSurface::KeyDown/KeyUp take "the virtual key code value
 * from your OS", which on macOS is a Carbon kVK_* code (HIToolbox/Events.h).
 * Values are spelled out so the unix side doesn't need Carbon headers.
 * kVK_ANSI_A is 0, so keys without a macOS equivalent map to MAC_VK_NONE. */
#define MAC_VK_NONE 0xffff

uint32_t manual_convert_nNativeKeyCode( uint32_t win_vk )
{
    /* VK_0..VK_9 and VK_A..VK_Z (ANSI layout positions) */
    static const uint16_t digits[10] = { 0x1d, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1a, 0x1c, 0x19 };
    static const uint16_t letters[26] =
    {
        0x00, 0x0b, 0x08, 0x02, 0x0e, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25, 0x2e, /* A-M */
        0x2d, 0x1f, 0x23, 0x0c, 0x0f, 0x01, 0x11, 0x20, 0x09, 0x0d, 0x07, 0x10, 0x06, /* N-Z */
    };
    static const uint16_t numpad[10] = { 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5b, 0x5c };
    /* VK_F1..VK_F20 */
    static const uint16_t fkeys[20] =
    {
        0x7a, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6d,
        0x67, 0x6f, 0x69, 0x6b, 0x71, 0x6a, 0x40, 0x4f, 0x50, 0x5a,
    };

    if (win_vk >= 0x30 && win_vk <= 0x39) return digits[win_vk - 0x30];
    if (win_vk >= 0x41 && win_vk <= 0x5a) return letters[win_vk - 0x41];
    if (win_vk >= 0x60 && win_vk <= 0x69) return numpad[win_vk - 0x60];
    if (win_vk >= 0x70 && win_vk <= 0x83) return fkeys[win_vk - 0x70];

    switch (win_vk)
    {
    case 0x08: return 0x33; /* VK_BACK -> kVK_Delete */
    case 0x09: return 0x30; /* VK_TAB -> kVK_Tab */
    case 0x0c: return 0x47; /* VK_CLEAR -> kVK_ANSI_KeypadClear */
    case 0x0d: return 0x24; /* VK_RETURN -> kVK_Return */
    case 0x10: return 0x38; /* VK_SHIFT -> kVK_Shift */
    case 0x11: return 0x3b; /* VK_CONTROL -> kVK_Control */
    case 0x12: return 0x3a; /* VK_MENU -> kVK_Option */
    case 0x14: return 0x39; /* VK_CAPITAL -> kVK_CapsLock */
    case 0x15: return 0x68; /* VK_KANA -> kVK_JIS_Kana */
    case 0x1b: return 0x35; /* VK_ESCAPE -> kVK_Escape */
    case 0x1d: return 0x66; /* VK_NONCONVERT -> kVK_JIS_Eisu */
    case 0x20: return 0x31; /* VK_SPACE -> kVK_Space */
    case 0x21: return 0x74; /* VK_PRIOR -> kVK_PageUp */
    case 0x22: return 0x79; /* VK_NEXT -> kVK_PageDown */
    case 0x23: return 0x77; /* VK_END -> kVK_End */
    case 0x24: return 0x73; /* VK_HOME -> kVK_Home */
    case 0x25: return 0x7b; /* VK_LEFT -> kVK_LeftArrow */
    case 0x26: return 0x7e; /* VK_UP -> kVK_UpArrow */
    case 0x27: return 0x7c; /* VK_RIGHT -> kVK_RightArrow */
    case 0x28: return 0x7d; /* VK_DOWN -> kVK_DownArrow */
    case 0x2d: return 0x72; /* VK_INSERT -> kVK_Help (Insert on PC keyboards) */
    case 0x2e: return 0x75; /* VK_DELETE -> kVK_ForwardDelete */
    case 0x2f: return 0x72; /* VK_HELP -> kVK_Help */
    case 0x5b: return 0x37; /* VK_LWIN -> kVK_Command */
    case 0x5c: return 0x36; /* VK_RWIN -> kVK_RightCommand */
    case 0x6a: return 0x43; /* VK_MULTIPLY -> kVK_ANSI_KeypadMultiply */
    case 0x6b: return 0x45; /* VK_ADD -> kVK_ANSI_KeypadPlus */
    case 0x6c: return 0x5f; /* VK_SEPARATOR -> kVK_JIS_KeypadComma */
    case 0x6d: return 0x4e; /* VK_SUBTRACT -> kVK_ANSI_KeypadMinus */
    case 0x6e: return 0x41; /* VK_DECIMAL -> kVK_ANSI_KeypadDecimal */
    case 0x6f: return 0x4b; /* VK_DIVIDE -> kVK_ANSI_KeypadDivide */
    case 0xa0: return 0x38; /* VK_LSHIFT -> kVK_Shift */
    case 0xa1: return 0x3c; /* VK_RSHIFT -> kVK_RightShift */
    case 0xa2: return 0x3b; /* VK_LCONTROL -> kVK_Control */
    case 0xa3: return 0x3e; /* VK_RCONTROL -> kVK_RightControl */
    case 0xa4: return 0x3a; /* VK_LMENU -> kVK_Option */
    case 0xa5: return 0x3d; /* VK_RMENU -> kVK_RightOption */
    case 0xad: return 0x4a; /* VK_VOLUME_MUTE -> kVK_Mute */
    case 0xae: return 0x49; /* VK_VOLUME_DOWN -> kVK_VolumeDown */
    case 0xaf: return 0x48; /* VK_VOLUME_UP -> kVK_VolumeUp */
    case 0xba: return 0x29; /* VK_OEM_1 (;:) -> kVK_ANSI_Semicolon */
    case 0xbb: return 0x18; /* VK_OEM_PLUS -> kVK_ANSI_Equal */
    case 0xbc: return 0x2b; /* VK_OEM_COMMA -> kVK_ANSI_Comma */
    case 0xbd: return 0x1b; /* VK_OEM_MINUS -> kVK_ANSI_Minus */
    case 0xbe: return 0x2f; /* VK_OEM_PERIOD -> kVK_ANSI_Period */
    case 0xbf: return 0x2c; /* VK_OEM_2 (/?) -> kVK_ANSI_Slash */
    case 0xc0: return 0x32; /* VK_OEM_3 (`~) -> kVK_ANSI_Grave */
    case 0xdb: return 0x21; /* VK_OEM_4 ([{) -> kVK_ANSI_LeftBracket */
    case 0xdc: return 0x2a; /* VK_OEM_5 (\|) -> kVK_ANSI_Backslash */
    case 0xdd: return 0x1e; /* VK_OEM_6 (]}) -> kVK_ANSI_RightBracket */
    case 0xde: return 0x27; /* VK_OEM_7 ('") -> kVK_ANSI_Quote */
    case 0xe2: return 0x0a; /* VK_OEM_102 -> kVK_ISO_Section */
    }
    return MAC_VK_NONE;
}
#else
#error You must implement VK<->Native keysym conversion for this platform.
#endif

typedef std::unordered_map< uint64_t, char * > glyph_cache;
static glyph_cache input_cache, input_cache_svg, input_cache_png, xbox_cache, controller_cache;

static const char *glyph_cache_lookup( glyph_cache &cache, const char *str, uint16_t origin,
                                       uint32_t flags, uint16_t size )
{
    uint64_t key = (uint64_t)flags << 32 | (uint64_t)size << 16 | origin;

    if (cache.find( key ) == cache.end())
    {
        char *dos_path = (char *)malloc( PATH_MAX );
        steamclient_unix_path_to_dos_path( 1, str, dos_path, PATH_MAX, 0 );
        cache[key] = dos_path;
    }

    return cache[key];
}

template< typename Iface, typename Params >
static NTSTATUS ISteamController_GetGlyphForActionOrigin( Iface *iface, Params *params, bool wow64 )
{
    if (!(params->_ret = iface->GetGlyphForActionOrigin( params->eOrigin ))) return 0;
    params->_ret = glyph_cache_lookup( controller_cache, params->_ret, params->eOrigin, 0, 0 );
    return 0;
}

template< typename Iface, typename Params >
static NTSTATUS ISteamController_GetGlyphForXboxOrigin( Iface *iface, Params *params, bool wow64 )
{
    if (!(params->_ret = iface->GetGlyphForXboxOrigin( params->eOrigin ))) return 0;
    params->_ret = glyph_cache_lookup( xbox_cache, params->_ret, params->eOrigin, 0, 0 );
    return 0;
}

template< typename Iface, typename Params >
static NTSTATUS ISteamInput_GetGlyphForActionOrigin( Iface *iface, Params *params, bool wow64 )
{
    if (!(params->_ret = iface->GetGlyphForActionOrigin( params->eOrigin ))) return 0;
    params->_ret = glyph_cache_lookup( input_cache, params->_ret, params->eOrigin, 0, 0 );
    return 0;
}

template< typename Iface, typename Params >
static NTSTATUS ISteamInput_GetGlyphForXboxOrigin( Iface *iface, Params *params, bool wow64 )
{
    if (!(params->_ret = iface->GetGlyphForXboxOrigin( params->eOrigin ))) return 0;
    params->_ret = glyph_cache_lookup( xbox_cache, params->_ret, params->eOrigin, 0, 0 );
    return 0;
}

static void (W_CDECL *w_EnableActionEventCallbacks_152)( SteamInputActionEvent_t * );
static void U_CDECL u_SteamInputActionEventCallbackPointer_152( SteamInputActionEvent_t *dat )
{
    if (w_EnableActionEventCallbacks_152) queue_cdecl_func_callback( (w_cdecl_func)w_EnableActionEventCallbacks_152, dat, sizeof(*dat) );
}

template< typename Iface, typename Params >
NTSTATUS ISteamInput_EnableActionEventCallbacks( Iface *iface, Params *params, bool wow64 )
{
    w_EnableActionEventCallbacks_152 = params->pCallback;
    iface->EnableActionEventCallbacks( params->pCallback ? &u_SteamInputActionEventCallbackPointer_152 : NULL );
    return 0;
}

template< typename Iface, typename Params >
static NTSTATUS ISteamInput_GetGlyphPNGForActionOrigin( Iface *iface, Params *params, bool wow64 )
{
    if (!(params->_ret = iface->GetGlyphPNGForActionOrigin( params->eOrigin, params->eSize, params->unFlags ))) return 0;
    params->_ret = glyph_cache_lookup( input_cache_png, params->_ret, params->eOrigin, params->unFlags, params->eSize );
    return 0;
}

template< typename Iface, typename Params >
static NTSTATUS ISteamInput_GetGlyphSVGForActionOrigin( Iface *iface, Params *params, bool wow64 )
{
    if (!(params->_ret = iface->GetGlyphSVGForActionOrigin( params->eOrigin, params->unFlags ))) return 0;
    params->_ret = glyph_cache_lookup( input_cache_svg, params->_ret, params->eOrigin, params->unFlags, 0 );
    return 0;
}

template< typename Iface, typename Params >
static NTSTATUS ISteamInput_GetGlyphForActionOrigin_Legacy( Iface *iface, Params *params, bool wow64 )
{
    if (!(params->_ret = iface->GetGlyphForActionOrigin_Legacy( params->eOrigin ))) return 0;
    params->_ret = glyph_cache_lookup( input_cache, params->_ret, params->eOrigin, 0, 0 );
    return 0;
}

LSTEAMCLIENT_UNIX_IMPL( ISteamController, SteamController005, GetGlyphForActionOrigin );

LSTEAMCLIENT_UNIX_IMPL( ISteamController, SteamController006, GetGlyphForActionOrigin );

LSTEAMCLIENT_UNIX_IMPL( ISteamController, SteamController007, GetGlyphForActionOrigin );
LSTEAMCLIENT_UNIX_IMPL( ISteamController, SteamController007, GetGlyphForXboxOrigin );

LSTEAMCLIENT_UNIX_IMPL( ISteamController, SteamController008, GetGlyphForActionOrigin );
LSTEAMCLIENT_UNIX_IMPL( ISteamController, SteamController008, GetGlyphForXboxOrigin );

LSTEAMCLIENT_UNIX_IMPL( ISteamInput, SteamInput001, GetGlyphForActionOrigin );
LSTEAMCLIENT_UNIX_IMPL( ISteamInput, SteamInput001, GetGlyphForXboxOrigin );

LSTEAMCLIENT_UNIX_IMPL( ISteamInput, SteamInput002, GetGlyphForActionOrigin );
LSTEAMCLIENT_UNIX_IMPL( ISteamInput, SteamInput002, GetGlyphForXboxOrigin );

LSTEAMCLIENT_UNIX_IMPL( ISteamInput, SteamInput005, EnableActionEventCallbacks );
LSTEAMCLIENT_UNIX_IMPL( ISteamInput, SteamInput005, GetGlyphPNGForActionOrigin );
LSTEAMCLIENT_UNIX_IMPL( ISteamInput, SteamInput005, GetGlyphSVGForActionOrigin );
LSTEAMCLIENT_UNIX_IMPL( ISteamInput, SteamInput005, GetGlyphForActionOrigin_Legacy );
LSTEAMCLIENT_UNIX_IMPL( ISteamInput, SteamInput005, GetGlyphForXboxOrigin );

LSTEAMCLIENT_UNIX_IMPL( ISteamInput, SteamInput006, EnableActionEventCallbacks );
LSTEAMCLIENT_UNIX_IMPL( ISteamInput, SteamInput006, GetGlyphPNGForActionOrigin );
LSTEAMCLIENT_UNIX_IMPL( ISteamInput, SteamInput006, GetGlyphSVGForActionOrigin );
LSTEAMCLIENT_UNIX_IMPL( ISteamInput, SteamInput006, GetGlyphForActionOrigin_Legacy );
LSTEAMCLIENT_UNIX_IMPL( ISteamInput, SteamInput006, GetGlyphForXboxOrigin );

LSTEAMCLIENT_UNIX_IMPL( ISteamInput, SteamInput007, EnableActionEventCallbacks );
LSTEAMCLIENT_UNIX_IMPL( ISteamInput, SteamInput007, GetGlyphPNGForActionOrigin );
LSTEAMCLIENT_UNIX_IMPL( ISteamInput, SteamInput007, GetGlyphSVGForActionOrigin );
LSTEAMCLIENT_UNIX_IMPL( ISteamInput, SteamInput007, GetGlyphForActionOrigin_Legacy );
LSTEAMCLIENT_UNIX_IMPL( ISteamInput, SteamInput007, GetGlyphForXboxOrigin );
