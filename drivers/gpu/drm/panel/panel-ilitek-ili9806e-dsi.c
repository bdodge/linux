// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017-2018, Bootlin
 * Copyright (C) 2021, Henson Li <henson@cutiepi.io>
 * Copyright (C) 2021, Penk Chen <penk@cutiepi.io>
 * Copyright (C) 2022, Brian Dodge
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>
#define ILI9806_DATA		BIT(8)

#define ILI9806_MAX_MSG_LEN	6

struct ili9806e_msg {
	unsigned int len;
	u8 msg[ILI9806_MAX_MSG_LEN];
};

struct ili9806e_desc {
	const struct ili9806e_msg *init;
	const size_t init_length;
	const struct drm_display_mode *mode;
	const unsigned flags;
};

struct ili9806e {
	struct drm_panel		panel;
	struct mipi_dsi_device		*dsi;
	const struct ili9806e_desc	*desc;
	struct regulator		*power;
	struct gpio_desc		*reset;
};

#define ILI9806_SET_PAGE(page)	\
	{				\
		.len = 6,		\
		.msg = {		\
			0xFF,		\
			0xFF,		\
			0x98,		\
			0x06,		\
			0x04,		\
			(page)		\
		},			\
	}

#define ILI9806_SET_REG_PARAM(reg, data)	\
	{					\
		.len = 2,			\
		.msg = {			\
			(reg),			\
			(data),			\
		},				\
	}

#define ILI9806_SET_REG(reg)	\
	{					\
		.len = 1,			\
		.msg = { (reg) },		\
	}

#define ILI9806_DELAY(ms)	\
	{					\
		.len = 0,			\
		.msg = { (ms) },		\
	}

static const struct ili9806e_msg e35rc_b_mw420_init[] = {
	ILI9806_SET_PAGE(1),
	ILI9806_SET_REG_PARAM(0x08, 0x10), // Output SDA
	ILI9806_SET_REG_PARAM(0x20, 0x00), // Set DE/VSYNC mode
	ILI9806_SET_REG_PARAM(0x21, 0x01), // DE = 1 activr
	ILI9806_SET_REG_PARAM(0x23, 0x02),
	ILI9806_SET_REG_PARAM(0x25, 0x0A),
	ILI9806_SET_REG_PARAM(0x26, 0x14),
	ILI9806_SET_REG_PARAM(0x27, 0x14),
	ILI9806_SET_REG_PARAM(0x28, 0x00),
	ILI9806_SET_REG_PARAM(0x30, 0x03), // resolution 480 x 640
	ILI9806_SET_REG_PARAM(0x31, 0x00), // inversion setting
	ILI9806_SET_REG_PARAM(0x40, 0x11), // BT DDVDH DDVDL
	ILI9806_SET_REG_PARAM(0x41, 0x44), // avdd +5.2v,avee-5.2v 33
	ILI9806_SET_REG_PARAM(0x42, 0x01),
	ILI9806_SET_REG_PARAM(0x43, 0x89), //SET VGH clamp level +15v
	ILI9806_SET_REG_PARAM(0x44, 0x89), //SET VGL clamp level +10v
	ILI9806_SET_REG_PARAM(0x46, 0x34),
	ILI9806_SET_REG_PARAM(0x50, 0x90), //VREG1 for positive Gamma
	ILI9806_SET_REG_PARAM(0x51, 0x90), //VREG2 for negative Gamma
	ILI9806_SET_REG_PARAM(0x52, 0x00), //VCOM
	ILI9806_SET_REG_PARAM(0x53, 0x55), //Forward Flicker
	ILI9806_SET_REG_PARAM(0x54, 0x00),
	ILI9806_SET_REG_PARAM(0x55, 0x55), //Backward Flicker
	ILI9806_SET_REG_PARAM(0x60, 0x07),
	ILI9806_SET_REG_PARAM(0x61, 0x04),
	ILI9806_SET_REG_PARAM(0x62, 0x08),
	ILI9806_SET_REG_PARAM(0x63, 0x04),

	ILI9806_SET_REG_PARAM(0xA0, 0x00), // Positive Gamma
	ILI9806_SET_REG_PARAM(0xA1, 0x09),
	ILI9806_SET_REG_PARAM(0xA2, 0x11),
	ILI9806_SET_REG_PARAM(0xA3, 0x0B),
	ILI9806_SET_REG_PARAM(0xA4, 0x05),
	ILI9806_SET_REG_PARAM(0xA5, 0x05),
	ILI9806_SET_REG_PARAM(0xA6, 0x07),
	ILI9806_SET_REG_PARAM(0xA7, 0x06),
	ILI9806_SET_REG_PARAM(0xA8, 0x05),
	ILI9806_SET_REG_PARAM(0xA9, 0x0B),
	ILI9806_SET_REG_PARAM(0xAA, 0x1C),
	ILI9806_SET_REG_PARAM(0xAB, 0x14),
	ILI9806_SET_REG_PARAM(0xAC, 0x1A),
	ILI9806_SET_REG_PARAM(0xAD, 0x1C),
	ILI9806_SET_REG_PARAM(0xAE, 0x1E),
	ILI9806_SET_REG_PARAM(0xAF, 0x0E),

	ILI9806_SET_REG_PARAM(0xC0, 0x00),  // Negative Gamma
	ILI9806_SET_REG_PARAM(0xC1, 0x09),
	ILI9806_SET_REG_PARAM(0xC2, 0x11),
	ILI9806_SET_REG_PARAM(0xC3, 0x0B),
	ILI9806_SET_REG_PARAM(0xC4, 0x05),
	ILI9806_SET_REG_PARAM(0xC5, 0x05),
	ILI9806_SET_REG_PARAM(0xC6, 0x07),
	ILI9806_SET_REG_PARAM(0xC7, 0x06),
	ILI9806_SET_REG_PARAM(0xC8, 0x05),
	ILI9806_SET_REG_PARAM(0xC9, 0x0B),
	ILI9806_SET_REG_PARAM(0xCA, 0x1C),
	ILI9806_SET_REG_PARAM(0xCB, 0x14),
	ILI9806_SET_REG_PARAM(0xCC, 0x1A),
	ILI9806_SET_REG_PARAM(0xCD, 0x1C),
	ILI9806_SET_REG_PARAM(0xCE, 0x1E),
	ILI9806_SET_REG_PARAM(0xCF, 0x0E),

	ILI9806_SET_PAGE(6),
	ILI9806_SET_REG_PARAM(0x00, 0x21),
	ILI9806_SET_REG_PARAM(0x01, 0x09),
	ILI9806_SET_REG_PARAM(0x02, 0x00),
	ILI9806_SET_REG_PARAM(0x03, 0x00),
	ILI9806_SET_REG_PARAM(0x04, 0x01),
	ILI9806_SET_REG_PARAM(0x05, 0x01),
	ILI9806_SET_REG_PARAM(0x06, 0x98),
	ILI9806_SET_REG_PARAM(0x07, 0x05),
	ILI9806_SET_REG_PARAM(0x08, 0x02),
	ILI9806_SET_REG_PARAM(0x09, 0x00),
	ILI9806_SET_REG_PARAM(0x0A, 0x00),
	ILI9806_SET_REG_PARAM(0x0B, 0x00),
	ILI9806_SET_REG_PARAM(0x0C, 0x01),
	ILI9806_SET_REG_PARAM(0x0D, 0x01),
	ILI9806_SET_REG_PARAM(0x0E, 0x00),
	ILI9806_SET_REG_PARAM(0x0F, 0x00),

	ILI9806_SET_REG_PARAM(0x10, 0xE0),
	ILI9806_SET_REG_PARAM(0x11, 0xE0),
	ILI9806_SET_REG_PARAM(0x12, 0x00),
	ILI9806_SET_REG_PARAM(0x13, 0x00),
	ILI9806_SET_REG_PARAM(0x14, 0x00),
	ILI9806_SET_REG_PARAM(0x15, 0x43),
	ILI9806_SET_REG_PARAM(0x16, 0x0B),
	ILI9806_SET_REG_PARAM(0x17, 0x00),
	ILI9806_SET_REG_PARAM(0x18, 0x00),
	ILI9806_SET_REG_PARAM(0x19, 0x00),
	ILI9806_SET_REG_PARAM(0x1A, 0x00),
	ILI9806_SET_REG_PARAM(0x1B, 0x00),
	ILI9806_SET_REG_PARAM(0x1C, 0x00),
	ILI9806_SET_REG_PARAM(0x1D, 0x00),

	ILI9806_SET_REG_PARAM(0x20, 0x01),
	ILI9806_SET_REG_PARAM(0x21, 0x23),
	ILI9806_SET_REG_PARAM(0x22, 0x45),
	ILI9806_SET_REG_PARAM(0x23, 0x67),
	ILI9806_SET_REG_PARAM(0x24, 0x01),
	ILI9806_SET_REG_PARAM(0x25, 0x23),
	ILI9806_SET_REG_PARAM(0x26, 0x45),
	ILI9806_SET_REG_PARAM(0x27, 0x67),

	ILI9806_SET_REG_PARAM(0x30, 0x01),
	ILI9806_SET_REG_PARAM(0x31, 0x11),
	ILI9806_SET_REG_PARAM(0x32, 0x00),
	ILI9806_SET_REG_PARAM(0x33, 0x22),
	ILI9806_SET_REG_PARAM(0x34, 0x22),
	ILI9806_SET_REG_PARAM(0x35, 0xCB),
	ILI9806_SET_REG_PARAM(0x36, 0xDA),
	ILI9806_SET_REG_PARAM(0x37, 0xAD),
	ILI9806_SET_REG_PARAM(0x38, 0xBC),
	ILI9806_SET_REG_PARAM(0x39, 0x67),
	ILI9806_SET_REG_PARAM(0x3A, 0x76),
	ILI9806_SET_REG_PARAM(0x3B, 0x22),
	ILI9806_SET_REG_PARAM(0x3C, 0x22),
	ILI9806_SET_REG_PARAM(0x3D, 0x22),
	ILI9806_SET_REG_PARAM(0x3E, 0x22),
	ILI9806_SET_REG_PARAM(0x3F, 0x22),
	ILI9806_SET_REG_PARAM(0x40, 0x22),

	ILI9806_SET_PAGE(7),
	ILI9806_SET_REG_PARAM(0x18, 0x1D),
	ILI9806_SET_REG_PARAM(0x02, 0x77),
	ILI9806_SET_REG_PARAM(0xE1, 0x79),

	ILI9806_SET_PAGE(0),
	ILI9806_SET_REG(0x35), // TE ON
	/*ILI9806_SET_REG(0x34),  TE OFF */
	ILI9806_SET_REG_PARAM(0x36, 0x01),
	ILI9806_SET_REG_PARAM(0x3A, 0x70), // 24 Bit

	ILI9806_SET_REG(0x11), // Out of Sleep
	ILI9806_DELAY(120),
	ILI9806_SET_REG(0x29), // Display On
	ILI9806_DELAY(25),
};

#define NUM_INIT_REGS ARRAY_SIZE(panel_init)

static inline struct ili9806e *panel_to_ili9806e(struct drm_panel *panel)
{
	return container_of(panel, struct ili9806e, panel);
}

static int ili9806e_write_msg(struct ili9806e *ctx, const struct ili9806e_msg *msg)
{
	int ret;

	if (msg->len) {
		ret = mipi_dsi_dcs_write_buffer(ctx->dsi, msg->msg, msg->len);
		if (ret < 0) {
			return ret;
		}
	} else {
		msleep(msg->msg[0]);
	}

	return 0;
}

static int ili9806e_write_msg_list(struct ili9806e *ctx,
				   const struct ili9806e_msg msgs[],
				   unsigned int num_msgs)
{
	int ret;
	int i;

	for (i = 0; i < num_msgs; i++) {
		ret = ili9806e_write_msg(ctx, &msgs[i]);
		if (ret)
			break;
	}

	return ret;
}

static int ili9806e_prepare(struct drm_panel *panel)
{
	struct ili9806e *ctx = panel_to_ili9806e(panel);
	int ret;

	/* Power the panel */
	ret = regulator_enable(ctx->power);
	if (ret)
		return ret;
	msleep(5);

	/* And reset it */
	gpiod_set_value(ctx->reset, 1);
	msleep(40);

	gpiod_set_value(ctx->reset, 0);
	msleep(20);

	ret = ili9806e_write_msg_list(ctx, ctx->desc->init, ctx->desc->init_length);
	if (ret)
		return ret;

	ret = mipi_dsi_dcs_set_tear_on(ctx->dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret)
		return ret;

	ret = mipi_dsi_dcs_exit_sleep_mode(ctx->dsi);
	if (ret)
		return ret;

	return 0;
}

static int ili9806e_enable(struct drm_panel *panel)
{
	struct ili9806e *ctx = panel_to_ili9806e(panel);

	msleep(120);

	return mipi_dsi_dcs_set_display_on(ctx->dsi);
}

static int ili9806e_disable(struct drm_panel *panel)
{
	struct ili9806e *ctx = panel_to_ili9806e(panel);
	return mipi_dsi_dcs_set_display_off(ctx->dsi);
}

static int ili9806e_unprepare(struct drm_panel *panel)
{
	struct ili9806e *ctx = panel_to_ili9806e(panel);

	mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);
	regulator_disable(ctx->power);
	gpiod_set_value(ctx->reset, 1);
	return 0;
}

static const struct drm_display_mode e35rcb_default_mode = {
	.clock = 22400,

	.hdisplay = 480,
	.hsync_start = 480 + 10,
	.hsync_end = 480 + 10 + 8,
	.htotal = 480 + 10 + 8 + 2 + 39,

	.vdisplay = 640,
	.vsync_start = 640 + 14,
	.vsync_end = 640 + 14 + 8,
	.vtotal = 640 + 14 + 8 + 2 + 7,

/*	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC, */

	.width_mm = 61,
	.height_mm = 103,
};

static int ili9806e_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	struct ili9806e *ctx = panel_to_ili9806e(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, ctx->desc->mode);
	if (!mode) {
		dev_err(&ctx->dsi->dev, "failed to add mode %ux%ux@%u\n",
			ctx->desc->mode->hdisplay,
			ctx->desc->mode->vdisplay,
			drm_mode_vrefresh(ctx->desc->mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	return 1;
}

static const struct drm_panel_funcs ili9806e_funcs = {
	.prepare	= ili9806e_prepare,
	.unprepare	= ili9806e_unprepare,
	.enable		= ili9806e_enable,
	.disable	= ili9806e_disable,
	.get_modes	= ili9806e_get_modes,
};

static int ili9806e_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct ili9806e *ctx;
	int ret;

	ctx = devm_kzalloc(&dsi->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->power = devm_regulator_get(&dsi->dev, "power");
	if (IS_ERR(ctx->power)) {
		dev_err(&dsi->dev, "Couldn't get our power regulator\n");
		return PTR_ERR(ctx->power);
	}

	ctx->reset = devm_gpiod_get_optional(&dsi->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset)) {
		dev_err(&dsi->dev, "Couldn't get our reset GPIO\n");
		return PTR_ERR(ctx->reset);
	}

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dsi = dsi;
	ctx->desc = of_device_get_match_data(&dsi->dev);

	dsi->lanes = 2;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = ctx->desc->flags;

	drm_panel_init(&ctx->panel, &dsi->dev, &ili9806e_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ctx->panel.prepare_upstream_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(&dsi->dev, "mipi_dsi_attach() failed: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static int ili9806e_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct ili9806e *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	return 0;
}

static const struct ili9806e_desc e35rc_b_mw420_desc = {
	.init = e35rc_b_mw420_init,
	.init_length = ARRAY_SIZE(e35rc_b_mw420_init),
	.mode = &e35rcb_default_mode,
	.flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM
};

static const struct of_device_id ili9806e_of_match[] = {
	{ .compatible = "focus,e335rc-b-mw420", .data = &e35rc_b_mw420_desc },
	{}
};
MODULE_DEVICE_TABLE(of, ili9806e_of_match);

static struct mipi_dsi_driver ili9806e_dsi_driver = {
	.probe		= ili9806e_dsi_probe,
	.remove		= ili9806e_dsi_remove,
	.driver = {
		.name		= "ili9806e-dsi",
		.of_match_table	= ili9806e_of_match,
	},
};
module_mipi_dsi_driver(ili9806e_dsi_driver);

MODULE_AUTHOR("Brian Dodge <bdodge09@gmail.com>");
MODULE_DESCRIPTION("Ilitek ILI9806e MIPI/DSI Controller Driver");
MODULE_LICENSE("GPL v2");
