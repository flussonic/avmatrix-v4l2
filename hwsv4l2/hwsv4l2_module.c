// SPDX-License-Identifier: GPL-2.0-only
/*
 * hwsv4l2: a V4L2 driver for AVMatrix HWS PCIe capture cards.
 *
 * One PCI function carries a capture core with up to four inputs, each with
 * its own frame engine and, on most boards, its own audio engine. Every
 * input becomes a capture node (see include/sdi_av.h for the buffer
 * layout); the card is a media device with the connectors as entities.
 * Nothing on the card is interrupt driven: the frame and audio events are
 * polled at 4 kHz while an engine runs (hws_poll_card() says why), and a
 * monitor reads the inputs five times a second, since the card tells
 * nothing when a signal comes or goes.
 *
 * The register-level knowledge -- the start sequence, the address windows,
 * the input status words -- comes from the vendor's driver as the baseline
 * driver of github.com/benhoff/hws carries it.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>

#include "hwsv4l2.h"

#define HWS_MONITOR_PERIOD	msecs_to_jiffies(200)
/*
 * How often the status word is read while an engine runs. Past 300 us the
 * card starts losing frames (hws_poll_card()), so that is the ceiling.
 */
static unsigned int hws_poll_us = 250;
module_param_named(poll_us, hws_poll_us, uint, 0644);
MODULE_PARM_DESC(poll_us, "Status poll period in microseconds while capturing, 50 to 300 (default 250)");
#define HWS_POLL_NS		((u64)clamp(hws_poll_us, 50u, 300u) * NSEC_PER_USEC)

struct hws_model {
	u16 device;
	const char *name;	/* the product, for the media device */
	const char *short_name;	/* without the interface, for the nodes */
	u8 inputs;
	u8 audio;		/* inputs with an audio engine */
	bool sdi;
};

/*
 * The boards of the family by PCI device id; each enumerates with vendor
 * 0x8888 or 0x1f33 and subsystem 8888:0007. Only the X4 HDMI has been seen
 * running under this driver.
 */
static const struct hws_model hws_models[] = {
	{ 0x8504, "HWS X4 HDMI", "HWS X4", 4, 4, false },
	{ 0x8524, "HWS 2x2 HDMI", "HWS 2x2", 4, 4, false },
	{ 0x6504, "HWS X4 SDI", "HWS X4", 4, 4, true },
	{ 0x6524, "HWS 2x2 SDI", "HWS 2x2", 4, 4, true },
	{ 0x9534, "HWS 9534", "HWS 9534", 4, 4, false },
	{ 0x8534, "HWS 8534", "HWS 8534", 4, 0, false },
	{ 0x8554, "HWS 8554", "HWS 8554", 4, 0, false },
	{ 0x8532, "HWS 8532", "HWS 8532", 2, 2, false },
	{ 0x8512, "HWS 8512", "HWS 8512", 2, 0, false },
	{ 0x6502, "HWS 6502", "HWS 6502", 2, 0, true },
	{ 0x8501, "HWS 8501", "HWS 8501", 1, 0, false },
};

#define HWS_ID(vend, dev) \
	{ PCI_DEVICE_SUB(vend, dev, 0x8888, 0x0007) }

static const struct pci_device_id hws_pci_ids[] = {
	HWS_ID(0x8888, 0x8504), HWS_ID(0x1f33, 0x8504),
	HWS_ID(0x8888, 0x8524), HWS_ID(0x1f33, 0x8524),
	HWS_ID(0x8888, 0x6504), HWS_ID(0x1f33, 0x6524),
	HWS_ID(0x8888, 0x9534), HWS_ID(0x1f33, 0x8534), HWS_ID(0x1f33, 0x8554),
	HWS_ID(0x8888, 0x8532), HWS_ID(0x8888, 0x8512), HWS_ID(0x1f33, 0x6502),
	HWS_ID(0x8888, 0x8501),
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, hws_pci_ids);

static const struct hws_model *hws_model_of(u16 device)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(hws_models); i++)
		if (hws_models[i].device == device)
			return &hws_models[i];
	return NULL;
}

/*
 * The card is polled, not interrupt driven. Its done bits are pulses: with
 * the bridge's interrupt enables set, a bit clears itself some 30 us after it
 * rises whether or not an interrupt reached the host, and on the X4 HDMI
 * most of them did not -- a 59.94 Hz input produced 25 to 40 interrupts a
 * second, over legacy and MSI alike, at any enable mask. With the enables
 * clear the bits latch until written back. The engine skips a frame whose
 * predecessor's bit is still up when it starts, so the bit has to be cleared
 * within the vertical blanking, 0.67 ms at 1080p: polled every 500 us the
 * card lost one frame in ten, every 250 us none. An event is dated to the
 * middle of the poll interval it fell in; the frame period is measured over
 * two seconds, which makes that error vanish in it.
 */
static void hws_poll_card(struct hws_card *card, u64 now)
{
	u64 at = card->last_poll_ns ? card->last_poll_ns + (now - card->last_poll_ns) / 2 : now;
	unsigned int i;
	u32 status;

	card->last_poll_ns = now;
	status = hws_rd(card, HWS_REG_INT_STATUS);
	if (status == 0xffffffff)
		return;
	if (status)
		hws_wr(card, HWS_REG_INT_STATUS, status);

	for (i = 0; i < card->nch; i++) {
		struct hws_chan *c = card->ch[i];

		if (!c)
			continue;
		spin_lock(&c->event_lock);
		/*
		 * The done event first: a register move that came due in the same
		 * interval is late for the frame just done, which is then given up
		 * rather than taken from a slot the next frame is already filling.
		 */
		if (status & HWS_INT_VDONE(i))
			hws_video_done(c, at);
		hws_video_poll(c, now);
		if (status & HWS_INT_ADONE(i)) {
			hws_audio_done(c, at);
			if (c->streaming && !list_empty(&c->waiting))
				schedule_work(&c->done_work);
		}
		spin_unlock(&c->event_lock);
	}
}

static enum hrtimer_restart hws_poll_fn(struct hrtimer *t)
{
	struct hws_card *card = container_of(t, struct hws_card, poll_timer);

	hws_poll_card(card, ktime_get_ns());
	hrtimer_forward_now(t, ns_to_ktime(HWS_POLL_NS));
	return HRTIMER_RESTART;
}

/* The poll runs while any engine of the card does. */
void hws_poll_get(struct hws_card *card)
{
	mutex_lock(&card->poll_lock);
	if (!card->poll_users++) {
		card->last_poll_ns = 0;
		hrtimer_start(&card->poll_timer, ns_to_ktime(HWS_POLL_NS), HRTIMER_MODE_REL);
	}
	mutex_unlock(&card->poll_lock);
}

void hws_poll_put(struct hws_card *card)
{
	bool stop;

	mutex_lock(&card->poll_lock);
	stop = !--card->poll_users;
	if (stop)
		hrtimer_cancel(&card->poll_timer);
	mutex_unlock(&card->poll_lock);
}

static void hws_monitor(struct work_struct *w)
{
	struct hws_card *card = container_of(to_delayed_work(w), struct hws_card, monitor);
	unsigned int i;

	for (i = 0; i < card->nch; i++) {
		hws_video_input_changed(card->ch[i]);
		hws_video_probe_tick(card->ch[i]);
	}
	schedule_delayed_work(&card->monitor, HWS_MONITOR_PERIOD);
}

/*
 * The media device carries what identifies the card: model, the PCI device
 * serial number when the board has one, the board version and the graph of
 * connectors to nodes (MEDIA_IOC_DEVICE_INFO, media-ctl -p).
 */
static void hws_media_init(struct hws_card *card)
{
	struct media_device *mdev = &card->mdev;
	u64 dsn = pci_get_dsn(card->pdev);

	mdev->dev = &card->pdev->dev;
	strscpy(mdev->model, card->name, sizeof(mdev->model));
	if (dsn)
		snprintf(mdev->serial, sizeof(mdev->serial), "%016llx", dsn);
	snprintf(mdev->bus_info, sizeof(mdev->bus_info), "PCI:%s", pci_name(card->pdev));
	mdev->hw_revision = card->device_ver << 8 | card->sub_ver;
	media_device_init(mdev);
	card->v4l2_dev.mdev = mdev;
}

static void hws_free_channels(struct hws_card *card)
{
	unsigned int i;

	for (i = 0; i < card->nch; i++) {
		if (!card->ch[i])
			continue;
		hws_chan_free(card->ch[i]);
		kfree(card->ch[i]);
		card->ch[i] = NULL;
	}
}

/*
 * The last reference to the card is gone: every node has been closed by
 * everyone who had it open, so the structures a file handle reaches -- the
 * queue lock, the controls, the v4l2 and media devices -- can go.
 */
static void hws_card_release(struct v4l2_device *v4l2_dev)
{
	struct hws_card *card = container_of(v4l2_dev, struct hws_card, v4l2_dev);
	unsigned int i;

	for (i = 0; i < card->nch; i++) {
		if (!card->ch[i])
			continue;
		v4l2_ctrl_handler_free(&card->ch[i]->ctrl_handler);
		kfree(card->ch[i]);
	}
	media_device_cleanup(&card->mdev);
	kfree(card);
}

/*
 * The engines stop and the card lets go of the bus before the memory they
 * write is freed; the poll stops with them, since it reads the channels.
 */
static void hws_card_teardown(struct hws_card *card)
{
	unsigned int i;

	WARN_ON(card->poll_users);
	hrtimer_cancel(&card->poll_timer);
	hws_card_stop(card);
	pci_clear_master(card->pdev);
	for (i = 0; i < card->nch; i++)
		if (card->ch[i])
			hws_chan_free(card->ch[i]);
}

static int hws_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	const struct hws_model *model = hws_model_of(pdev->device);
	struct hws_card *card;
	unsigned int i;
	int ret;

	if (!model)
		return -ENODEV;
	/* Not devres: a node held open outlives the device (hws_card_release()). */
	card = kzalloc(sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;
	card->pdev = pdev;
	card->name = model->name;
	card->short_name = model->short_name;
	card->sdi = model->sdi;
	card->nch = model->inputs;
	card->naudio = model->audio;
	spin_lock_init(&card->reg_lock);
	mutex_init(&card->start_lock);
	mutex_init(&card->poll_lock);
	INIT_DELAYED_WORK(&card->monitor, hws_monitor);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
	hrtimer_setup(&card->poll_timer, hws_poll_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
#else
	hrtimer_init(&card->poll_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	card->poll_timer.function = hws_poll_fn;
#endif
	card->v4l2_dev.release = hws_card_release;
	pci_set_drvdata(pdev, card);

	ret = pcim_enable_device(pdev);
	if (ret)
		goto err_card;
	pci_set_master(pdev);
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_card;
	ret = pcim_iomap_regions(pdev, BIT(0), HWS_DRV_NAME);
	if (ret)
		goto err_card;
	card->bar0 = pcim_iomap_table(pdev)[0];
	pcie_capability_set_word(pdev, PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_RELAX_EN);

	for (i = 0; i < card->nch; i++) {
		struct hws_chan *c = kzalloc(sizeof(*c), GFP_KERNEL);

		if (!c) {
			ret = -ENOMEM;
			goto err_channels;
		}
		c->card = card;
		c->index = i;
		c->has_audio = i < card->naudio;
		spin_lock_init(&c->event_lock);
		card->ch[i] = c;
		ret = hws_chan_alloc(c);
		if (ret)
			goto err_channels;
	}

	ret = hws_card_start(card);
	if (ret)
		goto err_channels;
	hws_media_init(card);
	ret = v4l2_device_register(&pdev->dev, &card->v4l2_dev);
	if (ret)
		goto err_media;
	/* From here on the card goes with its last reference. */

	for (i = 0; i < card->nch; i++) {
		ret = hws_video_register(card->ch[i]);
		if (ret)
			goto err_nodes;
	}
	ret = media_device_register(&card->mdev);
	if (ret)
		dev_warn(&pdev->dev, "no media device (%d)\n", ret);
	dev_info(&pdev->dev, "%s, board version %u.%u, %u inputs (%u with audio)\n",
		 card->name, card->device_ver, card->sub_ver, card->nch, card->naudio);
	schedule_delayed_work(&card->monitor, 0);
	return 0;

err_nodes:
	while (i--)
		hws_video_unregister(card->ch[i]);
	hws_card_teardown(card);
	v4l2_device_unregister(&card->v4l2_dev);
	v4l2_device_put(&card->v4l2_dev);
	return ret;
err_media:
	media_device_cleanup(&card->mdev);
	hws_card_stop(card);
err_channels:
	pci_clear_master(pdev);
	hws_free_channels(card);
err_card:
	kfree(card);
	return ret;
}

static void hws_remove(struct pci_dev *pdev)
{
	struct hws_card *card = pci_get_drvdata(pdev);
	unsigned int i;

	cancel_delayed_work_sync(&card->monitor);
	if (media_devnode_is_registered(card->mdev.devnode))
		media_device_unregister(&card->mdev);
	for (i = 0; i < card->nch; i++)
		hws_video_unregister(card->ch[i]);
	hws_card_teardown(card);
	v4l2_device_unregister(&card->v4l2_dev);
	v4l2_device_put(&card->v4l2_dev);
}

static void hws_shutdown(struct pci_dev *pdev)
{
	struct hws_card *card = pci_get_drvdata(pdev);

	cancel_delayed_work_sync(&card->monitor);
	hws_card_stop(card);
	pci_clear_master(pdev);
}

/*
 * Across a suspend the core loses its state. On resume it is started as at
 * probe, and the channels that were capturing get their registers back; a
 * rate measurement that was running times out and is taken again.
 */
static int hws_suspend(struct device *dev)
{
	struct hws_card *card = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&card->monitor);
	hws_card_stop(card);
	return 0;
}

static int hws_resume(struct device *dev)
{
	struct hws_card *card = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&card->start_lock);
	ret = hws_card_start(card);
	if (!ret)
		hws_restore_channels(card);
	mutex_unlock(&card->start_lock);
	schedule_delayed_work(&card->monitor, 0);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(hws_pm_ops, hws_suspend, hws_resume);

static struct pci_driver hws_pci_driver = {
	.name = HWS_DRV_NAME,
	.id_table = hws_pci_ids,
	.probe = hws_probe,
	.remove = hws_remove,
	.shutdown = hws_shutdown,
	.driver.pm = pm_sleep_ptr(&hws_pm_ops),
};
module_pci_driver(hws_pci_driver);

MODULE_DESCRIPTION("V4L2 driver for AVMatrix HWS capture cards");
MODULE_AUTHOR("Max Lapshin <max@flussonic.com>");
MODULE_AUTHOR("Ben Hoff <hoff.benjamin.k@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION(HWS_VERSION);
