// SPDX-License-Identifier: GPL-2.0+
/*
 * evo_raw - raw USB control transfer access for Audient EVO series
 *
 * This module binds to Audient EVO USB devices and exposes per-device misc
 * devices (/dev/evo4, /dev/evo8). A single ioctl (EVO_CTRL_TRANSFER) lets
 * userspace send/receive arbitrary USB control transfers via the kernel's
 * usb_control_msg(), which bypasses usbfs interface-ownership checks.
 * snd-usb-audio continues to handle audio streaming undisturbed.
 *
 * The module also renames the snd-usb-audio mixer control
 * "EVO4  Playback Volume" (or "EVO8  Playback Volume") to
 * "Master Playback Volume" so PipeWire's ACP layer recognises it via the
 * standard analog-output.conf path and drives the hardware mixer for
 * `wpctl set-volume`, media keys, pavucontrol, etc.
 */

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <sound/control.h>
#include <sound/core.h>

#define AUDIENT_VID 0x2708
#define EVO4_PID 0x0006
#define EVO8_PID 0x0007

#define EVO_MAX_DATA 256

/* Model table - maps PID to device name */
static const struct evo_model {
  __u16 pid;
  const char *name;
} evo_models[] = {
    {EVO4_PID, "evo4"},
    {EVO8_PID, "evo8"},
};

/* ioctl payload - matches the struct userspace packs */
struct evo_ctrl_xfer {
  __u8 bRequestType;
  __u8 bRequest;
  __u16 wValue;
  __u16 wIndex;
  __u16 wLength;
  __u8 data[EVO_MAX_DATA];
};

/* ioctl number: type='E' (0x45), nr=0, read+write, size of struct */
#define EVO_CTRL_TRANSFER _IOWR('E', 0, struct evo_ctrl_xfer)

struct evo_device {
  struct usb_device *udev;
  struct miscdevice misc;
  struct mutex lock;
  char name[8]; /* "evo4" or "evo8" */
  struct delayed_work rename_work;
  int rename_retries;
};

/*
 * The snd-usb-audio card may not be fully initialised when our driver
 * binds to interface 3; poll for the control with a backoff.
 */
#define EVO_RENAME_RETRY_MS 200
#define EVO_RENAME_MAX_RETRIES 50 /* ~10 seconds total */
#define EVO_NEW_VOLUME_NAME "Master Playback Volume"

static int evo_open(struct inode *inode, struct file *file) {
  struct evo_device *dev =
      container_of(file->private_data, struct evo_device, misc);
  file->private_data = dev;
  return 0;
}

static long evo_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
  struct evo_device *dev = file->private_data;
  struct evo_ctrl_xfer xfer;
  unsigned int pipe;
  void *dmabuf;
  int ret;

  if (cmd != EVO_CTRL_TRANSFER)
    return -ENOTTY;

  if (copy_from_user(&xfer, (void __user *)arg, sizeof(xfer)))
    return -EFAULT;

  if (xfer.wLength > EVO_MAX_DATA)
    return -EINVAL;

  /* usb_control_msg requires a DMA-able buffer, not stack memory */
  dmabuf = kmalloc(xfer.wLength, GFP_KERNEL);
  if (!dmabuf)
    return -ENOMEM;

  /* For OUT transfers, copy data into the DMA buffer */
  if (!(xfer.bRequestType & USB_DIR_IN))
    memcpy(dmabuf, xfer.data, xfer.wLength);

  mutex_lock(&dev->lock);

  if (!dev->udev) {
    mutex_unlock(&dev->lock);
    kfree(dmabuf);
    return -ENODEV;
  }

  /* Build the correct pipe based on transfer direction */
  if (xfer.bRequestType & USB_DIR_IN)
    pipe = usb_rcvctrlpipe(dev->udev, 0);
  else
    pipe = usb_sndctrlpipe(dev->udev, 0);

  ret = usb_control_msg(dev->udev, pipe, xfer.bRequest, xfer.bRequestType,
                        xfer.wValue, xfer.wIndex, dmabuf, xfer.wLength,
                        1000 /* 1s timeout */);

  mutex_unlock(&dev->lock);

  if (ret < 0) {
    kfree(dmabuf);
    return ret;
  }

  /* For IN transfers, copy the response data back to userspace */
  if (xfer.bRequestType & USB_DIR_IN) {
    memcpy(xfer.data, dmabuf, ret);
    xfer.wLength = ret;
    if (copy_to_user((void __user *)arg, &xfer, sizeof(xfer))) {
      kfree(dmabuf);
      return -EFAULT;
    }
  }

  kfree(dmabuf);
  return ret;
}

static const struct file_operations evo_fops = {
    .owner = THIS_MODULE,
    .open = evo_open,
    .unlocked_ioctl = evo_ioctl,
};

/*
 * Locate the snd-usb-audio card that belongs to our USB device. We match
 * by walking card->dev up to the usb_device of the card and comparing
 * pointers - this is exact and robust against multiple EVOs of the same
 * model, unlike a shortname substring match.
 *
 * Caller owns a reference on success and must call snd_card_unref().
 */
static struct snd_card *evo_find_card(struct evo_device *dev) {
  struct snd_card *card;
  int i;

  for (i = 0; i < SNDRV_CARDS; i++) {
    struct device *d;

    card = snd_card_ref(i);
    if (!card)
      continue;

    for (d = card->dev; d; d = d->parent) {
      if (d == &dev->udev->dev)
        return card;
    }
    snd_card_unref(card);
  }
  return NULL;
}

static int evo_rename_volume(struct snd_card *card, const char *model) {
  struct snd_ctl_elem_id src, dst;
  char old_name[44];

  /*
   * snd-usb-audio derives the control name from the USB Feature Unit
   * string descriptor; on the EVO that string starts with the model name
   * followed by a space, and snd-usb-audio appends " Playback Volume",
   * yielding e.g. "EVO4  Playback Volume" (two spaces).
   */
  snprintf(old_name, sizeof(old_name), "%s  Playback Volume", model);

  memset(&src, 0, sizeof(src));
  src.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
  strscpy(src.name, old_name, sizeof(src.name));

  memcpy(&dst, &src, sizeof(dst));
  strscpy(dst.name, EVO_NEW_VOLUME_NAME, sizeof(dst.name));

  return snd_ctl_rename_id(card, &src, &dst);
}

static void evo_rename_work_fn(struct work_struct *work) {
  struct evo_device *dev =
      container_of(to_delayed_work(work), struct evo_device, rename_work);
  struct snd_card *card;
  const char *model;
  int ret;

  card = evo_find_card(dev);
  if (!card) {
    if (++dev->rename_retries < EVO_RENAME_MAX_RETRIES)
      schedule_delayed_work(&dev->rename_work,
                            msecs_to_jiffies(EVO_RENAME_RETRY_MS));
    return;
  }

  model = (strcmp(dev->name, "evo4") == 0) ? "EVO4" : "EVO8";
  ret = evo_rename_volume(card, model);

  if (ret == 0) {
    dev_info(card->dev,
             "evo_raw: renamed '%s  Playback Volume' -> '" EVO_NEW_VOLUME_NAME
             "'\n",
             model);
  } else if (ret == -ENOENT) {
    /* Control not present yet (snd-usb-audio still probing) or already
     * renamed by a previous module load. Retry a few times. */
    if (++dev->rename_retries < EVO_RENAME_MAX_RETRIES) {
      snd_card_unref(card);
      schedule_delayed_work(&dev->rename_work,
                            msecs_to_jiffies(EVO_RENAME_RETRY_MS));
      return;
    }
    dev_dbg(card->dev,
            "evo_raw: '%s  Playback Volume' not found; assuming already "
            "renamed or unsupported firmware\n",
            model);
  } else {
    dev_warn(card->dev, "evo_raw: control rename failed: %d\n", ret);
  }

  snd_card_unref(card);
}

static const char *evo_find_name(__u16 pid) {
  int i;
  for (i = 0; i < ARRAY_SIZE(evo_models); i++) {
    if (evo_models[i].pid == pid)
      return evo_models[i].name;
  }
  return NULL;
}

static int evo_probe(struct usb_interface *intf,
                     const struct usb_device_id *id) {
  struct usb_device *udev = interface_to_usbdev(intf);
  struct evo_device *dev;
  const char *name;

  /*
   * snd-usb-audio claims interfaces 0-2 (audio control + streaming).
   * Interface 3 (DFU) is left unbound - we grab it just to get the
   * usb_device handle. We don't actually use interface 3 for anything;
   * all our work goes through endpoint 0 (control pipe).
   */
  if (intf->cur_altsetting->desc.bInterfaceNumber != 3)
    return -ENODEV;

  name = evo_find_name(le16_to_cpu(udev->descriptor.idProduct));
  if (!name)
    return -ENODEV;

  dev = kzalloc(sizeof(*dev), GFP_KERNEL);
  if (!dev)
    return -ENOMEM;

  mutex_init(&dev->lock);
  strscpy(dev->name, name, sizeof(dev->name));
  dev->udev = usb_get_dev(udev);
  dev->misc.minor = MISC_DYNAMIC_MINOR;
  dev->misc.name = dev->name;
  dev->misc.fops = &evo_fops;
  INIT_DELAYED_WORK(&dev->rename_work, evo_rename_work_fn);

  if (misc_register(&dev->misc)) {
    dev_err(&intf->dev, "failed to register /dev/%s\n", dev->name);
    usb_put_dev(dev->udev);
    kfree(dev);
    return -ENODEV;
  }

  dev_info(&intf->dev, "Audient %s raw control registered at /dev/%s\n",
           dev->name, dev->name);
  usb_set_intfdata(intf, dev);
  schedule_delayed_work(&dev->rename_work,
                        msecs_to_jiffies(EVO_RENAME_RETRY_MS));
  return 0;
}

static void evo_disconnect(struct usb_interface *intf) {
  struct evo_device *dev = usb_get_intfdata(intf);

  if (!dev)
    return;

  cancel_delayed_work_sync(&dev->rename_work);

  mutex_lock(&dev->lock);
  misc_deregister(&dev->misc);
  usb_put_dev(dev->udev);
  dev->udev = NULL;
  mutex_unlock(&dev->lock);

  dev_info(&intf->dev, "Audient %s raw control disconnected\n", dev->name);
  kfree(dev);
}

static const struct usb_device_id evo_id_table[] = {
    {USB_DEVICE(AUDIENT_VID, EVO4_PID)},
    {USB_DEVICE(AUDIENT_VID, EVO8_PID)},
    {}};
MODULE_DEVICE_TABLE(usb, evo_id_table);

static struct usb_driver evo_driver = {
    .name = "evo_raw",
    .id_table = evo_id_table,
    .probe = evo_probe,
    .disconnect = evo_disconnect,
};
module_usb_driver(evo_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("audient-evo-py contributors");
MODULE_DESCRIPTION("Raw USB control transfer access for Audient EVO series");
