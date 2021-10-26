/*
 * AV201x Airoha Technology silicon tuner driver
 *
 * Copyright (C) 2014 Luis Alves <ljalvs@gmail.com>
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 */

#ifndef AV201X_H
#define AV201X_H

#include <linux/kconfig.h>
#include <media/dvb_frontend.h>

enum av201x_id {
	ID_AV2011,
	ID_AV2012,
	ID_AV2018,
};

struct av201x_config {
	/* tuner i2c address */
	u8 i2c_address;
	/* tuner type */
	enum av201x_id id;

	/* crystal freq in kHz */
	u32 xtal_freq;
};

#if IS_REACHABLE(CONFIG_MEDIA_TUNER_AV201X)
extern struct dvb_frontend *av201x_attach(struct dvb_frontend *fe,
					  struct av201x_config *cfg,
					  struct i2c_adapter *i2c);
#else
static inline struct dvb_frontend *av201x_attach(struct dvb_frontend *fe,
						 struct av201x_config *cfg,
						 struct i2c_adapter *i2c)
{
	printk(KERN_WARNING "%s: driver disabled by Kconfig\n", __func__);
	return NULL;
}
#endif

#endif /* AV201X_H */
