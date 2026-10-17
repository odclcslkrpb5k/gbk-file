#include "devices.h"
#include "lifetime.h"
#include "scanner.h"

#define RELEASE_POLL_MS 10

typedef enum
{
  EJECT_NONE,
  EJECT_MOUNT,
  EJECT_VOLUME,
  EJECT_DRIVE,
  STOP_DRIVE,       /* "Safely Remove": power off a drive that can't eject (USB disks) */
} EjectKind;

/* One entry in the menu. Built from the monitor on every change, never edited. */
typedef struct
{
  char *id;
  char *name;
  GDrive *drive;      /* nullable refs */
  GVolume *volume;
  GMount *mount;
  char *root;         /* the mount's folder; nullptr if not mounted or not a local folder */
  EjectKind eject;
} Device;

struct _GbkDevices
{
  GObject parent_instance;
  GVolumeMonitor *monitor;   /* nullptr until started */
  GMenu *menu;
  GPtrArray *devices;        /* Device*, the list the menu shows */
  guint64 next_id;
  guint rebuild;             /* idle source: coalesces bursts of monitor signals */
};

G_DEFINE_FINAL_TYPE (GbkDevices, gbk_devices, G_TYPE_OBJECT)

enum { SIGNAL_PRE_UNMOUNT, N_SIGNALS };
static guint signals[N_SIGNALS];

static GVolumeMonitor *test_monitor;

void
gbk_devices_use_monitor_for_test (GVolumeMonitor *monitor)
{
  g_set_object (&test_monitor, monitor);
}

static void
device_free (Device *dev)
{
  g_free (dev->id);
  g_free (dev->name);
  g_clear_object (&dev->drive);
  g_clear_object (&dev->volume);
  g_clear_object (&dev->mount);
  g_free (dev->root);
  g_free (dev);
}

static char *
local_root (GMount *mount)
{
  g_autoptr (GFile) root = g_mount_get_root (mount);
  return g_file_get_path (root);   /* nullptr unless it's a local folder */
}

static EjectKind
eject_kind (GDrive *drive, GVolume *volume, GMount *mount)
{
  if (mount != nullptr && g_mount_can_eject (mount))
    return EJECT_MOUNT;
  if (volume != nullptr && g_volume_can_eject (volume))
    return EJECT_VOLUME;
  if (drive != nullptr && g_drive_can_eject (drive))
    return EJECT_DRIVE;
  if (drive != nullptr && g_drive_can_stop (drive)
      && g_drive_get_start_stop_type (drive) == G_DRIVE_START_STOP_TYPE_SHUTDOWN)
    return STOP_DRIVE;
  return EJECT_NONE;
}

static bool
can_open (const Device *dev)
{
  return dev->root != nullptr
         || (dev->mount == nullptr && dev->volume != nullptr && g_volume_can_mount (dev->volume));
}

static bool
can_unmount (const Device *dev)
{
  return dev->mount != nullptr && g_mount_can_unmount (dev->mount);
}

/* Adds the device for `volume` and/or `mount` (at least one) if anything can be done
 * with it. Takes nothing; refs what it keeps. */
static void
add_device (GbkDevices *d, GPtrArray *out, GDrive *drive, GVolume *volume, GMount *mount)
{
  Device *dev = g_new0 (Device, 1);
  dev->drive = drive != nullptr ? g_object_ref (drive) : nullptr;
  dev->volume = volume != nullptr ? g_object_ref (volume) : nullptr;
  dev->mount = mount != nullptr ? g_object_ref (mount) : nullptr;
  dev->name = mount != nullptr ? g_mount_get_name (mount) : g_volume_get_name (volume);
  if (dev->name == nullptr || dev->name[0] == '\0')
    {
      g_free (dev->name);
      dev->name = g_strdup ("Unnamed device");
    }
  dev->root = mount != nullptr ? local_root (mount) : nullptr;
  dev->eject = eject_kind (drive, volume, mount);
  if (!can_open (dev) && !can_unmount (dev) && dev->eject == EJECT_NONE)
    {
      device_free (dev);
      return;
    }
  dev->id = g_strdup_printf ("%" G_GUINT64_FORMAT, d->next_id++);
  g_ptr_array_add (out, dev);
}

static void
add_volume (GbkDevices *d, GPtrArray *out, GDrive *drive, GVolume *volume)
{
  g_autoptr (GMount) mount = g_volume_get_mount (volume);
  add_device (d, out, drive, volume, mount);
}

/* "File System" (/) first, always (it needs no monitor). Then drives in the monitor's order
 * with their volumes, volumes without a drive, and mounts without a volume (fstab "user"
 * mounts, network folders): what Thunar lists. A drive without volumes (an empty card
 * reader, a disk with only system partitions) is nothing to open. */
static GPtrArray *
collect (GbkDevices *d)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) device_free);
  Device *root = g_new0 (Device, 1);
  root->id = g_strdup_printf ("%" G_GUINT64_FORMAT, d->next_id++);
  root->name = g_strdup ("File System");
  root->root = g_strdup ("/");
  g_ptr_array_add (out, root);
  if (d->monitor == nullptr)
    return out;

  GList *drives = g_volume_monitor_get_connected_drives (d->monitor);
  for (GList *l = drives; l != nullptr; l = l->next)
    {
      GList *volumes = g_drive_get_volumes (l->data);
      for (GList *v = volumes; v != nullptr; v = v->next)
        add_volume (d, out, l->data, v->data);
      g_list_free_full (volumes, g_object_unref);
    }
  g_list_free_full (drives, g_object_unref);

  GList *volumes = g_volume_monitor_get_volumes (d->monitor);
  for (GList *l = volumes; l != nullptr; l = l->next)
    {
      g_autoptr (GDrive) drive = g_volume_get_drive (l->data);
      if (drive == nullptr)
        add_volume (d, out, nullptr, l->data);
    }
  g_list_free_full (volumes, g_object_unref);

  GList *mounts = g_volume_monitor_get_mounts (d->monitor);
  for (GList *l = mounts; l != nullptr; l = l->next)
    {
      g_autoptr (GVolume) volume = g_mount_get_volume (l->data);
      if (volume == nullptr && !g_mount_is_shadowed (l->data))
        add_device (d, out, nullptr, nullptr, l->data);
    }
  g_list_free_full (mounts, g_object_unref);
  return out;
}

/* Menu labels take "_" as a mnemonic marker: a device named "MY_USB" must keep it. */
static char *
menu_label (const char *name, const char *suffix)
{
  GString *s = g_string_new (nullptr);
  for (const char *p = name; *p != '\0'; p++)
    {
      if (*p == '_')
        g_string_append_c (s, '_');
      g_string_append_c (s, *p);
    }
  g_string_append (s, suffix);
  return g_string_free (s, false);
}

static void
append_action (GMenu *menu, const char *label, const char *action, const char *id)
{
  g_autoptr (GMenuItem) item = g_menu_item_new (label, nullptr);
  g_menu_item_set_action_and_target_value (item, action, g_variant_new_string (id));
  g_menu_append_item (menu, item);
}

static void
rebuild_menu (GbkDevices *d)
{
  g_menu_remove_all (d->menu);
  for (guint i = 0; i < d->devices->len; i++)
    {
      const Device *dev = d->devices->pdata[i];
      g_autoptr (GMenu) sub = g_menu_new ();
      if (can_open (dev))
        append_action (sub, "Open", "win.device-open", dev->id);
      if (can_unmount (dev))
        append_action (sub, "Unmount", "win.device-unmount", dev->id);
      if (dev->eject != EJECT_NONE)
        append_action (sub, dev->eject == STOP_DRIVE ? "Safely Remove" : "Eject",
                       "win.device-eject", dev->id);
      bool mounted = dev->mount != nullptr || dev->root != nullptr;
      g_autofree char *label = menu_label (dev->name, mounted ? "" : " (not mounted)");
      g_menu_append_submenu (d->menu, label, G_MENU_MODEL (sub));
    }
}

static void
rebuild (GbkDevices *d)
{
  g_ptr_array_unref (d->devices);
  d->devices = collect (d);
  rebuild_menu (d);
}

static bool
rebuild_idle (void *data)
{
  GbkDevices *d = data;
  d->rebuild = 0;
  rebuild (d);
  return false;
}

static void
on_monitor_changed (GVolumeMonitor *m G_GNUC_UNUSED, GObject *what G_GNUC_UNUSED, GbkDevices *d)
{
  if (d->rebuild == 0)
    d->rebuild = gbk_idle_add (G_OBJECT (d), rebuild_idle, d);
}

static void
emit_pre_unmount (GbkDevices *d, GMount *mount)
{
  g_autofree char *root = local_root (mount);
  if (root != nullptr)
    g_signal_emit (d, signals[SIGNAL_PRE_UNMOUNT], 0, root);
}

static void
on_mount_pre_unmount (GVolumeMonitor *m G_GNUC_UNUSED, GMount *mount, GbkDevices *d)
{
  emit_pre_unmount (d, mount);
}

void
gbk_devices_start (GbkDevices *d)
{
  if (d->monitor != nullptr)
    return;
  d->monitor = test_monitor != nullptr ? g_object_ref (test_monitor) : g_volume_monitor_get ();
  static const char *const changes[] = {
    "drive-connected", "drive-disconnected", "drive-changed",
    "volume-added", "volume-removed", "volume-changed",
    "mount-added", "mount-removed", "mount-changed",
  };
  for (size_t i = 0; i < G_N_ELEMENTS (changes); i++)
    gbk_connect (G_OBJECT (d), d->monitor, changes[i], G_CALLBACK (on_monitor_changed), d);
  gbk_connect (G_OBJECT (d), d->monitor, "mount-pre-unmount",
               G_CALLBACK (on_mount_pre_unmount), d);
  rebuild (d);
}

bool
gbk_devices_started (const GbkDevices *d)
{
  return d->monitor != nullptr;
}

GMenuModel *
gbk_devices_menu (GbkDevices *d)
{
  return G_MENU_MODEL (d->menu);
}

static const Device *
find (GbkDevices *d, const char *id)
{
  for (guint i = 0; i < d->devices->len; i++)
    {
      const Device *dev = d->devices->pdata[i];
      if (g_str_equal (dev->id, id))
        return dev;
    }
  return nullptr;
}

/* ---- operations ---- */

typedef enum { OP_MOUNT, OP_UNMOUNT, OP_EJECT } OpKind;

/* One operation in flight. Holds the device's objects (the list may be rebuilt meanwhile)
 * and the GbkDevices; the owner only weakly. */
typedef struct
{
  GbkDevices *d;
  GWeakRef owner;
  GbkDeviceDone done;
  GMountOperation *op;   /* nullable */
  OpKind kind;
  EjectKind eject;
  GDrive *drive;         /* nullable refs */
  GVolume *volume;
  GMount *mount;
  GPtrArray *roots;      /* char*: folders of the mounts that will go */
  int64_t deadline;      /* monotonic µs: stop waiting for our readers to let go */
} Pending;

static Pending *
pending_new (GbkDevices *d, const Device *dev, OpKind kind, GMountOperation *op,
             GObject *owner, GbkDeviceDone done)
{
  Pending *p = g_new0 (Pending, 1);
  p->d = g_object_ref (d);
  g_weak_ref_init (&p->owner, owner);
  p->done = done;
  p->op = op != nullptr ? g_object_ref (op) : nullptr;
  p->kind = kind;
  p->eject = dev->eject;
  p->drive = dev->drive != nullptr ? g_object_ref (dev->drive) : nullptr;
  p->volume = dev->volume != nullptr ? g_object_ref (dev->volume) : nullptr;
  p->mount = dev->mount != nullptr ? g_object_ref (dev->mount) : nullptr;
  p->roots = g_ptr_array_new_with_free_func (g_free);
  return p;
}

static void
pending_free (Pending *p)
{
  g_object_unref (p->d);
  g_weak_ref_clear (&p->owner);
  g_clear_object (&p->op);
  g_clear_object (&p->drive);
  g_clear_object (&p->volume);
  g_clear_object (&p->mount);
  g_ptr_array_unref (p->roots);
  g_free (p);
}

static void
finish (Pending *p, const char *path, const GError *error)
{
  g_autoptr (GObject) owner = g_weak_ref_get (&p->owner);
  if (owner != nullptr)
    p->done (owner, path, error);
  pending_free (p);
}

static void
on_mounted (GObject *source G_GNUC_UNUSED, GAsyncResult *res, void *data)
{
  Pending *p = data;
  g_autoptr (GError) error = nullptr;
  g_autofree char *root = nullptr;
  if (g_volume_mount_finish (p->volume, res, &error))
    {
      g_autoptr (GMount) mount = g_volume_get_mount (p->volume);
      root = mount != nullptr ? local_root (mount) : nullptr;
      if (root == nullptr)
        g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "The device was mounted, but not as a local folder.");
    }
  finish (p, root, error);
}

static void
on_gone (GObject *source G_GNUC_UNUSED, GAsyncResult *res, void *data)
{
  Pending *p = data;
  g_autoptr (GError) error = nullptr;
  switch (p->kind == OP_UNMOUNT ? EJECT_NONE : p->eject)
    {
    case EJECT_NONE: g_mount_unmount_with_operation_finish (p->mount, res, &error); break;
    case EJECT_MOUNT: g_mount_eject_with_operation_finish (p->mount, res, &error); break;
    case EJECT_VOLUME: g_volume_eject_with_operation_finish (p->volume, res, &error); break;
    case EJECT_DRIVE: g_drive_eject_with_operation_finish (p->drive, res, &error); break;
    case STOP_DRIVE: g_drive_stop_finish (p->drive, res, &error); break;
    default: gbk_fatal ("bad eject kind %d", (int) p->eject);
    }
  finish (p, nullptr, error);
}

static void
begin_removal (Pending *p)
{
  GMountUnmountFlags flags = G_MOUNT_UNMOUNT_NONE;
  switch (p->kind == OP_UNMOUNT ? EJECT_NONE : p->eject)
    {
    case EJECT_NONE:
      g_mount_unmount_with_operation (p->mount, flags, p->op, nullptr, on_gone, p);
      break;
    case EJECT_MOUNT:
      g_mount_eject_with_operation (p->mount, flags, p->op, nullptr, on_gone, p);
      break;
    case EJECT_VOLUME:
      g_volume_eject_with_operation (p->volume, flags, p->op, nullptr, on_gone, p);
      break;
    case EJECT_DRIVE:
      g_drive_eject_with_operation (p->drive, flags, p->op, nullptr, on_gone, p);
      break;
    case STOP_DRIVE:
      g_drive_stop (p->drive, flags, p->op, nullptr, on_gone, p);
      break;
    default:
      gbk_fatal ("bad eject kind %d", (int) p->eject);
    }
}

static bool
readers_busy (const Pending *p)
{
  for (guint i = 0; i < p->roots->len; i++)
    if (gbk_scanner_busy_under (p->roots->pdata[i]))
      return true;
  return false;
}

/* Our folder readers stop on their own a moment after they're told to. Unmounting before
 * they have would fail as busy, because of us. A reader stuck on a hung device never lets
 * go: after the deadline, try anyway and let the system report it. */
static bool
wait_for_readers (void *data)
{
  Pending *p = data;
  if (readers_busy (p) && g_get_monotonic_time () < p->deadline)
    return true;
  begin_removal (p);
  return false;
}

static void
add_root (Pending *p, GMount *mount)
{
  char *root = local_root (mount);
  if (root == nullptr)
    return;
  for (guint i = 0; i < p->roots->len; i++)
    if (g_str_equal (p->roots->pdata[i], root))
      {
        g_free (root);
        return;
      }
  g_ptr_array_add (p->roots, root);
}

static void
start_removal (Pending *p)
{
  /* Ejecting takes every mount on the drive with it. */
  g_autoptr (GDrive) drive = nullptr;
  if (p->kind == OP_EJECT)
    {
      if (p->drive != nullptr)
        drive = g_object_ref (p->drive);
      else if (p->volume != nullptr)
        drive = g_volume_get_drive (p->volume);
      else if (p->mount != nullptr)
        drive = g_mount_get_drive (p->mount);
    }
  if (p->mount != nullptr)
    add_root (p, p->mount);
  if (drive != nullptr)
    {
      GList *volumes = g_drive_get_volumes (drive);
      for (GList *l = volumes; l != nullptr; l = l->next)
        {
          g_autoptr (GMount) mount = g_volume_get_mount (l->data);
          if (mount != nullptr)
            add_root (p, mount);
        }
      g_list_free_full (volumes, g_object_unref);
    }

  for (guint i = 0; i < p->roots->len; i++)
    g_signal_emit (p->d, signals[SIGNAL_PRE_UNMOUNT], 0, p->roots->pdata[i]);
  p->deadline = g_get_monotonic_time () + GBK_DEVICES_RELEASE_MS * G_TIME_SPAN_MILLISECOND;
  if (wait_for_readers (p))
    gbk_timeout_add (G_OBJECT (p->d), RELEASE_POLL_MS, wait_for_readers, p);
}

bool
gbk_devices_open (GbkDevices *d, const char *id, GMountOperation *op, GObject *owner,
                  GbkDeviceDone done)
{
  const Device *dev = find (d, id);
  if (dev == nullptr || !can_open (dev))
    return false;
  if (dev->root != nullptr)
    {
      g_autofree char *root = g_strdup (dev->root);   /* `done` may rebuild the list */
      done (owner, root, nullptr);
      return true;
    }
  Pending *p = pending_new (d, dev, OP_MOUNT, op, owner, done);
  g_volume_mount (p->volume, G_MOUNT_MOUNT_NONE, p->op, nullptr, on_mounted, p);
  return true;
}

bool
gbk_devices_unmount (GbkDevices *d, const char *id, GMountOperation *op, GObject *owner,
                     GbkDeviceDone done)
{
  const Device *dev = find (d, id);
  if (dev == nullptr || !can_unmount (dev))
    return false;
  start_removal (pending_new (d, dev, OP_UNMOUNT, op, owner, done));
  return true;
}

bool
gbk_devices_eject (GbkDevices *d, const char *id, GMountOperation *op, GObject *owner,
                   GbkDeviceDone done)
{
  const Device *dev = find (d, id);
  if (dev == nullptr || dev->eject == EJECT_NONE)
    return false;
  start_removal (pending_new (d, dev, OP_EJECT, op, owner, done));
  return true;
}

/* ---- GObject ---- */

static void
gbk_devices_finalize (GObject *o)
{
  GbkDevices *d = GBK_DEVICES (o);
  g_clear_object (&d->monitor);
  g_object_unref (d->menu);
  g_ptr_array_unref (d->devices);
  G_OBJECT_CLASS (gbk_devices_parent_class)->finalize (o);
}

static void
gbk_devices_class_init (GbkDevicesClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = gbk_devices_finalize;
  signals[SIGNAL_PRE_UNMOUNT] = g_signal_new ("pre-unmount", G_TYPE_FROM_CLASS (klass),
                                              G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr,
                                              G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gbk_devices_init (GbkDevices *d)
{
  d->menu = g_menu_new ();
  d->devices = collect (d);   /* just "File System" until started */
  rebuild_menu (d);
}

GbkDevices *
gbk_devices_new (void)
{
  return g_object_new (GBK_TYPE_DEVICES, nullptr);
}
