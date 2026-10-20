/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  input.c -- feeds the core's input.pad[] / input.analog[] once per frame.
 *
 *  Keyboard state is polled rather than driven from WM_KEYDOWN so that
 *  several buttons held at once behave correctly and key repeat never leaks
 *  into the emulated pad. Polling reads the global key state, so everything
 *  is gated on the window actually having focus.
 *
 *  XInput is resolved at runtime: linking against it directly would stop the
 *  executable from starting on systems where the DLL is missing, and a
 *  gamepad is optional.
 *
 *  Gamepads reach here through two independent backends, picked per pad by
 *  t_pad_map.device (see GUI_INPUT_XPAD_COUNT in gui.h):
 *
 *   - XInput, for the four modern-pad slots the rest of this file already
 *     had. Kept exactly as it was.
 *   - DirectInput, for everything XInput doesn't cover -- older joysticks,
 *     wheels, flight sticks, and any pad without an XInput-compatible
 *     driver. DirectInput itself also sees XInput-capable pads (as raw HID
 *     joysticks with none of XInput's button semantics), so those are
 *     filtered back out of the DirectInput device list on sight; a modern
 *     pad is only ever addressed through its XInput slot.
 *
 *  The two backends store completely different things in a button mask
 *  (DI_* bits vs XPAD_* bits below), so a mask is only ever meaningful
 *  alongside the device it was captured against -- every function that
 *  takes or returns one also takes that device.
 ****************************************************************************/

#include <windows.h>

#define DIRECTINPUT_VERSION 0x0800
#define COBJMACROS
#include <dinput.h>

#include "shared.h"
#include "gui.h"
#include "netplay.h"
#include "resource.h"

/****************************************************************************
 * XInput, loaded on demand
 ****************************************************************************/

#define XPAD_DPAD_UP        0x0001
#define XPAD_DPAD_DOWN      0x0002
#define XPAD_DPAD_LEFT      0x0004
#define XPAD_DPAD_RIGHT     0x0008
#define XPAD_START          0x0010
#define XPAD_BACK           0x0020
#define XPAD_LEFT_SHOULDER  0x0100
#define XPAD_RIGHT_SHOULDER 0x0200
#define XPAD_A              0x1000
#define XPAD_B              0x2000
#define XPAD_X              0x4000
#define XPAD_Y              0x8000
#define XPAD_LEFT_TRIGGER   0x00010000
#define XPAD_RIGHT_TRIGGER  0x00020000
#define XPAD_RS_UP          0x00040000   /* the right stick, as four more buttons */
#define XPAD_RS_DOWN        0x00080000
#define XPAD_RS_LEFT        0x00100000
#define XPAD_RS_RIGHT       0x00200000

#define XPAD_TRIGGER_THRESHOLD 30   /* out of 0-255; comfortably past resting noise */

#define XPAD_MAX_DEVICES    GUI_INPUT_XPAD_COUNT   /* gui.h -- also where DirectInput's own device range starts */
#define XPAD_DEADZONE       10000

/* gui.deadzone[] is 0-100%, the units a person actually sees and adjusts on
   the slider; everywhere else in this file wants it in XInput's own
   -32768..32767 raw units instead. Falls back to this file's own prior
   fixed constant, scaled to a percentage, for a player index outside 0/1 --
   should never actually happen, but a sensible default beats an out-of-
   bounds array read if it ever did. */
static int deadzone_raw(int player)
{
  int pct = (player == 0 || player == 1) ? gui.deadzone[player] : 30;

  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return (pct * 32767) / 100;
}

/* Applies a deadzone to one raw analog stick axis (-32768..32767): exactly
   centered (returns 0) anywhere within the deadzone, unchanged otherwise --
   deliberately not also rescaling the remaining range, so this only adds
   the deadzone itself and doesn't change this file's existing analog
   sensitivity/range in the process. */
static int deadzone_clamp(int raw, int dz)
{
  int mag = (raw < 0) ? -raw : raw;
  return (mag <= dz) ? 0 : raw;
}

typedef struct
{
  WORD  wButtons;
  BYTE  bLeftTrigger;
  BYTE  bRightTrigger;
  SHORT sThumbLX;
  SHORT sThumbLY;
  SHORT sThumbRX;
  SHORT sThumbRY;
} XPAD_GAMEPAD;

typedef struct
{
  DWORD        dwPacketNumber;
  XPAD_GAMEPAD Gamepad;
} XPAD_STATE;

typedef DWORD (WINAPI *xinput_get_state_fn)(DWORD, XPAD_STATE *);

static HMODULE             xinput_dll;
static xinput_get_state_fn xinput_get_state;

/* Cached once per frame so four devices do not mean four polls each. */
static XPAD_STATE xpad_state[XPAD_MAX_DEVICES];
static int        xpad_connected[XPAD_MAX_DEVICES];

static int  has_focus = 1;
static int  frame_advance_armed;

/****************************************************************************
 * DirectInput, for joysticks/pads XInput doesn't cover
 *
 * Devices are polled through IDirectInputDevice8::GetDeviceState rather than
 * the buffered/event-driven API: this file already re-reads everything once
 * a frame (and, for the fast-forward/rewind hotkeys, on demand outside the
 * frame loop too), so there is nothing to buffer between reads.
 ****************************************************************************/

/* First 24 joystick buttons as individual bits, then the D-pad/POV/stick
   folded together the same way gamepad_buttons() already folds XInput's
   thumbstick into its own d-pad bits below -- one set of 4 "direction" bits
   fed from whichever of POV hat or main stick is actually deflected, rather
   than separate, individually mappable bits for each. Distinct numeric
   values from the XPAD_* bits above are not required (a mask is only ever
   compared against another mask captured for the very same device, never
   across backends), but bits 24-27 are used here anyway to leave buttons
   25-32 of a device with that many free for the future without renumbering
   anything already saved to genesis-plus-ex.ini. */
#define DI_MAX_BUTTONS   24
#define DI_BTN(n)        (1u << (n))
#define DI_DPAD_UP       (1u << 24)
#define DI_DPAD_DOWN     (1u << 25)
#define DI_DPAD_LEFT     (1u << 26)
#define DI_DPAD_RIGHT    (1u << 27)
#define DI_RS_UP         (1u << 28)   /* the right stick, as four more buttons */
#define DI_RS_DOWN       (1u << 29)
#define DI_RS_LEFT       (1u << 30)
#define DI_RS_RIGHT      (1u << 31)

static IDirectInput8A *di_iface;

typedef struct
{
  GUID guidInstance;
  char name[64];
} di_joy_info;

static di_joy_info          joy_info[GUI_INPUT_DI_MAX];
static int                  joy_count;
static IDirectInputDevice8A *joy_dev[GUI_INPUT_DI_MAX];
static DIJOYSTATE2          joy_state[GUI_INPUT_DI_MAX];
static int                  joy_rs_pair[GUI_INPUT_DI_MAX];       /* 0 = Rx/Ry is the right stick, 1 = Z/Rz, -1 = none */
static LONG                 joy_rs_base[GUI_INPUT_DI_MAX][2];    /* where those axes rested when first read */
static int                  joy_rs_base_ok[GUI_INPUT_DI_MAX];
static int                  joy_connected[GUI_INPUT_DI_MAX];

/* Same slow-reprobe idea as xpad_known[]/xpad_next_probe[] below: a device
   that failed to Acquire (unplugged, or another app holding it exclusively)
   is only retried about once a second rather than on every single call. */
static int   joy_known[GUI_INPUT_DI_MAX];
static DWORD joy_next_probe[GUI_INPUT_DI_MAX];

static int is_di_device(int device)
{
  return device >= GUI_INPUT_XPAD_COUNT && device < GUI_INPUT_XPAD_COUNT + GUI_INPUT_DI_MAX;
}

/* DirectInput enumerates XInput-capable pads too, as plain HID joysticks
   with none of XInput's button/trigger semantics -- this app already talks
   to those through the XInput backend above, so they're filtered back out
   here rather than showing up a second time as an unlabelled generic
   joystick. Standard technique (see the DirectX SDK's XInput/DirectInput
   sample): an XInput device's underlying HID interface name always
   contains "IG_", and DirectInput's own product GUID happens to pack the
   same vendor/product IDs Raw Input reports for that HID device into its
   first 4 bytes, so matching those two together identifies it without ever
   opening the device itself. */
typedef struct
{
  RAWINPUTDEVICELIST *list;
  UINT                 count;
} raw_input_snapshot;

/* One GetRawInputDeviceList() call, reused for every candidate device
   di_enum_callback() sees below -- system-wide raw input device count
   doesn't change between those calls, so re-fetching and re-scanning the
   whole list from scratch per candidate (as this used to) turns an
   attached-joystick count of N and a system HID device count of M into
   O(N*M) work for no reason; snapshotting it once here is O(N+M). */
static void raw_input_snapshot_take(raw_input_snapshot *snap)
{
  UINT count = 0;

  snap->list = NULL;
  snap->count = 0;

  if (GetRawInputDeviceList(NULL, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0)
    return;

  snap->list = (RAWINPUTDEVICELIST *)malloc(sizeof(RAWINPUTDEVICELIST) * count);
  if (!snap->list) return;

  if (GetRawInputDeviceList(snap->list, &count, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1)
  {
    free(snap->list);
    snap->list = NULL;
    return;
  }

  snap->count = count;
}

static void raw_input_snapshot_free(raw_input_snapshot *snap)
{
  if (snap->list) free(snap->list);
  snap->list = NULL;
  snap->count = 0;
}

static int is_xinput_device(const GUID *guid_product, const raw_input_snapshot *snap)
{
  UINT i;
  int found = 0;

  for (i = 0; i < snap->count && !found; i++)
  {
    RID_DEVICE_INFO info;
    UINT size;
    UINT namelen = 0;
    char *name;

    if (snap->list[i].dwType != RIM_TYPEHID) continue;

    info.cbSize = sizeof(info);
    size = sizeof(info);
    if ((int)GetRawInputDeviceInfoA(snap->list[i].hDevice, RIDI_DEVICEINFO, &info, &size) <= 0)
      continue;

    if ((LONG)MAKELONG((WORD)info.hid.dwVendorId, (WORD)info.hid.dwProductId) != (LONG)guid_product->Data1)
      continue;

    GetRawInputDeviceInfoA(snap->list[i].hDevice, RIDI_DEVICENAME, NULL, &namelen);
    if (namelen == 0 || namelen > 4096) continue;

    name = (char *)malloc(namelen);
    if (!name) continue;

    if ((int)GetRawInputDeviceInfoA(snap->list[i].hDevice, RIDI_DEVICENAME, name, &namelen) >= 0 &&
        strstr(name, "IG_"))
    {
      found = 1;
    }
    free(name);
  }

  return found;
}

/* The product names DirectInput gives the XInput (Xbox-style) pads, in the order it lists them: the Controls
   window shows them instead of "Gamepad 1" (XInput itself has no names). */
static char xi_names[4][64];
static int  xi_count;

static BOOL CALLBACK di_enum_callback(const DIDEVICEINSTANCEA *inst, void *ref)
{
  const raw_input_snapshot *snap = (const raw_input_snapshot *)ref;

  if (joy_count >= GUI_INPUT_DI_MAX) return DIENUM_STOP;
  if (is_xinput_device(&inst->guidProduct, snap))
  {
    if (xi_count < 4) lstrcpynA(xi_names[xi_count++], inst->tszProductName, sizeof(xi_names[0]));
    return DIENUM_CONTINUE;
  }

  joy_info[joy_count].guidInstance = inst->guidInstance;
  lstrcpynA(joy_info[joy_count].name, inst->tszProductName, sizeof(joy_info[joy_count].name));
  joy_count++;
  return DIENUM_CONTINUE;
}

/* Releases every currently-open device and re-enumerates from scratch, so a
   joystick plugged in after this app started shows up too. Indices can
   shift across a refresh (the OS enumeration order isn't a stable identity),
   which is exactly what t_pad_map.joy_name/gui_input_resolve_device() below
   exist to paper over. */
void gui_input_refresh_joysticks(void)
{
  int i;

  for (i = 0; i < GUI_INPUT_DI_MAX; i++)
  {
    if (joy_dev[i])
    {
      IDirectInputDevice8_Unacquire(joy_dev[i]);
      IDirectInputDevice8_Release(joy_dev[i]);
      joy_dev[i] = NULL;
    }
    joy_known[i] = 0;
    joy_next_probe[i] = 0;
    joy_connected[i] = 0;
    ZeroMemory(&joy_state[i], sizeof(joy_state[i]));
  }

  joy_count = 0;
  xi_count = 0;

  /* Created here, lazily, on the first call -- rather than unconditionally
     at startup -- so a session where nobody ever opens the config dialog
     never touches DirectInput (or any attached joystick) at all. Whatever
     interaction it is that some XInput pads apparently don't like sharing
     their HID interface with, this keeps it strictly opt-in: it can only
     start once someone has actually asked to see the device list. */
  if (!di_iface)
  {
    if (FAILED(DirectInput8Create(g_inst, DIRECTINPUT_VERSION, &IID_IDirectInput8A, (void **)&di_iface, NULL)))
    {
      di_iface = NULL;
      return;
    }
  }

  {
    raw_input_snapshot snap;

    raw_input_snapshot_take(&snap);
    IDirectInput8_EnumDevices(di_iface, DI8DEVCLASS_GAMECTRL, di_enum_callback, &snap, DIEDFL_ATTACHEDONLY);
    raw_input_snapshot_free(&snap);
  }

  /* Both players, not just whichever one's dialog triggered this refresh --
     a device's index can shift for everyone once enumeration order changes
     (a joystick replugged, another one plugged in first), and leaving the
     other player's mapping on its old, now-possibly-wrong index until they
     separately happen to open their own dialog would silently hand them a
     different controller's input in the meantime. gui_input_resolve_device()
     itself is a no-op for a slot that isn't a DirectInput device or has no
     name to match, so this costs nothing for the common XInput-only case. */
  gui.pad[0].device = gui_input_resolve_device(gui.pad[0].device, gui.pad[0].joy_name, -1);
  gui.pad[1].device = gui_input_resolve_device(gui.pad[1].device, gui.pad[1].joy_name, gui.pad[0].device);
}

/* A USB / Bluetooth device came or went: look for joysticks again. A joystick that just appeared becomes
   Player 1's pad when Player 1 has no working pad (an XInput pad that is plugged in works the same way). */
static int xpad_poll_slot(int d, int force);       /* (defined with xpad_query below) */
static CRITICAL_SECTION xp_cs;
static HANDLE xp_thread;
static volatile LONG xp_quit;
static DWORD WINAPI xpad_thread_proc(LPVOID arg);

/* Which XInput slots have a pad: a bit each. Asked afresh (the once-a-second probing of empty slots is skipped). */
static int xpad_connected_mask(void)
{
  int s, m = 0;
  for (s = 0; s < XPAD_MAX_DEVICES; s++)
  {
    if (xinput_get_state) xpad_poll_slot(s, 1);
    if (gui_input_xpad_connected(s)) m |= 1 << s;
  }
  return m;
}

static int xpad_prev_mask = -1;       /* -1: not taken yet */
static char xpad_last_name[XPAD_MAX_DEVICES][64];   /* what each slot was called while it had a pad, for the "disconnected" message */

static void xpad_remember_names(int mask)
{
  int s;
  for (s = 0; s < XPAD_MAX_DEVICES; s++)
    if (mask & (1 << s)) lstrcpynA(xpad_last_name[s], gui_input_xpad_name(s)[0] ? gui_input_xpad_name(s) : "Gamepad", sizeof(xpad_last_name[s]));
}

void gui_input_devices_changed(void)
{
  GUID old[GUI_INPUT_DI_MAX];
  char old_name[GUI_INPUT_DI_MAX][64];
  int n_old = joy_count, i, k, fresh = -1;
  int cur_x, fresh_x = -1, d0;

  DWORD scan_t0 = GetTickCount();

  gui_diag_start();

  for (i = 0; i < n_old; i++) { old[i] = joy_info[i].guidInstance; lstrcpynA(old_name[i], joy_info[i].name, sizeof(old_name[i])); }
  gui_input_refresh_joysticks();
  gui_log("Controller change: looking for joysticks took %lu ms", (unsigned long)(GetTickCount() - scan_t0));

  /* Pads that just went away (an Xbox-style slot that is empty now, or a joystick no longer listed). */
  cur_x = xpad_connected_mask();
  if (xpad_prev_mask >= 0)
    for (i = 0; i < XPAD_MAX_DEVICES; i++)
    {
      if ((xpad_prev_mask & ~cur_x) & (1 << i))
      {
        gui_notify("Controller disconnected: %s", xpad_last_name[i][0] ? xpad_last_name[i] : "Gamepad");
        if (gui.pad[0].device == i) gui_pad_lost(xpad_last_name[i][0] ? xpad_last_name[i] : "Gamepad");
      }
    }
  for (k = 0; k < n_old; k++)
  {
    int still = 0;
    for (i = 0; i < joy_count; i++) if (IsEqualGUID(&old[k], &joy_info[i].guidInstance)) still = 1;
    if (!still)
    {
      gui_notify("Controller disconnected: %s", old_name[k]);
      if (gui.pad[0].device >= XPAD_MAX_DEVICES && gui.pad[0].joy_name[0] && lstrcmpiA(gui.pad[0].joy_name, old_name[k]) == 0)
        gui_pad_lost(old_name[k]);
    }
  }

  /* An Xbox-style pad that just appeared. */
  if (xpad_prev_mask >= 0)
    for (i = 0; i < XPAD_MAX_DEVICES && fresh_x < 0; i++)
      if ((cur_x & ~xpad_prev_mask) & (1 << i)) fresh_x = i;
  xpad_prev_mask = cur_x;
  xpad_remember_names(cur_x);
  d0 = gui.pad[0].device;
  if (fresh_x >= 0)
  {
    /* Always announced; Player 1 only moves to it when it has no working pad of its own. */
    if (!((d0 >= 0 && d0 < XPAD_MAX_DEVICES && (cur_x & (1 << d0))) || (d0 >= XPAD_MAX_DEVICES && d0 - XPAD_MAX_DEVICES < joy_count)))
    {
      gui.pad[0].device = fresh_x;
      gui.pad[0].joy_name[0] = '\0';
    }
    gui_notify("Controller connected: %s", gui_input_xpad_name(fresh_x)[0] ? gui_input_xpad_name(fresh_x) : "Gamepad");
    { int d1 = gui.pad[0].device; if (d1 >= 0 && d1 < XPAD_MAX_DEVICES && (cur_x & (1 << d1))) gui_pad_back(gui_input_xpad_name(d1)); }
    return;
  }

  for (i = 0; i < joy_count && fresh < 0; i++)
  {
    int known = 0;
    for (k = 0; k < n_old; k++) if (IsEqualGUID(&old[k], &joy_info[i].guidInstance)) known = 1;
    if (!known) fresh = i;
  }

  if (fresh >= 0)
  {
    int d = gui.pad[0].device;
    int working = (d >= 0 && d < XPAD_MAX_DEVICES && gui_input_xpad_connected(d)) ||
                  (d >= XPAD_MAX_DEVICES && d - XPAD_MAX_DEVICES < joy_count);
    if (!working)
    {
      gui.pad[0].device = XPAD_MAX_DEVICES + fresh;
      lstrcpynA(gui.pad[0].joy_name, joy_info[fresh].name, sizeof(gui.pad[0].joy_name));
    }
    gui_notify("Controller connected: %s", joy_info[fresh].name);   /* always announced, as for an Xbox-style pad */
    { int d1 = gui.pad[0].device; if (d1 >= XPAD_MAX_DEVICES && d1 - XPAD_MAX_DEVICES < joy_count) gui_pad_back(joy_info[d1 - XPAD_MAX_DEVICES].name); }
  }
}

int gui_input_joystick_count(void)
{
  return joy_count;
}

const char *gui_input_joystick_name(int index)
{
  if (index < 0 || index >= joy_count) return "";
  return joy_info[index].name;
}

/* avoid_device: a device value (typically the other player's, already
   resolved) this call must never return, even if its name is also a match --
   two identical controller models is a common two-player setup, and without
   this both players' by-name search would happily return the very same
   physical device, leaving the other one unpolled and unusable. Pass -1 (or
   any value that is never a DirectInput device, like an XInput slot) when
   there is nothing to avoid. */
int gui_input_resolve_device(int device, const char *joy_name, int avoid_device)
{
  int i, cur;

  if (!is_di_device(device) || !joy_name || !joy_name[0]) return device;

  /* Keeps the index this mapping already has, if it's still a valid name
     match, rather than researching from scratch every refresh -- besides
     being pointless extra work, a fresh search has no way to prefer "the
     one this player already had" over "the one a same-model device
     happens to enumerate first", so this also avoids two same-model
     controllers trading assignments back and forth across refreshes. */
  cur = device - GUI_INPUT_XPAD_COUNT;
  if (device != avoid_device && cur >= 0 && cur < joy_count &&
      lstrcmpiA(joy_info[cur].name, joy_name) == 0)
  {
    return device;
  }

  for (i = 0; i < joy_count; i++)
  {
    int candidate = GUI_INPUT_XPAD_COUNT + i;
    if (candidate != avoid_device && lstrcmpiA(joy_info[i].name, joy_name) == 0) return candidate;
  }

  return device;   /* not currently attached -- keeps reading as disconnected rather than losing the assignment */
}

/* Opens the device and configures it the first time something actually asks
   for its state -- not at enumeration time, since that happens before
   create_main_window() gives this file a window handle to hand
   SetCooperativeLevel(), and there is no reason to open every attached
   joystick if only one of them ends up mapped to a player. */
static int di_ensure_acquired(int idx)
{
  DIPROPRANGE range;
  HRESULT hr;

  if (idx < 0 || idx >= joy_count) return 0;
  if (joy_dev[idx]) return 1;
  if (!di_iface || !g_hwnd) return 0;

  hr = IDirectInput8_CreateDevice(di_iface, &joy_info[idx].guidInstance, &joy_dev[idx], NULL);
  if (FAILED(hr))
  {
    joy_dev[idx] = NULL;
    return 0;
  }

  IDirectInputDevice8_SetDataFormat(joy_dev[idx], &c_dfDIJoystick2);
  /* Background: this app's own "keep controller input working while
     unfocused" option (gui.background_input) already gates every call in
     here on pad_input_allowed(), so the device itself doesn't need to
     refuse input the moment focus moves elsewhere. Non-exclusive: nothing
     here needs to stop other applications (or a second instance) from also
     reading the same joystick. */
  IDirectInputDevice8_SetCooperativeLevel(joy_dev[idx], g_hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);

  /* Ranged to XInput's own -32768..32767 so every place downstream that
     folds a stick into a d-pad or scales it by gui.deadzone[] treats both
     backends identically. A device that refuses this (rare) just keeps
     whatever narrower default range it already reports -- deadzone
     thresholds computed for the wider range then end up too small to
     matter for it, not too large, so the worst case is a d-pad that goes
     digital a bit earlier than the slider says, never one that doesn't
     respond at all. */
  ZeroMemory(&range, sizeof(range));
  range.diph.dwSize = sizeof(range);
  range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
  range.diph.dwHow = DIPH_BYOFFSET;
  range.lMin = -32768;
  range.lMax = 32767;

  range.diph.dwObj = DIJOFS_X;
  IDirectInputDevice8_SetProperty(joy_dev[idx], DIPROP_RANGE, &range.diph);
  range.diph.dwObj = DIJOFS_Y;
  IDirectInputDevice8_SetProperty(joy_dev[idx], DIPROP_RANGE, &range.diph);
  /* The right stick's axes get the same range (a missing axis just refuses). */
  range.diph.dwObj = DIJOFS_Z;   IDirectInputDevice8_SetProperty(joy_dev[idx], DIPROP_RANGE, &range.diph);
  range.diph.dwObj = DIJOFS_RX;  IDirectInputDevice8_SetProperty(joy_dev[idx], DIPROP_RANGE, &range.diph);
  range.diph.dwObj = DIJOFS_RY;  IDirectInputDevice8_SetProperty(joy_dev[idx], DIPROP_RANGE, &range.diph);
  range.diph.dwObj = DIJOFS_RZ;  IDirectInputDevice8_SetProperty(joy_dev[idx], DIPROP_RANGE, &range.diph);
  {
    /* Which axes are the right stick: Rx / Ry on Xbox-style pads, Z / Rz on PlayStation-style ones. */
    DIDEVICEOBJECTINSTANCEA oi;
    ZeroMemory(&oi, sizeof(oi));
    oi.dwSize = sizeof(oi);
    if (SUCCEEDED(IDirectInputDevice8_GetObjectInfo(joy_dev[idx], &oi, DIJOFS_RX, DIPH_BYOFFSET)))
    {
      ZeroMemory(&oi, sizeof(oi));
      oi.dwSize = sizeof(oi);
      joy_rs_pair[idx] = SUCCEEDED(IDirectInputDevice8_GetObjectInfo(joy_dev[idx], &oi, DIJOFS_RY, DIPH_BYOFFSET)) ? 0 : -1;
    }
    else joy_rs_pair[idx] = -1;
    if (joy_rs_pair[idx] < 0)
    {
      ZeroMemory(&oi, sizeof(oi));
      oi.dwSize = sizeof(oi);
      if (SUCCEEDED(IDirectInputDevice8_GetObjectInfo(joy_dev[idx], &oi, DIJOFS_Z, DIPH_BYOFFSET)))
      {
        ZeroMemory(&oi, sizeof(oi));
        oi.dwSize = sizeof(oi);
        if (SUCCEEDED(IDirectInputDevice8_GetObjectInfo(joy_dev[idx], &oi, DIJOFS_RZ, DIPH_BYOFFSET))) joy_rs_pair[idx] = 1;
      }
    }
    joy_rs_base_ok[idx] = 0;
  }

  IDirectInputDevice8_Acquire(joy_dev[idx]);
  return 1;
}

static int di_query(int idx, DIJOYSTATE2 *out)
{
  DWORD now = GetTickCount();
  HRESULT hr;

  if (idx < 0 || idx >= joy_count) return 0;
  if (!joy_known[idx] && (int)(now - joy_next_probe[idx]) < 0) return 0;

  if (!di_ensure_acquired(idx))
  {
    joy_known[idx] = 0;
    joy_next_probe[idx] = now + 1000;
    return 0;
  }

  IDirectInputDevice8_Poll(joy_dev[idx]);   /* some devices need this pumped; harmless on the rest */
  hr = IDirectInputDevice8_GetDeviceState(joy_dev[idx], sizeof(DIJOYSTATE2), out);

  if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)
  {
    if (SUCCEEDED(IDirectInputDevice8_Acquire(joy_dev[idx])))
      hr = IDirectInputDevice8_GetDeviceState(joy_dev[idx], sizeof(DIJOYSTATE2), out);
  }

  if (FAILED(hr))
  {
    joy_known[idx] = 0;
    joy_next_probe[idx] = now + 1000;
    return 0;
  }

  joy_known[idx] = 1;
  return 1;
}

/* Every joystick button plus the POV hat, folded into the bits above -- just
   the parts of gamepad_buttons()'s job that don't need a deadzone. Shared by
   the once-per-frame cached poll and the on-demand fresh reads (hotkeys
   during rewind, and the "listening" capture in the config dialog) so the
   two never disagree about what a given raw state means. */
static DWORD di_raw_buttons(const DIJOYSTATE2 *js)
{
  DWORD b = 0;
  DWORD pov = js->rgdwPOV[0];
  int i;

  for (i = 0; i < DI_MAX_BUTTONS; i++)
  {
    if (js->rgbButtons[i] & 0x80) b |= DI_BTN(i);
  }

  if (LOWORD(pov) != 0xFFFF)   /* 0xFFFF (all bits of the low word set) = hat centered */
  {
    int deg = (int)(pov / 100);   /* hundredths of a degree, clockwise from north */

    if (deg >= 315 || deg <= 45)  b |= DI_DPAD_UP;
    if (deg >= 45  && deg <= 135) b |= DI_DPAD_RIGHT;
    if (deg >= 135 && deg <= 225) b |= DI_DPAD_DOWN;
    if (deg >= 225 && deg <= 315) b |= DI_DPAD_LEFT;
  }

  return b;
}

/****************************************************************************
 * Setup
 ****************************************************************************/

void gui_input_init(void)
{
  static const char *candidates[] =
  {
    "xinput1_4.dll",     /* Windows 8 and later  */
    "xinput1_3.dll",     /* DirectX SDK redist   */
    "xinput9_1_0.dll",   /* Windows 7 in-box     */
    NULL
  };
  int i;

  for (i = 0; candidates[i]; i++)
  {
    xinput_dll = LoadLibraryExA(candidates[i], NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (xinput_dll) break;
  }

  InitializeCriticalSection(&xp_cs);
  if (xinput_dll)
  {
    xinput_get_state =
      (xinput_get_state_fn)(void *)GetProcAddress(xinput_dll, "XInputGetState");
  }
  if (xinput_get_state)
  {
    xp_quit = 0;
    xp_thread = CreateThread(NULL, 0, xpad_thread_proc, NULL, 0, NULL);
    if (xp_thread) SetThreadPriority(xp_thread, THREAD_PRIORITY_ABOVE_NORMAL);
  }

  /* DirectInput itself is not set up here -- see gui_input_refresh_joysticks(),
     which creates it lazily on first use (the config dialog opening) rather
     than unconditionally on every launch. A player saved on a joystick is the exception: it has to be
     found again for the game to use it. */
  if (gui.pad[0].joy_name[0] || gui.pad[1].joy_name[0]) gui_input_refresh_joysticks();
  xpad_prev_mask = xpad_connected_mask();
  xpad_remember_names(xpad_prev_mask);
}

void gui_input_shutdown(void)
{
  int i;

  for (i = 0; i < GUI_INPUT_DI_MAX; i++)
  {
    if (joy_dev[i])
    {
      IDirectInputDevice8_Unacquire(joy_dev[i]);
      IDirectInputDevice8_Release(joy_dev[i]);
      joy_dev[i] = NULL;
    }
  }
  joy_count = 0;

  if (di_iface)
  {
    IDirectInput8_Release(di_iface);
    di_iface = NULL;
  }

  if (xp_thread)
  {
    xp_quit = 1;
    if (WaitForSingleObject(xp_thread, 1000) != WAIT_OBJECT_0) xinput_dll = NULL;     /* stuck inside the driver: the library stays loaded, not pulled from under it */
    CloseHandle(xp_thread);
    xp_thread = NULL;
  }
  if (xinput_dll)
  {
    FreeLibrary(xinput_dll);
    xinput_dll = NULL;
    xinput_get_state = NULL;
  }
}

void gui_input_set_focus(int focused)
{
  has_focus = focused;
  if (!focused) gui_combos_release();          /* like keys that are let go when the window loses the focus */
}

/****************************************************************************
 * Polling helpers
 ****************************************************************************/

/* Tracks the last time either Alt or Ctrl was seen held, so a brief
   grace period after release (below) can still suppress game input --
   not just while the modifier is physically down. Both are shortcut
   modifiers used throughout this app's own accelerator table (Ctrl+O,
   Ctrl+R, Ctrl+C, etc., alongside Alt+Enter), so a key that's also
   mapped to a Genesis button shouldn't leak through as gameplay input
   during or immediately around one of those combos. */
static DWORD last_modifier_seen;

/* Controller input (pad keys, gamepads) can optionally keep working while
   another window has focus. Hotkeys and the mouse never do: typing digits
   or clicking elsewhere must not trigger save slots or lightgun shots. */
static int pad_input_allowed(void)
{
  return has_focus || gui.background_input;
}

/* Ctrl and Alt themselves. A pad button assigned to one of these has to work
   while it is held, so the shortcut-modifier suppression below cannot apply. */
static int is_ctrl_alt_key(int vk)
{
  switch (vk)
  {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU:    case VK_LMENU:    case VK_RMENU:
      return 1;
    default:
      return 0;
  }
}

static int key_down_any(int vk, int allowed)
{
  if (!vk || !allowed) return 0;

  if (is_ctrl_alt_key(vk)) return (GetAsyncKeyState(vk) & 0x8000) != 0;

  if ((GetAsyncKeyState(VK_MENU) & 0x8000) || (GetAsyncKeyState(VK_CONTROL) & 0x8000))
  {
    last_modifier_seen = GetTickCount();
    return 0;
  }
  if (GetTickCount() - last_modifier_seen < 1000) return 0;

  return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static int key_down(int vk)      { return key_down_any(vk, has_focus); }            /* hotkeys */
static int pad_key_down(int vk)  { return key_down_any(vk, pad_input_allowed()); }  /* controller keys */

/* Asking XInput about an empty controller slot can be slow (older XInput DLLs
   look for hardware each time), so a slot that was empty is only tried again
   about once a second. */
static int   xpad_known[XPAD_MAX_DEVICES];
static DWORD xpad_next_probe[XPAD_MAX_DEVICES];

/* XInputGetState() is a call into the driver and can take from a few to dozens of milliseconds (a pad that was just
   plugged in, a wireless pad waking up, the system busy with the device change). Called from the frame loop that is a
   dropped frame. A thread of its own asks every 2 ms and keeps the answer; the frame loop only copies it. */
static XPAD_STATE xp_snap[XPAD_MAX_DEVICES];

/* Asks the driver about one slot (force: even one that was empty a moment ago) and stores the answer. */
static int xpad_poll_slot(int d, int force)
{
  DWORD now = GetTickCount(), t0, r, took;
  XPAD_STATE st;
  int known, due;

  EnterCriticalSection(&xp_cs);
  known = xpad_known[d];
  due = force || known || (int)(now - xpad_next_probe[d]) >= 0;
  LeaveCriticalSection(&xp_cs);
  if (!due) return 0;

  ZeroMemory(&st, sizeof(st));
  t0 = GetTickCount();
  r = xinput_get_state((DWORD)d, &st);
  took = GetTickCount() - t0;
  if (took >= 8) gui_log("XInput slot %d took %lu ms to answer (%s)", d, (unsigned long)took, r == ERROR_SUCCESS ? "connected" : "empty");   /* for finding a slow frame */

  EnterCriticalSection(&xp_cs);
  if (r == ERROR_SUCCESS)
  {
    xp_snap[d] = st;
    xpad_known[d] = 1;
  }
  else
  {
    xpad_known[d] = 0;
    xpad_next_probe[d] = now + 2000;     /* an empty slot is only tried again about every two seconds */
  }
  LeaveCriticalSection(&xp_cs);
  return r == ERROR_SUCCESS;
}

static DWORD WINAPI xpad_thread_proc(LPVOID arg)
{
  (void)arg;
  while (!xp_quit)
  {
    int i;
    for (i = 0; i < XPAD_MAX_DEVICES; i++) xpad_poll_slot(i, 0);
    Sleep(2);
  }
  return 0;
}

static int xpad_query(int device, XPAD_STATE *st)
{
  int ok;

  if (!xinput_get_state || device < 0 || device >= XPAD_MAX_DEVICES) return 0;
  if (!xp_thread) xpad_poll_slot(device, 0);       /* no thread (it could not be started): ask directly */

  EnterCriticalSection(&xp_cs);
  ok = xpad_known[device];
  if (ok) *st = xp_snap[device];
  LeaveCriticalSection(&xp_cs);
  return ok;
}

/* The name of the pad in this XInput slot ("" when not known): the nth connected slot gets the nth name
   DirectInput listed for an XInput pad (the usual order), which is a best guess when pads were swapped about. */
const char *gui_input_xpad_name(int slot)
{
  int k, rank = 0;

  if (slot < 0 || slot >= XPAD_MAX_DEVICES || !gui_input_xpad_connected(slot)) return "";
  for (k = 0; k < slot; k++) if (gui_input_xpad_connected(k)) rank++;
  return rank < xi_count ? xi_names[rank] : "";
}

/* Whether an XInput pad is plugged into this slot right now (for the Controls window's device list). */
int gui_input_xpad_connected(int slot)
{
  XPAD_STATE st;
  return xpad_query(slot, &st);
}

/* Whether some player is actually mapped to this DirectInput device right
   now. Unlike XInputGetState() -- a stateless, side-effect-free poll that
   was already being called for all four slots regardless of whether
   anything used them -- a DirectInput device has to be opened and
   Acquire()'d before it can be read at all, and holding it open is a real,
   continuous side effect: it contends with any other application (or this
   one's own XInput backend, for a pad whose separate DirectInput-visible
   HID interface didn't get filtered out by is_xinput_device()) also trying
   to read that same physical device. Polling every attached joystick every
   frame regardless of whether anyone chose it was exactly that -- silently
   acquiring hardware nobody asked this app to touch, XInput pads included. */
static int di_wanted(int idx)
{
  int dev = GUI_INPUT_XPAD_COUNT + idx;
  return gui.pad[0].device == dev || gui.pad[1].device == dev;
}

static void poll_gamepads(void)
{
  int i;

  for (i = 0; i < XPAD_MAX_DEVICES; i++)
  {
    xpad_connected[i] = 0;
    ZeroMemory(&xpad_state[i], sizeof(XPAD_STATE));
  }
  for (i = 0; i < GUI_INPUT_DI_MAX; i++)
  {
    joy_connected[i] = 0;
    ZeroMemory(&joy_state[i], sizeof(joy_state[i]));
  }

  if (!pad_input_allowed()) return;

  if (xinput_get_state)
  {
    for (i = 0; i < XPAD_MAX_DEVICES; i++)
    {
      if (xpad_query(i, &xpad_state[i])) xpad_connected[i] = 1;
    }
  }

  for (i = 0; i < joy_count; i++)
  {
    if (di_wanted(i) && di_query(i, &joy_state[i])) joy_connected[i] = 1;
  }
}

/* Raw stick position, X and Y each -32768..32767, in XInput's own sign
   convention (Y positive = up) regardless of which backend actually holds
   the device -- DirectInput reports Y the opposite way round (down is
   positive), so the DI branch below flips it on the way out rather than
   leaving every caller to remember which backend it's talking to. Returns
   0, leaving the outputs untouched, for an unmapped or disconnected
   device. */
/* The right stick as four buttons. */
static DWORD xpad_rs_bits(const XPAD_STATE *st, int dz)
{
  DWORD b = 0;
  if (st->Gamepad.sThumbRY >  dz) b |= XPAD_RS_UP;
  if (st->Gamepad.sThumbRY < -dz) b |= XPAD_RS_DOWN;
  if (st->Gamepad.sThumbRX < -dz) b |= XPAD_RS_LEFT;
  if (st->Gamepad.sThumbRX >  dz) b |= XPAD_RS_RIGHT;
  return b;
}

/* Same for a DirectInput pad. Triggers can share these axes and rest at one end, so the
   deflection is measured from where the axis rested when first read. */
static DWORD di_rs_bits(int idx, const DIJOYSTATE2 *st, int dz)
{
  LONG x, y;
  DWORD b = 0;

  if (idx < 0 || idx >= GUI_INPUT_DI_MAX || joy_rs_pair[idx] < 0) return 0;
  if (joy_rs_pair[idx] == 0) { x = st->lRx; y = st->lRy; }
  else                       { x = st->lZ;  y = st->lRz; }
  if (!joy_rs_base_ok[idx]) { joy_rs_base[idx][0] = x; joy_rs_base[idx][1] = y; joy_rs_base_ok[idx] = 1; }
  x -= joy_rs_base[idx][0];
  y -= joy_rs_base[idx][1];
  if (y >  dz) b |= DI_RS_DOWN;
  if (y < -dz) b |= DI_RS_UP;
  if (x < -dz) b |= DI_RS_LEFT;
  if (x >  dz) b |= DI_RS_RIGHT;
  return b;
}

static int pad_stick(int device, int *x, int *y)
{
  if (device >= 0 && device < XPAD_MAX_DEVICES)
  {
    if (!xpad_connected[device]) return 0;
    *x = xpad_state[device].Gamepad.sThumbLX;
    *y = xpad_state[device].Gamepad.sThumbLY;
    return 1;
  }

  if (is_di_device(device))
  {
    int idx = device - XPAD_MAX_DEVICES;
    if (!joy_connected[idx]) return 0;
    *x = joy_state[idx].lX;
    *y = -joy_state[idx].lY;
    return 1;
  }

  return 0;
}

/* Buttons plus the left stick folded into the d-pad bits, and (XInput only)
   the analog triggers folded in as digital past XPAD_TRIGGER_THRESHOLD.
   deadzone_raw is in XInput's own -32768..32767 units, already converted by
   the caller from the 0-100% the person actually sees and adjusts (see
   gui.deadzone[]) -- kept that way here so this function has no notion of
   percentages or which player is asking, just a threshold to compare
   against, the same as the fixed constant this replaced. */
static DWORD gamepad_buttons(int device, int deadzone_raw)
{
  DWORD b;
  int x, y;

  if (device >= 0 && device < XPAD_MAX_DEVICES)
  {
    if (!xpad_connected[device]) return 0;

    b = xpad_state[device].Gamepad.wButtons;
    if (xpad_state[device].Gamepad.bLeftTrigger  > XPAD_TRIGGER_THRESHOLD) b |= XPAD_LEFT_TRIGGER;
    if (xpad_state[device].Gamepad.bRightTrigger > XPAD_TRIGGER_THRESHOLD) b |= XPAD_RIGHT_TRIGGER;
    b |= xpad_rs_bits(&xpad_state[device], deadzone_raw);
  }
  else if (is_di_device(device))
  {
    int idx = device - XPAD_MAX_DEVICES;
    if (!joy_connected[idx]) return 0;
    b = di_raw_buttons(&joy_state[idx]);
    b |= di_rs_bits(idx, &joy_state[idx], deadzone_raw);
  }
  else
  {
    return 0;
  }

  if (pad_stick(device, &x, &y))
  {
    DWORD up = is_di_device(device) ? DI_DPAD_UP : XPAD_DPAD_UP;
    DWORD dn = is_di_device(device) ? DI_DPAD_DOWN : XPAD_DPAD_DOWN;
    DWORD lf = is_di_device(device) ? DI_DPAD_LEFT : XPAD_DPAD_LEFT;
    DWORD rt = is_di_device(device) ? DI_DPAD_RIGHT : XPAD_DPAD_RIGHT;

    if (y >  deadzone_raw) b |= up;
    if (y < -deadzone_raw) b |= dn;
    if (x < -deadzone_raw) b |= lf;
    if (x >  deadzone_raw) b |= rt;
  }

  return b;
}

/*
 * Returns the mapping to use for a given emulated device slot.
 *
 * Slots 0 and 1 are the two configurable players. Higher slots only exist
 * with a multitap; those get the default layout on the matching XInput pad so
 * a four-player game works without extra setup.
 */
static int is_pad_dev(int d)
{
  return d == DEVICE_PAD3B || d == DEVICE_PAD6B || d == DEVICE_PAD2B;
}

/*
 * Which emulated device slot holds Player 2's control pad, or -1 if the two
 * ports don't both have a plain pad. On a Genesis, port B's pad lives in slot
 * 4 (slots 1-3 only exist with a multitap on port A); on the 8-bit consoles
 * the second pad is slot 1.
 */
int gui_input_p2_slot(void)
{
  int i;

  if (!is_pad_dev(input.dev[0])) return -1;
  for (i = 2; i < MAX_DEVICES; i++)
    if (i != 4 && input.dev[i] != NO_DEVICE) return -1;

  if (is_pad_dev(input.dev[4]) && input.dev[1] == NO_DEVICE) return 4;
  if (is_pad_dev(input.dev[1]) && input.dev[4] == NO_DEVICE) return 1;
  return -1;
}

static void resolve_map(int slot, t_pad_map *out)
{
  /* Genesis port B: Player 2's own controls. (It used to fall through to the
     "multitap" branch below, which has no keys and no gamepad, so Player 2
     couldn't be controlled at all.) */
  if (slot == 4 && is_pad_dev(input.dev[4]) && input.dev[1] == NO_DEVICE)
  {
    *out = gui.pad[1];
  }
  else if (slot < 2)
  {
    *out = gui.pad[slot];
  }
  else
  {
    *out = gui.pad[0];
    memset(out->key, 0, sizeof(out->key));
    out->turbo = 0;

    if (slot >= 5 && slot <= 7)
    {
      /* TeamPlayer's 2nd/3rd/4th controller (port B, slots 5-7) -- its 1st
         (slot 4) already gets gui.pad[1] above. These continue from XInput
         pad 1, the same "next available slot" scheme port A's own
         multitap positions (slots 2/3, below) already use for pads 2/3 --
         without this they fell all the way through to device = -1 (no
         keys, no gamepad), leaving TeamPlayer positions 2-4 permanently
         unresponsive in a 3-4 player game. */
      out->device = slot - 4;
    }
    else
    {
      out->device = (slot < XPAD_MAX_DEVICES) ? slot : -1;
    }
  }
}

/* Counts frames, for the turbo buttons. */
static unsigned turbo_frames;
static int turbo_master = 1;     /* the gamepad hotkey "Toggle Turbo": 0 = every Turbo button acts as a plain button (for this run) */
int gui_input_toggle_turbo(void) { turbo_master = !turbo_master; return turbo_master; }
static int turbo_half_period(void)      /* frames the button stays down (and then up), for gui.turbo_rate 0-3 */
{
  static const int half[4] = { 4, 3, 2, 1 };   /* 7.5, 10, 15 and 30 presses a second at 60 frames */
  return half[gui.turbo_rate & 3];
}

/* Combo buttons: lines like "Q = A+B" or "Pad:LB = B+C" (gui.combo_text): one key or pad button presses several game buttons
   together. Parsed again whenever the text changes.

   An action can be Held ("Down:H1000" for 1 s, "Down:Hold" until the player lets go of it), and a pause can sit between actions
   ("P500" for half a second): "Pad:Y = Down:H1000+P70+A" holds Down for a second and presses A 70 ms after it went down. The
   actions are laid out on a time line that starts when the trigger is pressed. A combo with no Hold and no pause is the plain
   kind: its buttons are pressed together for as long as the trigger is. */
#define COMBO_TAP_MS 50          /* a pressed action stays down at least this long, even for a quick tap of the trigger */
#define COMBO_AUTO_PAUSE 70      /* between a Held action and the next one, when no pause is given */

typedef struct { unsigned bit; int hold, ms, start; } CSTEP;      /* hold: 0 pressed with the trigger, 1 Held; ms: 0 = until the player lets go; start: ms after the trigger */
typedef struct
{
  int vk; DWORD padbit;
  unsigned game;                 /* every button the combo uses */
  int adv, nsteps, end;          /* adv: Hold or pause used; end: when the time line is over (ms) */
  CSTEP st[12];
  unsigned holdbits, forever;    /* the Held buttons; those of them held until the player lets go */
  int toggle;                    /* every action is held until let go: the trigger switches them on and off */
  int was, alive;                /* the trigger was down last time; the time line is running */
  DWORD t0;                      /* when the trigger was pressed */
  unsigned latched, pending, ph; /* held until let go: down now / waiting for their start / the player's own presses last time */
} COMBO;
static COMBO combos[12];
static int n_combos;

/* Lets go of every combo that is being held. */
void gui_combos_release(void)
{
  int k;
  for (k = 0; k < n_combos; k++) { combos[k].latched = combos[k].pending = 0; combos[k].alive = 0; }
}
static char combo_parsed[512];

static void combos_parse(void)
{
  char buf[512], *line, *next;
  static const struct { const char *n; DWORD m; } pads[] =
  {
    { "A", 0x1000 }, { "B", 0x2000 }, { "X", 0x4000 }, { "Y", 0x8000 }, { "LB", 0x0100 }, { "RB", 0x0200 },
    { "BACK", 0x0020 }, { "START", 0x0010 }, { "LT", XPAD_LEFT_TRIGGER }, { "RT", XPAD_RIGHT_TRIGGER },
    { "UP", 0x0001 }, { "DOWN", 0x0002 }, { "LEFT", 0x0004 }, { "RIGHT", 0x0008 }
  };
  static const struct { const char *n; unsigned m; } gb[] =
  {
    { "A", INPUT_A }, { "B", INPUT_B }, { "C", INPUT_C }, { "X", INPUT_X }, { "Y", INPUT_Y }, { "Z", INPUT_Z },
    { "START", INPUT_START }, { "MODE", INPUT_MODE },
    { "UP", INPUT_UP }, { "DOWN", INPUT_DOWN }, { "LEFT", INPUT_LEFT }, { "RIGHT", INPUT_RIGHT }
  };

  lstrcpynA(combo_parsed, gui.combo_text, sizeof(combo_parsed));
  lstrcpynA(buf, gui.combo_text, sizeof(buf));
  n_combos = 0;
  for (line = buf; line && *line && n_combos < 12; line = next)
  {
    char *eq, *t;
    COMBO c;
    int i, ok = 0, pause = -1, prev_start = 0, prev_hold = 0;

    next = strpbrk(line, "\r\n;");
    if (next) { *next++ = '\0'; while (*next == '\r' || *next == '\n' || *next == ';') next++; }
    eq = strchr(line, '=');
    if (!eq) continue;
    *eq++ = '\0';
    ZeroMemory(&c, sizeof(c));
    for (t = line; *t; t++) *t = (char)toupper((unsigned char)*t);
    for (t = eq; *t; t++) *t = (char)toupper((unsigned char)*t);
    while (*line == ' ') line++;
    { char *e = line + strlen(line); while (e > line && e[-1] == ' ') *--e = '\0'; }
    if (!strncmp(line, "PAD:", 4))
    {
      for (i = 0; i < 14; i++) if (!strcmp(line + 4, pads[i].n)) { c.padbit = pads[i].m; ok = 1; }
    }
    else if (strlen(line) == 1 && isalnum((unsigned char)line[0])) { c.vk = line[0]; ok = 1; }
    else if (line[0] == 'F' && atoi(line + 1) >= 1 && atoi(line + 1) <= 12) { c.vk = VK_F1 + atoi(line + 1) - 1; ok = 1; }
    else if (!strncmp(line, "NUM", 3) && line[3] >= '0' && line[3] <= '9') { c.vk = VK_NUMPAD0 + (line[3] - '0'); ok = 1; }
    else if (!strcmp(line, "SPACE")) { c.vk = VK_SPACE; ok = 1; }
    else if (!strcmp(line, "UP")) { c.vk = VK_UP; ok = 1; }
    else if (!strcmp(line, "DOWN")) { c.vk = VK_DOWN; ok = 1; }
    else if (!strcmp(line, "LEFT")) { c.vk = VK_LEFT; ok = 1; }
    else if (!strcmp(line, "RIGHT")) { c.vk = VK_RIGHT; ok = 1; }
    if (!ok) continue;

    for (t = strtok(eq, "+ "); t; t = strtok(NULL, "+ "))
    {
      char *colon;
      int held = 0, ms = 0, first = (c.nsteps == 0);

      if (t[0] == 'P' && t[1] >= '0' && t[1] <= '9')                 /* "P500": a pause of half a second before the next action */
      {
        pause = atoi(t + 1);
        if (pause > 60000) pause = 60000;
        c.adv = 1;
        continue;
      }
      colon = strchr(t, ':');                                        /* "DOWN:H1000" / "DOWN:HOLD" */
      if (colon)
      {
        *colon = '\0';
        if (colon[1] == 'H')
        {
          held = 1;
          if (colon[2] >= '0' && colon[2] <= '9') ms = atoi(colon + 2);
          if (ms > 60000) ms = 60000;
        }
      }
      for (i = 0; i < 12; i++)
        if (!strcmp(t, gb[i].n) && c.nsteps < 12 && !(c.game & gb[i].m))
        {
          CSTEP *s = &c.st[c.nsteps++];
          s->bit = gb[i].m;
          s->hold = held;
          s->ms = ms;
          s->start = first ? 0 : prev_start + (pause >= 0 ? pause : (prev_hold ? COMBO_AUTO_PAUSE : 0));
          if (!first && pause < 0 && prev_hold) c.adv = 1;
          prev_start = s->start;
          prev_hold = held;
          pause = -1;
          c.game |= gb[i].m;
          if (held) { c.adv = 1; c.holdbits |= gb[i].m; if (!ms) c.forever |= gb[i].m; }
        }
    }
    if (!c.game) continue;
    for (i = 0; i < c.nsteps; i++)
    {
      int e = c.st[i].hold ? (c.st[i].ms ? c.st[i].start + c.st[i].ms : c.st[i].start) : c.st[i].start + COMBO_TAP_MS;
      if (e > c.end) c.end = e;
    }
    c.toggle = (c.holdbits && c.forever == c.game);               /* nothing but "until let go": the trigger is a switch */
    combos[n_combos++] = c;
  }
}

/* Collects one pad's state into a set of INPUT_* bits. */
static int gamepad_button_held_now(int device, int mask, int deadzone_raw_val);
static int padhk_gate(void);

static uint16 read_pad(int slot)
{
  t_pad_map map;
  DWORD buttons;
  uint16 pad = 0;

  resolve_map(slot, &map);
  buttons = gamepad_buttons(map.device, deadzone_raw(slot));

  #define PRESSED(idx) \
    (pad_key_down(map.key[idx]) || (map.button[idx] && (buttons & map.button[idx])))

  if (PRESSED(PAD_A))     pad |= INPUT_A;
  if (PRESSED(PAD_B))     pad |= INPUT_B;
  if (PRESSED(PAD_C))     pad |= INPUT_C;
  if (PRESSED(PAD_START)) pad |= INPUT_START;
  if (PRESSED(PAD_X))     pad |= INPUT_X;
  if (PRESSED(PAD_Y))     pad |= INPUT_Y;
  if (PRESSED(PAD_Z))     pad |= INPUT_Z;
  if (PRESSED(PAD_MODE))  pad |= INPUT_MODE;

  /* Turbo buttons fire on and off while they are held: every 1 - 4 frames (Controls > Turbo Rate). */
  if (map.turbo && turbo_master && ((turbo_frames / (unsigned)turbo_half_period()) & 1))
  {
    if (map.turbo & (1u << PAD_A)) pad &= ~INPUT_A;
    if (map.turbo & (1u << PAD_B)) pad &= ~INPUT_B;
    if (map.turbo & (1u << PAD_C)) pad &= ~INPUT_C;
    if (map.turbo & (1u << PAD_X)) pad &= ~INPUT_X;
    if (map.turbo & (1u << PAD_Y)) pad &= ~INPUT_Y;
    if (map.turbo & (1u << PAD_Z)) pad &= ~INPUT_Z;
  }

  /* Opposite directions cannot be held on real hardware. */
  if (PRESSED(PAD_UP))         pad |= INPUT_UP;
  else if (PRESSED(PAD_DOWN))  pad |= INPUT_DOWN;

  if (PRESSED(PAD_LEFT))       pad |= INPUT_LEFT;
  else if (PRESSED(PAD_RIGHT)) pad |= INPUT_RIGHT;

  #undef PRESSED

  if (slot == 0)
  {
    int k;
    uint16 own;

    if (strcmp(combo_parsed, gui.combo_text)) combos_parse();
    own = pad;                                                 /* what the player presses himself, before the combos add theirs */

    for (k = 0; k < n_combos; k++)
    {
      COMBO *c = &combos[k];
      int down = (c->vk && pad_key_down(c->vk)) ||
                 (c->padbit && map.device >= 0 && map.device < XPAD_MAX_DEVICES && (buttons & c->padbit));   /* the PAD: names are XInput buttons: no meaning on a DirectInput device */

      if (!c->adv)                                                  /* the plain kind: the buttons go down with the trigger */
      {
        if (down) pad |= (uint16)c->game;
        continue;
      }
      else
      {
        DWORD now = GetTickCount();
        unsigned mine = own & c->holdbits;
        int e, i;

        if (down && !c->was)                                        /* the trigger was pressed: the time line starts over */
        {
          if (c->toggle) c->latched = c->latched ? 0 : c->game;     /* a switch: held, or let go again */
          else { c->t0 = now; c->alive = 1; c->pending = c->forever; }
        }
        c->was = down;
        if (c->latched & mine & ~c->ph) c->latched &= ~(mine & ~c->ph);   /* the player presses a held button himself: it is let go (the press goes through) */
        c->ph = mine;

        if (c->alive)
        {
          e = (int)(now - c->t0);
          for (i = 0; i < c->nsteps; i++)
          {
            const CSTEP *s = &c->st[i];
            if (e < s->start) continue;
            if (s->hold && !s->ms) { if (c->pending & s->bit) { c->latched |= s->bit; c->pending &= ~s->bit; } }
            else if (s->hold) { if (e < s->start + s->ms) pad |= (uint16)s->bit; }
            else if (down || e < s->start + COMBO_TAP_MS) pad |= (uint16)s->bit;
          }
          if (!down && e >= c->end) c->alive = 0;
        }
        pad |= (uint16)c->latched;
      }
    }
  }

  return pad;
}

/* Cursor position expressed in emulated pixels, clamped to the frame. */
static int cursor_in_frame(int *px, int *py)
{
  POINT pt;
  RECT dest;
  int sw, sh;
  int dw, dh;

  if (!has_focus) return 0;
  if (!GetCursorPos(&pt)) return 0;
  if (!ScreenToClient(g_hwnd, &pt)) return 0;

  video_get_output_rect(&dest, &sw, &sh);

  dw = dest.right - dest.left;
  dh = dest.bottom - dest.top;
  if (dw < 1 || dh < 1) return 0;

  *px = ((pt.x - dest.left) * sw) / dw;
  *py = ((pt.y - dest.top)  * sh) / dh;

  if (*px < 0) *px = 0; else if (*px >= sw) *px = sw - 1;
  if (*py < 0) *py = 0; else if (*py >= sh) *py = sh - 1;

  return 1;
}

/*
 * Movement since the last frame, in emulated pixels.
 *
 * The Sega Mouse reports relative motion, not a position, so an absolute
 * offset from the centre of the frame would read as a steady drift towards
 * the cursor rather than as the cursor moving.
 */
static int cursor_delta(int slot, int *dx, int *dy)
{
  /* Per slot, not one shared position -- Port A and Port B can each
     independently be set to DEVICE_MOUSE, and a single shared last-position
     pair would have the second mouse's very first read of every frame see
     the first mouse's just-updated position as "last", reporting zero
     movement every single frame regardless of how the physical mouse
     actually moved. */
  static int last_x[MAX_DEVICES], last_y[MAX_DEVICES];
  static int have_last[MAX_DEVICES];
  int x, y;

  *dx = 0;
  *dy = 0;

  if (!cursor_in_frame(&x, &y))
  {
    have_last[slot] = 0;
    return 0;
  }

  if (have_last[slot])
  {
    *dx = x - last_x[slot];
    *dy = y - last_y[slot];
  }

  last_x[slot] = x;
  last_y[slot] = y;
  have_last[slot] = 1;

  return 1;
}

/* The pointer is over the game picture itself (not the menu bar, title bar,
   borders or status bar, and not another window in front of it). */
static int cursor_over_picture(void)
{
  POINT pt;
  RECT dest;

  if (!GetCursorPos(&pt)) return 0;
  if (WindowFromPoint(pt) != g_hwnd) return 0;
  if (!ScreenToClient(g_hwnd, &pt)) return 0;

  video_get_output_rect(&dest, NULL, NULL);
  return PtInRect(&dest, pt) ? 1 : 0;
}

/* 1 = show a crosshair over the picture while a light gun is plugged in, 0 = the normal pointer. */
int gui_input_gun_cursor(void)
{
  int slot;

  if (!emu_running || !has_focus || !cursor_over_picture()) return 0;
  for (slot = 0; slot < MAX_DEVICES; slot++)
    if (input.dev[slot] == DEVICE_LIGHTGUN)
      return config.gun_cursor[0] ? 1 : 0;
  return 0;
}

static int mouse_button(int vk_button)
{
  if (!has_focus) return 0;
  if (!cursor_over_picture()) return 0;     /* a click on the menu or title bar is not a shot */
  return (GetAsyncKeyState(vk_button) & 0x8000) != 0;
}

/****************************************************************************
 * Per-frame update, called by the core
 ****************************************************************************/

int win32_input_update(void)
{
  int slot;
  int sw, sh;
  RECT dest;

  turbo_frames++;
  poll_gamepads();
  video_get_output_rect(&dest, &sw, &sh);

  for (slot = 0; slot < MAX_DEVICES; slot++)
  {
    int x = 0, y = 0;

    input.pad[slot] = 0;

    switch (input.dev[slot])
    {
      case NO_DEVICE:
        break;

      case DEVICE_MOUSE:
      {
        int dx, dy;

        if (!cursor_delta(slot, &dx, &dy))
        {
          /* input.analog[]/[]  isn't reset elsewhere per frame the way
             input.pad[slot] just above is -- without this, the moment the
             cursor leaves the game picture (or the window loses focus),
             the Sega Mouse keeps reporting whatever nonzero delta was last
             seen, every frame, until the cursor re-enters: the in-game
             cursor drifts on its own in the last direction moved instead
             of correctly stopping. */
          input.analog[slot][0] = 0;
          input.analog[slot][1] = 0;
          break;
        }

        /* Speed setting (Settings > Input): scale the movement, carrying the remainder
           so slow settings still move on small steps. */
        {
          static const int pct[5] = { 50, 75, 100, 150, 200 };
          static int rem_x[MAX_DEVICES], rem_y[MAX_DEVICES];
          int p = pct[gui.mouse_sens >= 0 && gui.mouse_sens < 5 ? gui.mouse_sens : 2];

          if (p != 100)
          {
            dx = dx * p + rem_x[slot]; rem_x[slot] = dx % 100; dx /= 100;
            dy = dy * p + rem_y[slot]; rem_y[slot] = dy % 100; dy /= 100;
          }
        }

        /* Y is reported bottom-up by the Sega Mouse. */
        input.analog[slot][0] = dx;
        input.analog[slot][1] = config.invert_mouse ? dy : -dy;

        if (mouse_button(VK_LBUTTON))  input.pad[slot] |= INPUT_MOUSE_LEFT;
        if (mouse_button(VK_RBUTTON))  input.pad[slot] |= INPUT_MOUSE_RIGHT;
        if (mouse_button(VK_MBUTTON))  input.pad[slot] |= INPUT_MOUSE_CENTER;
        {
          /* Whichever player this slot actually belongs to (Port B's mouse
             is Player 2's, per resolve_map()) -- hardcoding gui.pad[0]
             here meant Player 2's own configured Start key did nothing
             whenever their device was a mouse, only Player 1's did. */
          t_pad_map map;
          resolve_map(slot, &map);
          if (pad_key_down(map.key[PAD_START])) input.pad[slot] |= INPUT_START;
        }
        break;
      }

      case DEVICE_LIGHTGUN:
      {
        if (!cursor_in_frame(&x, &y)) break;
        x += (gui.gun_ox - 3) * 4;     /* aim offset (Settings > Input) */
        y += (gui.gun_oy - 3) * 4;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (sw > 0 && x >= sw) x = sw - 1;
        if (sh > 0 && y >= sh) y = sh - 1;
        input.analog[slot][0] = x;
        input.analog[slot][1] = y;
        if (mouse_button(VK_LBUTTON)) input.pad[slot] |= INPUT_A;
        if (mouse_button(VK_RBUTTON)) input.pad[slot] |= INPUT_B;
        if (mouse_button(VK_MBUTTON)) input.pad[slot] |= INPUT_C;
        {
          /* Same reasoning as the mouse case above. */
          t_pad_map map;
          resolve_map(slot, &map);
          if (pad_key_down(map.key[PAD_START])) input.pad[slot] |= INPUT_START;
        }
        break;
      }

      case DEVICE_PADDLE:
      {
        if (cursor_in_frame(&x, &y) && sw > 0)
        {
          input.analog[slot][0] = (x * 256) / sw;
        }
        input.pad[slot] = read_pad(slot) & (INPUT_BUTTON1 | INPUT_BUTTON2 | INPUT_START);
        break;
      }

      case DEVICE_SPORTSPAD:
      {
        if (cursor_in_frame(&x, &y) && sw > 0 && sh > 0)
        {
          input.analog[slot][0] = (x * 256) / sw;
          input.analog[slot][1] = (y * 256) / sh;
        }
        input.pad[slot] = read_pad(slot) & (INPUT_BUTTON1 | INPUT_BUTTON2);
        break;
      }

      case DEVICE_PICO:
      {
        if (cursor_in_frame(&x, &y) && sw > 0 && sh > 0)
        {
          input.analog[0][0] = 0x03c + ((x * (0x17c - 0x03c + 1)) / sw);
          input.analog[0][1] = 0x1fc + ((y * (0x2f7 - 0x1fc + 1)) / sh);
        }
        if (mouse_button(VK_LBUTTON)) input.pad[slot] |= INPUT_PICO_PEN;
        if (mouse_button(VK_RBUTTON)) input.pad[slot] |= INPUT_PICO_RED;
        break;
      }

      case DEVICE_TEREBI:
      {
        if (cursor_in_frame(&x, &y) && sw > 0 && sh > 0)
        {
          input.analog[0][0] = (x * 250) / sw;
          input.analog[0][1] = (y * 250) / sh;
        }
        if (mouse_button(VK_LBUTTON)) input.pad[slot] |= INPUT_B;
        break;
      }

      case DEVICE_GRAPHIC_BOARD:
      {
        /* Unlike PICO/TEREBI (core only ever puts those in slot 0), the
           Graphic Board can be port B's device too, in which case the core
           reads its position from analog[4], not analog[0] -- see
           core/input_hw/graphic_board.c's board.Port. */
        if (cursor_in_frame(&x, &y) && sw > 0 && sh > 0)
        {
          input.analog[slot][0] = (x * 255) / sw;
          input.analog[slot][1] = (y * 255) / sh;
        }
        if (mouse_button(VK_LBUTTON)) input.pad[slot] |= INPUT_GRAPHIC_PEN;
        if (mouse_button(VK_RBUTTON)) input.pad[slot] |= INPUT_GRAPHIC_MENU;
        if (mouse_button(VK_MBUTTON)) input.pad[slot] |= INPUT_GRAPHIC_DO;
        break;
      }

      case DEVICE_XE_1AP:
      {
        uint16 pad = read_pad(slot);
        t_pad_map map;
        DWORD buttons;

        resolve_map(slot, &map);
        buttons = gamepad_buttons(map.device, deadzone_raw(slot));

        if (pad & INPUT_A)     input.pad[slot] |= INPUT_XE_A;
        if (pad & INPUT_B)     input.pad[slot] |= INPUT_XE_B;
        if (pad & INPUT_C)     input.pad[slot] |= INPUT_XE_C;
        if (pad & INPUT_X)     input.pad[slot] |= INPUT_XE_D;
        if (pad & INPUT_START) input.pad[slot] |= INPUT_XE_START;
        if (pad & INPUT_MODE)  input.pad[slot] |= INPUT_XE_SELECT;

        /* Left stick drives the analog axes, d-pad falls back to the extremes. */
        {
          int dz = deadzone_raw(slot);
          int sx, sy;

          if (pad_stick(map.device, &sx, &sy))
          {
            /* >>8, not >>9: sx/sy are -32768..32767, and >>8 is exactly
               what maps that onto 128 +/- 128 = the same full 0..255 range
               the d-pad fallback below reaches at its extremes. The
               previous >>9 only ever reached about 64..191 -- half the
               real stick's travel looked like it wasn't there at all. */
            input.analog[slot][0] = 128 + (deadzone_clamp(sx, dz) >> 8);
            input.analog[slot][1] = 128 - (deadzone_clamp(sy, dz) >> 8);
          }
          else
          {
            input.analog[slot][0] = (pad & INPUT_LEFT) ? 0 : ((pad & INPUT_RIGHT) ? 255 : 128);
            input.analog[slot][1] = (pad & INPUT_UP)   ? 0 : ((pad & INPUT_DOWN)  ? 255 : 128);
          }
        }

        /* No DirectInput equivalent of an XInput shoulder button is assumed
           (an arbitrary joystick has no fixed button layout to guess one
           from), so this second analog channel only ever moves off-centre
           for an XInput pad -- same as before DirectInput support existed. */
        input.analog[slot + 1][0] = (!is_di_device(map.device) && (buttons & XPAD_LEFT_SHOULDER)) ? 0 : 128;
        input.analog[slot + 1][1] = 128;

        if (input.analog[slot][0] < 0)   input.analog[slot][0] = 0;
        if (input.analog[slot][0] > 255) input.analog[slot][0] = 255;
        if (input.analog[slot][1] < 0)   input.analog[slot][1] = 0;
        if (input.analog[slot][1] > 255) input.analog[slot][1] = 255;
        break;
      }

      case DEVICE_ACTIVATOR:
      {
        if (pad_key_down('G')) input.pad[slot] |= INPUT_ACTIVATOR_7L;
        if (pad_key_down('H')) input.pad[slot] |= INPUT_ACTIVATOR_7U;
        if (pad_key_down('J')) input.pad[slot] |= INPUT_ACTIVATOR_8L;
        if (pad_key_down('K')) input.pad[slot] |= INPUT_ACTIVATOR_8U;
        break;
      }

      case DEVICE_SMASH:
      {
        if (pad_key_down(VK_NUMPAD9)) input.pad[slot] |= INPUT_SMASH_UP_RIGHT;
        if (pad_key_down(VK_NUMPAD8)) input.pad[slot] |= INPUT_SMASH_UP;
        if (pad_key_down(VK_NUMPAD7)) input.pad[slot] |= INPUT_SMASH_UP_LEFT;
        if (pad_key_down(VK_NUMPAD6)) input.pad[slot] |= INPUT_SMASH_RIGHT;
        if (pad_key_down(VK_NUMPAD5)) input.pad[slot] |= INPUT_SMASH_CENTER;
        if (pad_key_down(VK_NUMPAD4)) input.pad[slot] |= INPUT_SMASH_LEFT;
        if (pad_key_down(VK_NUMPAD3)) input.pad[slot] |= INPUT_SMASH_DOWN_RIGHT;
        if (pad_key_down(VK_NUMPAD2)) input.pad[slot] |= INPUT_SMASH_DOWN;
        if (pad_key_down(VK_NUMPAD1)) input.pad[slot] |= INPUT_SMASH_DOWN_LEFT;
        break;
      }

      default:
        input.pad[slot] = read_pad(slot);
        break;
    }
  }

  /* Netplay: swap in the agreed inputs for this frame (no-op otherwise). */
  {
    int p2 = netplay_active() ? gui_input_p2_slot() : -1;
    if (p2 >= 0) netplay_apply_input(&input.pad[0], &input.pad[p2]);
  }

  movie_frame();     /* input recording / playback */

  return 1;
}

/****************************************************************************
 * Host shortcuts polled outside the core
 ****************************************************************************/

/* Independent of xpad_state[]/joy_state[]/poll_gamepads() on purpose -- that
   cache is only refreshed once per forward emulated frame (inside
   win32_input_update()), which the rewind path deliberately never calls.
   A hotkey checked against that cache while rewinding would read
   whatever button state happened to be true the instant rewind started,
   forever, regardless of what's actually being pressed right now. This
   does its own fresh read every single call instead (xpad_query()/di_query()
   already back off a device that isn't there rather than genuinely
   re-querying it every call, so this isn't as wasteful as it looks), the
   same way input_capture_gamepad() already does for the "listening"
   dialog. */
/* The pad's buttons (the d-pad bits include the left stick, the trigger and right stick bits are folded in)
   for XInput or DirectInput. Several shortcuts ask in one frame, and a DirectInput read is real work
   (a poll of the device), so the answer is kept for a few milliseconds and shared. */
static DWORD pad_bits_now(int device, int deadzone_raw_val)
{
  if (device >= 0 && device < XPAD_MAX_DEVICES)
  {
    XPAD_STATE st;
    DWORD b;

    if (!xinput_get_state) return 0;
    ZeroMemory(&st, sizeof(st));
    if (!xpad_query(device, &st)) return 0;

    b = st.Gamepad.wButtons;
    if (st.Gamepad.sThumbLY >  deadzone_raw_val) b |= XPAD_DPAD_UP;
    if (st.Gamepad.sThumbLY < -deadzone_raw_val) b |= XPAD_DPAD_DOWN;
    if (st.Gamepad.sThumbLX < -deadzone_raw_val) b |= XPAD_DPAD_LEFT;
    if (st.Gamepad.sThumbLX >  deadzone_raw_val) b |= XPAD_DPAD_RIGHT;
    if (st.Gamepad.bLeftTrigger  > XPAD_TRIGGER_THRESHOLD) b |= XPAD_LEFT_TRIGGER;
    if (st.Gamepad.bRightTrigger > XPAD_TRIGGER_THRESHOLD) b |= XPAD_RIGHT_TRIGGER;
    b |= xpad_rs_bits(&st, deadzone_raw_val);
    return b;
  }

  if (is_di_device(device))
  {
    int idx = device - XPAD_MAX_DEVICES;
    DIJOYSTATE2 st;
    DWORD b;

    ZeroMemory(&st, sizeof(st));
    if (!di_query(idx, &st)) return 0;

    b = di_raw_buttons(&st);
    if (st.lY >  deadzone_raw_val) b |= DI_DPAD_DOWN;
    if (st.lY < -deadzone_raw_val) b |= DI_DPAD_UP;
    if (st.lX < -deadzone_raw_val) b |= DI_DPAD_LEFT;
    if (st.lX >  deadzone_raw_val) b |= DI_DPAD_RIGHT;
    b |= di_rs_bits(idx, &st, deadzone_raw_val);
    return b;
  }

  return 0;
}

static int gamepad_button_held_now(int device, int mask, int deadzone_raw_val)
{
  static struct { int device, dz; DWORD tick, bits; int used; } cache[4];
  static int next;
  DWORD now;
  int i;

  if (!mask) return 0;
  now = GetTickCount();
  for (i = 0; i < 4; i++)
    if (cache[i].used && cache[i].device == device && cache[i].dz == deadzone_raw_val && (DWORD)(now - cache[i].tick) < 4)
      return (cache[i].bits & (DWORD)mask) != 0;

  i = next; next = (next + 1) & 3;
  cache[i].device = device; cache[i].dz = deadzone_raw_val; cache[i].tick = now; cache[i].used = 1;
  cache[i].bits = pad_bits_now(device, deadzone_raw_val);
  return (cache[i].bits & (DWORD)mask) != 0;
}

static int ff_latched, rw_latched;   /* Emulation > Fast Forward / Rewind, ticked */
int gui_input_ff_latched(void) { return ff_latched; }
int gui_input_rewind_latched(void) { return rw_latched; }
void gui_input_toggle_ff(void) { ff_latched = !ff_latched; if (ff_latched) rw_latched = 0; }
void gui_input_toggle_rewind(void) { rw_latched = !rw_latched; if (rw_latched) ff_latched = 0; }
void gui_input_clear_latches(void) { ff_latched = rw_latched = 0; }

int gui_input_fast_forward(void)
{
  if (ff_latched) return 1;
  /* Deliberately absent from the accelerator table so it reaches us here
     regardless of which key it's mapped to. */
  int vk = hotkey_hold_vk(HK_FASTFWD);
  if (vk && key_down(vk)) return 1;
  if (gui.pad[0].device >= 0 &&      /* XInput or DirectInput: padhk_mask() gives the numbers of Player 1's own kind */
      padhk_gate() && gamepad_button_held_now(gui.pad[0].device, padhk_mask(PADHK_FASTFWD), deadzone_raw(0))) return 1;
  return 0;
}

/* Gamepad hotkeys can be set to need a held button (Controls > Gamepad Hotkeys): 1 when they may run now. */
static int padhk_gate(void)
{
  int dev = gui.pad[0].device, mask;

  if (!gui.padhk_hold_on || dev < 0) return 1;
  mask = gui.padhk_hold[dev >= XPAD_MAX_DEVICES ? 1 : 0];
  return mask && gamepad_button_held_now(dev, mask, deadzone_raw(0));
}

int gui_input_rewind(void)
{
  if (rw_latched) return 1;
  int vk = hotkey_hold_vk(HK_REWIND);
  if (vk && key_down(vk)) return 1;
  if (gui.pad[0].device >= 0 &&
      padhk_gate() && gamepad_button_held_now(gui.pad[0].device, padhk_mask(PADHK_REWIND), deadzone_raw(0))) return 1;
  return 0;
}

/* Moving through the in-game pause menu with the pad: the D-pad and the left stick (bit 1 up, 2 down) and the pad's own A or Start
   (4 choose) and B (8 back), each reported once per press. */
int gui_input_menu_pad(void)
{
  static int prev;
  t_pad_map map;
  DWORD b, up, dn;
  int bits = 0, now;

  poll_gamepads();
  resolve_map(0, &map);
  if (map.device < 0) return 0;
  b = gamepad_buttons(map.device, deadzone_raw(0));
  up = is_di_device(map.device) ? DI_DPAD_UP : XPAD_DPAD_UP;
  dn = is_di_device(map.device) ? DI_DPAD_DOWN : XPAD_DPAD_DOWN;
  if (b & up) bits |= 1;
  if (b & dn) bits |= 2;
  if ((map.button[PAD_A] && (b & map.button[PAD_A])) || (map.button[PAD_START] && (b & map.button[PAD_START]))) bits |= 4;
  if (map.button[PAD_B] && (b & map.button[PAD_B])) bits |= 8;
  now = bits & ~prev;
  prev = bits;
  return now;
}

int gui_input_frame_advance(void)
{
  int vk = hotkey_frame_advance_vk();   /* backslash unless changed under Input > Hotkeys */
  int down = vk ? key_down(vk) : 0;

  if (down && !frame_advance_armed)
  {
    frame_advance_armed = 1;
    return 1;
  }
  if (!down) frame_advance_armed = 0;

  return 0;
}

/* Gamepad hotkeys: a press of the pad button set for a shortcut (Controls > Gamepad Hotkeys) on Player 1's
   XInput pad. Returns the menu command that was just pressed, or 0. Fast Forward and Rewind are held
   buttons, read separately. */
int gui_input_pad_hotkey(void)
{
  static int armed[24];
  int dev = gui.pad[0].device, i, result = 0;
  int rs[4];

  if (dev < 0) return 0;                              /* keyboard only: no pad */
  if (!padhk_gate())
  {
    for (i = 0; i < 24; i++) armed[i] = 0;
    return 0;
  }
  if (dev < XPAD_MAX_DEVICES) { rs[0] = XPAD_RS_UP; rs[1] = XPAD_RS_DOWN; rs[2] = XPAD_RS_LEFT; rs[3] = XPAD_RS_RIGHT; }
  else                        { rs[0] = (int)DI_RS_UP; rs[1] = (int)DI_RS_DOWN; rs[2] = (int)DI_RS_LEFT; rs[3] = (int)DI_RS_RIGHT; }

  for (i = 0; i < padhk_count() && i < 24; i++)
  {
    int down, m = padhk_mask(i), k, is_rs = 0;
    if (padhk_is_hold(i)) continue;
    down = m && gamepad_button_held_now(dev, m, 20000);
    for (k = 0; k < 4; k++) if (m == rs[k]) is_rs = 1;
    /* A diagonal push of the right stick would run two shortcuts at once: it counts as neither. */
    if (down && is_rs)
    {
      int held = 0;
      for (k = 0; k < 4; k++) held += gamepad_button_held_now(dev, rs[k], 20000);
      if (held > 1) down = 0;
    }
    if (down && !armed[i]) { armed[i] = 1; if (!result) result = padhk_cmd(i); }
    if (!down) armed[i] = 0;
  }
  return result;
}

static int save_slot_armed[10];
static int load_slot_armed[10];

/* Returns the slot 0-9 whose Shift+digit was just pressed, or -1. */
int gui_input_save_slot_shortcut(void)
{
  int i;

  for (i = 0; i <= 9; i++)
  {
    int down = key_down('0' + i) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);

    if (down && !save_slot_armed[i])
    {
      save_slot_armed[i] = 1;
      load_slot_armed[i] = 1;   /* same physical key is still down -- don't let releasing Shift a moment early re-trigger this as a plain-digit load */
      return i;
    }
    if (!down) save_slot_armed[i] = 0;
  }

  return -1;
}

/* Returns the slot 0-9 whose plain digit was just pressed, or -1.
   Shift held excludes this so the same key press can't register as
   both a save and a load. */
int gui_input_load_slot_shortcut(void)
{
  int i;

  for (i = 0; i <= 9; i++)
  {
    int key_is_down = key_down('0' + i);
    int shift_down = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

    if (key_is_down && !shift_down && !load_slot_armed[i])
    {
      load_slot_armed[i] = 1;
      return i;
    }
    /* Cleared only once the physical key itself comes up -- not merely
       when Shift changes -- so a Shift release that happens a moment
       before the digit key's own release can't be misread as a fresh,
       separate plain-digit press of the same still-held key. */
    if (!key_is_down) load_slot_armed[i] = 0;
  }

  return -1;
}

/****************************************************************************
 * Capture helpers for the configuration dialog
 *
 * These read the raw key state directly instead of going through key_down():
 * while the dialog is up the main window has lost focus, which is exactly
 * when the emulated pad should go quiet but capture should still work.
 ****************************************************************************/

int input_capture_key(void)
{
  int vk;

  for (vk = 1; vk < 256; vk++)
  {
    switch (vk)
    {
      /* Mouse buttons belong to the pointer devices, Esc cancels. */
      case VK_LBUTTON: case VK_RBUTTON: case VK_MBUTTON:
      case VK_XBUTTON1: case VK_XBUTTON2:
      case VK_ESCAPE:
        continue;
      default:
        break;
    }

    if (GetAsyncKeyState(vk) & 0x8000) return vk;
  }

  return 0;
}

/* Everything held down on a pad right now (stick and triggers folded into the
   same bits the assignments use), for showing live presses. */
int input_pad_button_down(int device, int mask)
{
  if (!mask) return 0;

  if (device >= 0 && device < XPAD_MAX_DEVICES)
  {
    XPAD_STATE st;
    DWORD b;

    if (!xinput_get_state) return 0;
    ZeroMemory(&st, sizeof(st));
    if (!xpad_query(device, &st)) return 0;

    b = st.Gamepad.wButtons;
    if (st.Gamepad.sThumbLY >  XPAD_DEADZONE) b |= XPAD_DPAD_UP;
    if (st.Gamepad.sThumbLY < -XPAD_DEADZONE) b |= XPAD_DPAD_DOWN;
    if (st.Gamepad.sThumbLX < -XPAD_DEADZONE) b |= XPAD_DPAD_LEFT;
    if (st.Gamepad.sThumbLX >  XPAD_DEADZONE) b |= XPAD_DPAD_RIGHT;
    if (st.Gamepad.bLeftTrigger  > XPAD_TRIGGER_THRESHOLD) b |= XPAD_LEFT_TRIGGER;
    if (st.Gamepad.bRightTrigger > XPAD_TRIGGER_THRESHOLD) b |= XPAD_RIGHT_TRIGGER;
    b |= xpad_rs_bits(&st, XPAD_DEADZONE);
    return (b & (DWORD)mask) != 0;
  }

  if (is_di_device(device))
  {
    DIJOYSTATE2 st;
    DWORD b;

    ZeroMemory(&st, sizeof(st));
    if (!di_query(device - XPAD_MAX_DEVICES, &st)) return 0;

    b = di_raw_buttons(&st);
    if (st.lY >  XPAD_DEADZONE) b |= DI_DPAD_DOWN;
    if (st.lY < -XPAD_DEADZONE) b |= DI_DPAD_UP;
    if (st.lX < -XPAD_DEADZONE) b |= DI_DPAD_LEFT;
    if (st.lX >  XPAD_DEADZONE) b |= DI_DPAD_RIGHT;
    b |= di_rs_bits(device - XPAD_MAX_DEVICES, &st, XPAD_DEADZONE);
    return (b & (DWORD)mask) != 0;
  }

  return 0;
}

int input_capture_gamepad(int device)
{
  if (device >= 0 && device < XPAD_MAX_DEVICES)
  {
    static const int masks[] =
    {
      XPAD_DPAD_UP, XPAD_DPAD_DOWN, XPAD_DPAD_LEFT, XPAD_DPAD_RIGHT,
      XPAD_START, XPAD_BACK, XPAD_LEFT_SHOULDER, XPAD_RIGHT_SHOULDER,
      0x0040, 0x0080,                              /* the clicks of the left and right sticks (LSB, RSB) */
      XPAD_A, XPAD_B, XPAD_X, XPAD_Y, XPAD_LEFT_TRIGGER, XPAD_RIGHT_TRIGGER,
      XPAD_RS_UP, XPAD_RS_DOWN, XPAD_RS_LEFT, XPAD_RS_RIGHT
    };

    XPAD_STATE st;
    DWORD b;
    int i;

    if (!xinput_get_state) return 0;

    ZeroMemory(&st, sizeof(st));
    if (!xpad_query(device, &st)) return 0;

    b = st.Gamepad.wButtons;
    if (st.Gamepad.sThumbLY >  XPAD_DEADZONE) b |= XPAD_DPAD_UP;
    if (st.Gamepad.sThumbLY < -XPAD_DEADZONE) b |= XPAD_DPAD_DOWN;
    if (st.Gamepad.sThumbLX < -XPAD_DEADZONE) b |= XPAD_DPAD_LEFT;
    if (st.Gamepad.sThumbLX >  XPAD_DEADZONE) b |= XPAD_DPAD_RIGHT;
    if (st.Gamepad.bLeftTrigger  > XPAD_TRIGGER_THRESHOLD) b |= XPAD_LEFT_TRIGGER;
    if (st.Gamepad.bRightTrigger > XPAD_TRIGGER_THRESHOLD) b |= XPAD_RIGHT_TRIGGER;
    b |= xpad_rs_bits(&st, XPAD_DEADZONE);

    for (i = 0; i < (int)(sizeof(masks) / sizeof(masks[0])); i++)
    {
      if (b & masks[i]) return masks[i];
    }

    return 0;
  }

  if (is_di_device(device))
  {
    int idx = device - XPAD_MAX_DEVICES;
    DIJOYSTATE2 st;
    DWORD b;
    int i;

    ZeroMemory(&st, sizeof(st));
    if (!di_query(idx, &st)) return 0;

    b = di_raw_buttons(&st);
    if (st.lY >  XPAD_DEADZONE) b |= DI_DPAD_DOWN;
    if (st.lY < -XPAD_DEADZONE) b |= DI_DPAD_UP;
    if (st.lX < -XPAD_DEADZONE) b |= DI_DPAD_LEFT;
    if (st.lX >  XPAD_DEADZONE) b |= DI_DPAD_RIGHT;
    b |= di_rs_bits(idx, &st, XPAD_DEADZONE);

    if (b & DI_DPAD_UP)    return DI_DPAD_UP;
    if (b & DI_DPAD_DOWN)  return DI_DPAD_DOWN;
    if (b & DI_DPAD_LEFT)  return DI_DPAD_LEFT;
    if (b & DI_DPAD_RIGHT) return DI_DPAD_RIGHT;
    for (i = 0; i < DI_MAX_BUTTONS; i++)
    {
      if (b & DI_BTN(i)) return (int)DI_BTN(i);
    }
    if (b & DI_RS_UP)    return (int)DI_RS_UP;
    if (b & DI_RS_DOWN)  return (int)DI_RS_DOWN;
    if (b & DI_RS_LEFT)  return (int)DI_RS_LEFT;
    if (b & DI_RS_RIGHT) return (int)DI_RS_RIGHT;

    return 0;
  }

  return 0;
}

int input_any_input_down(int device)
{
  if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) return 1;
  if (input_capture_key()) return 1;
  if (input_capture_gamepad(device)) return 1;
  return 0;
}

/****************************************************************************
 * Names for the configuration dialog
 ****************************************************************************/

const char *input_key_name(int vk)
{
  static char name[64];
  UINT scan;
  LONG lparam;

  if (!vk) return "(Unassigned)";

  scan = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
  if (!scan) { wsprintfA(name, "Key %d", vk); return name; }

  lparam = (LONG)(scan << 16);

  switch (vk)
  {
    /* Keys on the grey block report the same scan code as the numpad. */
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
    case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
      lparam |= (1 << 24);
      break;
    default:
      break;
  }

  if (GetKeyNameTextA(lparam, name, sizeof(name)) > 0) return name;

  wsprintfA(name, "Key %d", vk);
  return name;
}

/* mask is only meaningful alongside the device it was captured against (see
   the file banner comment) -- the two backends use overlapping numeric bit
   values for entirely different things, so device picks which of the two
   naming tables below applies. */
const char *input_pad_button_name(int device, int mask)
{
  static const struct { int mask; const char *name; } xinput_names[] =
  {
    { XPAD_DPAD_UP,        "D-Pad Up"    },
    { XPAD_DPAD_DOWN,      "D-Pad Down"  },
    { XPAD_DPAD_LEFT,      "D-Pad Left"  },
    { XPAD_DPAD_RIGHT,     "D-Pad Right" },
    { XPAD_START,          "Start"  },
    { XPAD_BACK,           "Back"   },
    { 0x0040,              "LSB" },
    { 0x0080,              "RSB" },
    { XPAD_LEFT_SHOULDER,  "LB"   },
    { XPAD_RIGHT_SHOULDER, "RB"   },
    { XPAD_A,              "A"    },
    { XPAD_B,              "B"    },
    { XPAD_X,              "X"    },
    { XPAD_Y,              "Y"    },
    { XPAD_LEFT_TRIGGER,   "LT"   },
    { XPAD_RIGHT_TRIGGER,  "RT"   },
    { XPAD_RS_UP,          "RS Up"    },
    { XPAD_RS_DOWN,        "RS Down"  },
    { XPAD_RS_LEFT,        "RS Left"  },
    { XPAD_RS_RIGHT,       "RS Right" },
  };
  static char name[32];
  int i;

  if (!mask) return "(Unassigned)";

  if (is_di_device(device))
  {
    if (mask == (int)DI_DPAD_UP)    return "D-Pad/Stick Up";
    if (mask == (int)DI_DPAD_DOWN)  return "D-Pad/Stick Down";
    if (mask == (int)DI_DPAD_LEFT)  return "D-Pad/Stick Left";
    if (mask == (int)DI_DPAD_RIGHT) return "D-Pad/Stick Right";
    if (mask == (int)DI_RS_UP)      return "RS Up";
    if (mask == (int)DI_RS_DOWN)    return "RS Down";
    if (mask == (int)DI_RS_LEFT)    return "RS Left";
    if (mask == (int)DI_RS_RIGHT)   return "RS Right";

    for (i = 0; i < DI_MAX_BUTTONS; i++)
    {
      if (mask == (int)DI_BTN(i))
      {
        wsprintfA(name, "Button %d", i + 1);
        return name;
      }
    }

    wsprintfA(name, "Pad 0x%06X", mask);
    return name;
  }

  for (i = 0; i < (int)(sizeof(xinput_names) / sizeof(xinput_names[0])); i++)
  {
    if (xinput_names[i].mask == mask) return xinput_names[i].name;
  }

  /* Should not happen for anything this app itself ever assigns, but
     falls back to the raw value rather than showing nothing if it does. */
  wsprintfA(name, "Pad 0x%05X", mask);
  return name;
}

const char *input_button_label(int index)
{
  static const char *labels[PAD_KEYS] =
  {
    "Up", "Down", "Left", "Right",
    "A", "B", "C",
    "X", "Y", "Z", "Start", "Mode"
  };

  if (index < 0 || index >= PAD_KEYS) return "?";
  return labels[index];
}
