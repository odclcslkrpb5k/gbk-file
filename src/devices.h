/* Devices: drives, partitions and phones that can be opened, mounted, unmounted or ejected,
 * as GIO's volume monitor sees them (udisks2 through gvfs where installed; otherwise the
 * system's mount table), led by the root file system. Other system mounts (/boot, ...) are
 * not devices. One per window.
 *
 * gbk_devices_menu() is the contents of the "Devices" submenu: "File System" (/) first, as
 * in Thunar, then one submenu per device, holding Open, Unmount and Eject (or Safely Remove)
 * as they apply. They activate
 * win.device-open / win.device-unmount / win.device-eject with the device's id (a string).
 * The model follows plugging and unplugging while it is shown. Ids are never reused, so an
 * item left over from an older list names no device rather than the wrong one.
 *
 * Signals:
 *   "pre-unmount" (const char *root)  a mount is about to go away: whatever shows a folder
 *       under `root` must let go of it now. Emitted before our own unmounts and ejects, and
 *       when another program unmounts through gvfs. */
#pragma once

#define GBK_TYPE_DEVICES (gbk_devices_get_type ())
G_DECLARE_FINAL_TYPE (GbkDevices, gbk_devices, GBK, DEVICES, GObject)

GbkDevices *gbk_devices_new (void);
/* Starts following the volume monitor. The first one in a process blocks for a while
 * (~20 ms on D-Bus with gvfs), so callers start it after the window has something to show.
 * Calling it again does nothing. */
void gbk_devices_start (GbkDevices *d);
bool gbk_devices_started (const GbkDevices *d);
GMenuModel *gbk_devices_menu (GbkDevices *d);

/* Finished: `path` is the opened device's folder (open only, else nullptr); `error` is
 * nullptr on success. G_IO_ERROR_FAILED_HANDLED means the user has already been told (or
 * cancelled a password prompt): show nothing. Runs only while `owner` is alive. */
typedef void (*GbkDeviceDone) (GObject *owner, const char *path, const GError *error);

/* Each returns false, doing nothing, if `id` names no current device (or the device can't
 * do that). `op` (may be nullptr) asks for passwords and shows "device is busy" details.
 * Open mounts the device first if it isn't mounted. Unmount and eject first emit
 * "pre-unmount" for every mount that will go, then wait (up to GBK_DEVICES_RELEASE_MS) for
 * our own folder readers there to finish, so we never make the device busy ourselves. */
bool gbk_devices_open (GbkDevices *d, const char *id, GMountOperation *op, GObject *owner,
                       GbkDeviceDone done);
bool gbk_devices_unmount (GbkDevices *d, const char *id, GMountOperation *op, GObject *owner,
                          GbkDeviceDone done);
bool gbk_devices_eject (GbkDevices *d, const char *id, GMountOperation *op, GObject *owner,
                        GbkDeviceDone done);

#define GBK_DEVICES_RELEASE_MS 2000

/* Tests: use this monitor instead of the system's (nullptr: the system's again). */
void gbk_devices_use_monitor_for_test (GVolumeMonitor *monitor);
