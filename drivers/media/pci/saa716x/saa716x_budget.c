#include <asm/page.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kmod.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pgtable.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>

#include "saa716x_mod.h"

#include "saa716x_gpio_reg.h"
#include "saa716x_greg_reg.h"
#include "saa716x_msi_reg.h"

#include "saa716x_adap.h"
#include "saa716x_i2c.h"
#include "saa716x_msi.h"
#include "saa716x_budget.h"
#include "saa716x_gpio.h"
#include "saa716x_rom.h"
#include "saa716x_priv.h"

#include "stv6110x.h"
#include "stv090x.h"
#include "cx24117.h"
#include "stb6100.h"
#include "stb6100_cfg.h"

#include "tda18212.h"
#include "cxd2820r.h"

#include "si2168.h"
#include "si2157.h"

#include "tbsci-i2c.h"
#include "tbs-ci.h"

unsigned int verbose;
module_param(verbose, int, 0644);
MODULE_PARM_DESC(verbose, "verbose startup messages, default is 1 (yes)");

unsigned int int_type;
module_param(int_type, int, 0644);
MODULE_PARM_DESC(int_type, "force Interrupt Handler type: 0=INT-A, 1=MSI, 2=MSI-X. default INT-A mode");

#define DRIVER_NAME	"SAA716x Budget"

static int saa716x_budget_pci_probe(struct pci_dev *pdev, const struct pci_device_id *pci_id)
{
	struct saa716x_dev *saa716x;
	int err = 0;

	saa716x = kzalloc(sizeof(*saa716x), GFP_KERNEL);
	if (!saa716x) {
		err = -ENOMEM;
		goto fail0;
	}

	saa716x->verbose	= verbose;
	saa716x->int_type	= int_type;
	saa716x->pdev		= pdev;
	saa716x->module		= THIS_MODULE;
	saa716x->config		= (struct saa716x_config *) pci_id->driver_data;

	err = saa716x_pci_init(saa716x);
	if (err) {
		dev_err(&pdev->dev, "SAA716x PCI Initialization failed");
		goto fail1;
	}

	err = saa716x_cgu_init(saa716x);
	if (err) {
		dev_err(&pdev->dev, "SAA716x CGU Init failed");
		goto fail1;
	}

	err = saa716x_core_boot(saa716x);
	if (err) {
		dev_err(&pdev->dev, "SAA716x Core Boot failed");
		goto fail2;
	}
	dev_err(&pdev->dev, "SAA716x Core Boot Success");

	err = saa716x_msi_init(saa716x);
	if (err) {
		dev_err(&pdev->dev, "SAA716x MSI Init failed");
		goto fail2;
	}

	err = saa716x_jetpack_init(saa716x);
	if (err) {
		dev_err(&pdev->dev, "SAA716x Jetpack core initialization failed");
		goto fail2;
	}

	err = saa716x_i2c_init(saa716x);
	if (err) {
		dev_err(&pdev->dev, "SAA716x I2C Initialization failed");
		goto fail2;
	}

	saa716x_gpio_init(saa716x);

	/* set default port mapping */
	SAA716x_EPWR(GREG, GREG_VI_CTRL, 0x2C688F0A);
	/* enable FGPI3, FGPI2, FGPI1 and FGPI0 for TS input from Port 2 and 6 */
	SAA716x_EPWR(GREG, GREG_FGPI_CTRL, 0x322);

	err = saa716x_dvb_init(saa716x);
	if (err) {
		dev_err(&pdev->dev, "SAA716x DVB initialization failed");
		goto fail3;
	}

	return 0;

fail3:
	saa716x_dvb_exit(saa716x);
	saa716x_i2c_exit(saa716x);
fail2:
	saa716x_pci_exit(saa716x);
fail1:
	kfree(saa716x);
fail0:
	return err;
}

static void saa716x_budget_pci_remove(struct pci_dev *pdev)
{
	struct saa716x_dev *saa716x = pci_get_drvdata(pdev);
	struct saa716x_adapter *saa716x_adap = saa716x->saa716x_adap;
	int i;

	for (i = 0; i < saa716x->config->adapters; i++) {
		if (saa716x_adap->tbsci) {
			tbsci_release(saa716x_adap);
			tbsci_i2c_remove(saa716x_adap);
		}

		saa716x_adap++;
	}

	saa716x_dvb_exit(saa716x);
	saa716x_i2c_exit(saa716x);
	saa716x_pci_exit(saa716x);
	kfree(saa716x);
}

static irqreturn_t saa716x_budget_pci_irq(int irq, void *dev_id)
{
	struct saa716x_dev *saa716x	= (struct saa716x_dev *) dev_id;

	u32 stat_h, stat_l, mask_h, mask_l;

	if (unlikely(!saa716x))
		return IRQ_NONE;

	stat_l = SAA716x_EPRD(MSI, MSI_INT_STATUS_L);
	stat_h = SAA716x_EPRD(MSI, MSI_INT_STATUS_H);
	mask_l = SAA716x_EPRD(MSI, MSI_INT_ENA_L);
	mask_h = SAA716x_EPRD(MSI, MSI_INT_ENA_H);

	dev_dbg(&saa716x->pdev->dev, "MSI STAT L=<%02x> H=<%02x>, CTL L=<%02x> H=<%02x>",
		stat_l, stat_h, mask_l, mask_h);

	if (!((stat_l & mask_l) || (stat_h & mask_h)))
		return IRQ_NONE;

	if (stat_l)
		SAA716x_EPWR(MSI, MSI_INT_STATUS_CLR_L, stat_l);

	if (stat_h)
		SAA716x_EPWR(MSI, MSI_INT_STATUS_CLR_H, stat_h);

	saa716x_msi_event(saa716x, stat_l, stat_h);

	if (stat_l) {
		if (stat_l & MSI_INT_TAGACK_FGPI_0)
			tasklet_schedule(&saa716x->fgpi[0].tasklet);

		if (stat_l & MSI_INT_TAGACK_FGPI_1)
			tasklet_schedule(&saa716x->fgpi[1].tasklet);

		if (stat_l & MSI_INT_TAGACK_FGPI_2)
			tasklet_schedule(&saa716x->fgpi[2].tasklet);

		if (stat_l & MSI_INT_TAGACK_FGPI_3)
			tasklet_schedule(&saa716x->fgpi[3].tasklet);
	}

	return IRQ_HANDLED;
}

static void demux_worker(unsigned long data)
{
	struct saa716x_fgpi_stream_port *fgpi_entry = (struct saa716x_fgpi_stream_port *)data;
	struct saa716x_dev *saa716x = fgpi_entry->saa716x;
	struct dvb_demux *demux;
	u32 fgpi_index;
	u32 i;
	u32 write_index;

	fgpi_index = fgpi_entry->dma_channel - 6;
	demux = NULL;
	for (i = 0; i < saa716x->config->adapters; i++) {
		if (saa716x->config->adap_config[i].ts_port == fgpi_index) {
			demux = &saa716x->saa716x_adap[i].demux;
			break;
		}
	}
	if (demux == NULL) {
		dev_err(&saa716x->pdev->dev, "%s: unexpected channel %u\n",
			__func__, fgpi_entry->dma_channel);
		return;
	}

	write_index = saa716x_fgpi_get_write_index(saa716x, fgpi_index);
	if (write_index < 0)
		return;

	dev_dbg(&saa716x->pdev->dev, "dma buffer = %d", write_index);

	if (write_index == fgpi_entry->read_index) {
		dev_dbg(&saa716x->pdev->dev, "%s: called but nothing to do\n",
			__func__);
		return;
	}

	do {
		u8 *data = (u8 *)fgpi_entry->dma_buf[fgpi_entry->read_index].mem_virt;

		pci_dma_sync_sg_for_cpu(saa716x->pdev,
			fgpi_entry->dma_buf[fgpi_entry->read_index].sg_list,
			fgpi_entry->dma_buf[fgpi_entry->read_index].list_len,
			PCI_DMA_FROMDEVICE);

		dvb_dmx_swfilter(demux, data, 348 * 188);

		fgpi_entry->read_index = (fgpi_entry->read_index + 1) & 7;
	} while (write_index != fgpi_entry->read_index);
}


#define SAA716x_MODEL_TWINHAN_VP3071	"Twinhan/Azurewave VP-3071"
#define SAA716x_DEV_TWINHAN_VP3071	"2x DVB-T"

static int saa716x_vp3071_frontend_attach(struct saa716x_adapter *adapter,
					  int count)
{
	struct saa716x_dev *saa716x = adapter->saa716x;

	dev_dbg(&saa716x->pdev->dev, "Adapter (%d) SAA716x frontend Init",
		count);
	dev_dbg(&saa716x->pdev->dev, "Adapter (%d) Device ID=%02x", count,
		saa716x->pdev->subsystem_device);

	return -ENODEV;
}

static struct saa716x_config saa716x_vp3071_config = {
	.model_name		= SAA716x_MODEL_TWINHAN_VP3071,
	.dev_type		= SAA716x_DEV_TWINHAN_VP3071,
	.boot_mode		= SAA716x_EXT_BOOT,
	.adapters		= 2,
	.frontend_attach	= saa716x_vp3071_frontend_attach,
	.irq_handler		= saa716x_budget_pci_irq,
	.i2c_rate		= SAA716x_I2C_RATE_100,
};


#define SAA716x_MODEL_TWINHAN_VP6002	"Twinhan/Azurewave VP-6002"
#define SAA716x_DEV_TWINHAN_VP6002	"DVB-S"

static int saa716x_vp6002_frontend_attach(struct saa716x_adapter *adapter,
					  int count)
{
	struct saa716x_dev *saa716x = adapter->saa716x;

	dev_dbg(&saa716x->pdev->dev, "Adapter (%d) SAA716x frontend Init",
		count);
	dev_dbg(&saa716x->pdev->dev, "Adapter (%d) Device ID=%02x", count,
		saa716x->pdev->subsystem_device);

	return -ENODEV;
}

static struct saa716x_config saa716x_vp6002_config = {
	.model_name		= SAA716x_MODEL_TWINHAN_VP6002,
	.dev_type		= SAA716x_DEV_TWINHAN_VP6002,
	.boot_mode		= SAA716x_EXT_BOOT,
	.adapters		= 1,
	.frontend_attach	= saa716x_vp6002_frontend_attach,
	.irq_handler		= saa716x_budget_pci_irq,
	.i2c_rate		= SAA716x_I2C_RATE_100,
};


#define SAA716x_MODEL_KNC1_DUALS2	"KNC One Dual S2"
#define SAA716x_DEV_KNC1_DUALS2		"1xDVB-S + 1xDVB-S/S2"

static int saa716x_knc1_duals2_frontend_attach(struct saa716x_adapter *adapter,
					       int count)
{
	struct saa716x_dev *saa716x = adapter->saa716x;

	dev_dbg(&saa716x->pdev->dev, "Adapter (%d) SAA716x frontend Init",
		count);
	dev_dbg(&saa716x->pdev->dev, "Adapter (%d) Device ID=%02x", count,
		saa716x->pdev->subsystem_device);

	return -ENODEV;
}

static struct saa716x_config saa716x_knc1_duals2_config = {
	.model_name		= SAA716x_MODEL_KNC1_DUALS2,
	.dev_type		= SAA716x_DEV_KNC1_DUALS2,
	.boot_mode		= SAA716x_EXT_BOOT,
	.adapters		= 2,
	.frontend_attach	= saa716x_knc1_duals2_frontend_attach,
	.irq_handler		= saa716x_budget_pci_irq,
	.i2c_rate		= SAA716x_I2C_RATE_100,
};


#define SAA716x_MODEL_SKYSTAR2_EXPRESS_HD	"SkyStar 2 eXpress HD"
#define SAA716x_DEV_SKYSTAR2_EXPRESS_HD		"DVB-S/S2"

static struct stv090x_config skystar2_stv090x_config = {
	.device			= STV0903,
	.demod_mode		= STV090x_SINGLE,
	.clk_mode		= STV090x_CLK_EXT,

	.xtal			= 8000000,
	.address		= 0x68,

	.ts1_mode		= STV090x_TSMODE_DVBCI,
	.ts2_mode		= STV090x_TSMODE_SERIAL_CONTINUOUS,

	.repeater_level		= STV090x_RPTLEVEL_16,

	.tuner_init		= NULL,
	.tuner_sleep		= NULL,
	.tuner_set_mode		= NULL,
	.tuner_set_frequency	= NULL,
	.tuner_get_frequency	= NULL,
	.tuner_set_bandwidth	= NULL,
	.tuner_get_bandwidth	= NULL,
	.tuner_set_bbgain	= NULL,
	.tuner_get_bbgain	= NULL,
	.tuner_set_refclk	= NULL,
	.tuner_get_status	= NULL,
};

static int skystar2_set_voltage(struct dvb_frontend *fe,
				enum fe_sec_voltage voltage)
{
	int err;
	u8 en = 0;
	u8 sel = 0;

	switch (voltage) {
	case SEC_VOLTAGE_OFF:
		en = 0;
		break;

	case SEC_VOLTAGE_13:
		en = 1;
		sel = 0;
		break;

	case SEC_VOLTAGE_18:
		en = 1;
		sel = 1;
		break;

	default:
		break;
	}

	err = skystar2_stv090x_config.set_gpio(fe, 2, 0, en, 0);
	if (err < 0)
		goto exit;
	err = skystar2_stv090x_config.set_gpio(fe, 3, 0, sel, 0);
	if (err < 0)
		goto exit;

	return 0;
exit:
	return err;
}

static int skystar2_voltage_boost(struct dvb_frontend *fe, long arg)
{
	int err;
	u8 value;

	if (arg)
		value = 1;
	else
		value = 0;

	err = skystar2_stv090x_config.set_gpio(fe, 4, 0, value, 0);
	if (err < 0)
		goto exit;

	return 0;
exit:
	return err;
}

static struct stv6110x_config skystar2_stv6110x_config = {
	.addr			= 0x60,
	.refclk			= 16000000,
	.clk_div		= 2,
};

static int skystar2_express_hd_frontend_attach(struct saa716x_adapter *adapter,
					       int count)
{
	struct saa716x_dev *saa716x = adapter->saa716x;
	struct saa716x_i2c *i2c = &saa716x->i2c[SAA716x_I2C_BUS_B];
	struct stv6110x_devctl *ctl;

	if (count < saa716x->config->adapters) {
		dev_dbg(&saa716x->pdev->dev, "Adapter (%d) SAA716x frontend Init",
			count);
		dev_dbg(&saa716x->pdev->dev, "Adapter (%d) Device ID=%02x",
			count, saa716x->pdev->subsystem_device);

		saa716x_gpio_set_output(saa716x, 26);

		/* Reset the demodulator */
		saa716x_gpio_write(saa716x, 26, 1);
		saa716x_gpio_write(saa716x, 26, 0);
		usleep_range(10000, 15000);
		saa716x_gpio_write(saa716x, 26, 1);
		usleep_range(10000, 15000);

		adapter->fe = dvb_attach(stv090x_attach,
					 &skystar2_stv090x_config,
					 &i2c->i2c_adapter,
					 STV090x_DEMODULATOR_0);

		if (adapter->fe) {
			dev_info(&saa716x->pdev->dev, "found STV0903 @0x%02x",
				 skystar2_stv090x_config.address);
		} else {
			goto exit;
		}

		adapter->fe->ops.set_voltage = skystar2_set_voltage;
		adapter->fe->ops.enable_high_lnb_voltage =
						skystar2_voltage_boost;

		ctl = dvb_attach(stv6110x_attach, adapter->fe,
				 &skystar2_stv6110x_config, &i2c->i2c_adapter);

		if (ctl) {
			dev_info(&saa716x->pdev->dev, "found STV6110(A) @0x%02x",
				 skystar2_stv6110x_config.addr);

			skystar2_stv090x_config.tuner_init = ctl->tuner_init;
			skystar2_stv090x_config.tuner_sleep = ctl->tuner_sleep;
			skystar2_stv090x_config.tuner_set_mode =
						ctl->tuner_set_mode;
			skystar2_stv090x_config.tuner_set_frequency =
						ctl->tuner_set_frequency;
			skystar2_stv090x_config.tuner_get_frequency =
						ctl->tuner_get_frequency;
			skystar2_stv090x_config.tuner_set_bandwidth =
						ctl->tuner_set_bandwidth;
			skystar2_stv090x_config.tuner_get_bandwidth =
						ctl->tuner_get_bandwidth;
			skystar2_stv090x_config.tuner_set_bbgain =
						ctl->tuner_set_bbgain;
			skystar2_stv090x_config.tuner_get_bbgain =
						ctl->tuner_get_bbgain;
			skystar2_stv090x_config.tuner_set_refclk =
						ctl->tuner_set_refclk;
			skystar2_stv090x_config.tuner_get_status =
						ctl->tuner_get_status;

			/*
			 * call the init function once to initialize
			 * tuner's clock output divider and demod's
			 *  master clock
			 */
			if (adapter->fe->ops.init)
				adapter->fe->ops.init(adapter->fe);
		} else {
			goto exit;
		}

		return 0;
	}
exit:
	dev_err(&saa716x->pdev->dev, "Frontend attach failed");
	return -ENODEV;
}

static struct saa716x_config skystar2_express_hd_config = {
	.model_name		= SAA716x_MODEL_SKYSTAR2_EXPRESS_HD,
	.dev_type		= SAA716x_DEV_SKYSTAR2_EXPRESS_HD,
	.boot_mode		= SAA716x_EXT_BOOT,
	.adapters		= 1,
	.frontend_attach	= skystar2_express_hd_frontend_attach,
	.irq_handler		= saa716x_budget_pci_irq,
	.i2c_rate		= SAA716x_I2C_RATE_100,
	.adap_config		= {
		{
			/* Adapter 0 */
			.ts_port = 1, /* using FGPI 1 */
			.worker = demux_worker
		}
	}
};


#define SAA716x_MODEL_TBS6284		"TurboSight TBS 6284 "
#define SAA716x_DEV_TBS6284		"DVB-T/T2/C"

static struct cxd2820r_config cxd2820r_config[] = {
	{
		.i2c_address = 0x6c, /* (0xd8 >> 1) */
		.ts_mode = 0x38,
	},
	{
		.i2c_address = 0x6d, /* (0xda >> 1) */
		.ts_mode = 0x38,
	}
};

static struct tda18212_config tda18212_config[] = {
	{
		/* .i2c_address = 0x60  (0xc0 >> 1) */
		.if_dvbt_6 = 3550,
		.if_dvbt_7 = 3700,
		.if_dvbt_8 = 4150,
		.if_dvbt2_6 = 3250,
		.if_dvbt2_7 = 4000,
		.if_dvbt2_8 = 4000,
		.if_dvbc = 5000,
		.loop_through = 1,
		.xtout = 1
	},
	{
		/* .i2c_address = 0x63  (0xc6 >> 1) */
		.if_dvbt_6 = 3550,
		.if_dvbt_7 = 3700,
		.if_dvbt_8 = 4150,
		.if_dvbt2_6 = 3250,
		.if_dvbt2_7 = 4000,
		.if_dvbt2_8 = 4000,
		.if_dvbc = 5000,
		.loop_through = 0,
		.xtout = 0
	},
};

static int saa716x_tbs_read_mac(struct saa716x_dev *saa716x, int count, u8 *mac)
{
	return saa716x_read_rombytes(saa716x, 0x2A0 + count * 16, 6, mac);
}

static int saa716x_tbs6284_frontend_attach(struct saa716x_adapter *adapter,
					   int count)
{
	struct saa716x_dev *dev = adapter->saa716x;
	struct saa716x_i2c *i2c = &dev->i2c[1 - (count >> 1)];
	struct i2c_adapter *i2cadapter = &i2c->i2c_adapter;
	u8 mac[6];

	struct i2c_client *client;

	struct i2c_board_info board_info = {
		.type = "tda18212",
		.platform_data = &tda18212_config[count & 1],
	};


	if (count > 3)
		goto err;

	/* reset */
	if (count == 0) {
		saa716x_gpio_set_output(dev, 22);
		saa716x_gpio_write(dev, 22, 0);
		msleep(200);
		saa716x_gpio_write(dev, 22, 1);
		msleep(400);
	} else if (count == 2) {
		saa716x_gpio_set_output(dev, 12);
		saa716x_gpio_write(dev, 12, 0);
		msleep(200);
		saa716x_gpio_write(dev, 12, 1);
		msleep(400);
	}

	/* attach frontend */
	adapter->fe = dvb_attach(cxd2820r_attach, &cxd2820r_config[count & 1],
				 &i2c->i2c_adapter, NULL);
	if (!adapter->fe)
		goto err;

	/* attach tuner */
	board_info.addr = (count & 1) ? 0x63 : 0x60;
	tda18212_config[count & 1].fe = adapter->fe;
	request_module("tda18212");
	client = i2c_new_client_device(i2cadapter, &board_info);
	if (!client || !client->dev.driver) {
		dvb_frontend_detach(adapter->fe);
		goto err2;
	}
	if (!try_module_get(client->dev.driver->owner)) {
		i2c_unregister_device(client);
		dvb_frontend_detach(adapter->fe);
		goto err2;
	}
	adapter->i2c_client_tuner = client;

	strlcpy(adapter->fe->ops.info.name, dev->config->model_name, 52);
	strlcat(adapter->fe->ops.info.name, dev->config->dev_type, 52);

	dev_dbg(&dev->pdev->dev, "%s frontend %d attached\n",
		dev->config->model_name, count);

	if (!saa716x_tbs_read_mac(dev, count, mac)) {
		memcpy(adapter->dvb_adapter.proposed_mac, mac, 6);
		dev_notice(&dev->pdev->dev, "%s MAC[%d]=%pM\n",
			   dev->config->model_name, count,
			   adapter->dvb_adapter.proposed_mac);
	}

	return 0;
err2:
	dev_err(&dev->pdev->dev, "%s frontend %d tuner attach failed\n",
		dev->config->model_name, count);
err:
	dev_err(&dev->pdev->dev, "%s frontend %d attach failed\n",
		dev->config->model_name, count);

	adapter->fe = NULL;
	return -ENODEV;
}

static struct saa716x_config saa716x_tbs6284_config = {
	.model_name		= SAA716x_MODEL_TBS6284,
	.dev_type		= SAA716x_DEV_TBS6284,
	.boot_mode		= SAA716x_EXT_BOOT,
	.adapters		= 4,
	.frontend_attach	= saa716x_tbs6284_frontend_attach,
	.irq_handler		= saa716x_budget_pci_irq,
	.i2c_rate		= SAA716x_I2C_RATE_100,
	.i2c_mode		= SAA716x_I2C_MODE_POLLING,
	.adap_config		= {
		{
			/* adapter 0 */
			.ts_port = 3,
			.worker = demux_worker
		},
		{
			/* adapter 1 */
			.ts_port = 2,
			.worker = demux_worker
		},
		{
			/* adapter 2 */
			.ts_port = 1,
			.worker = demux_worker
		},
		{
			/* adapter 3 */
			.ts_port = 0,
			.worker = demux_worker
		}
	},
};


#define SAA716x_MODEL_TBS6280		"TurboSight TBS 6280 "
#define SAA716x_DEV_TBS6280		"DVB-T/T2/C"

static int saa716x_tbs6280_frontend_attach(struct saa716x_adapter *adapter,
					   int count)
{
	struct saa716x_dev *dev = adapter->saa716x;
	struct saa716x_i2c *i2c = &dev->i2c[SAA716x_I2C_BUS_A];
	struct i2c_adapter *i2cadapter = &i2c->i2c_adapter;
	u8 mac[6];

	struct i2c_client *client;

	struct i2c_board_info board_info = {
		.type = "tda18212",
		.platform_data = &tda18212_config[count & 1],
	};

	if (count > 1)
		goto err;

	/* reset */
	if (count == 0) {
		saa716x_gpio_set_output(dev, 2);
		saa716x_gpio_write(dev, 2, 0);
		msleep(200);
		saa716x_gpio_write(dev, 2, 1);
		msleep(400);
	}

	/* attach frontend */
	adapter->fe = dvb_attach(cxd2820r_attach, &cxd2820r_config[count],
				 &i2c->i2c_adapter, NULL);
	if (!adapter->fe)
		goto err;

	/* attach tuner */
	board_info.addr = (count & 1) ? 0x63 : 0x60;
	tda18212_config[count & 1].fe = adapter->fe;
	request_module("tda18212");
	client = i2c_new_client_device(i2cadapter, &board_info);
	if (!client || !client->dev.driver) {
		dvb_frontend_detach(adapter->fe);
		goto err2;
	}
	if (!try_module_get(client->dev.driver->owner)) {
		i2c_unregister_device(client);
		dvb_frontend_detach(adapter->fe);
		goto err2;
	}
	adapter->i2c_client_tuner = client;

	strlcpy(adapter->fe->ops.info.name, dev->config->model_name, 52);
	strlcat(adapter->fe->ops.info.name, dev->config->dev_type, 52);

	dev_dbg(&dev->pdev->dev, "%s frontend %d attached\n",
		dev->config->model_name, count);

	if (!saa716x_tbs_read_mac(dev, count, mac)) {
		memcpy(adapter->dvb_adapter.proposed_mac, mac, 6);
		dev_notice(&dev->pdev->dev, "%s MAC[%d]=%pM\n",
			   dev->config->model_name, count,
			   adapter->dvb_adapter.proposed_mac);
	}

	return 0;
err2:
	dev_err(&dev->pdev->dev, "%s frontend %d tuner attach failed\n",
		dev->config->model_name, count);
err:
	dev_err(&dev->pdev->dev, "%s frontend %d attach failed\n",
		dev->config->model_name, count);

	adapter->fe = NULL;
	return -ENODEV;
}

static struct saa716x_config saa716x_tbs6280_config = {
	.model_name		= SAA716x_MODEL_TBS6280,
	.dev_type		= SAA716x_DEV_TBS6280,
	.boot_mode		= SAA716x_EXT_BOOT,
	.adapters		= 2,
	.frontend_attach	= saa716x_tbs6280_frontend_attach,
	.irq_handler		= saa716x_budget_pci_irq,
	.i2c_rate		= SAA716x_I2C_RATE_100,
	.i2c_mode		= SAA716x_I2C_MODE_POLLING,
	.adap_config		= {
		{
			/* adapter 0 */
			.ts_port = 1, /* using FGPI 1 */
			.worker = demux_worker
		},
		{
			/* adapter 1 */
			.ts_port = 3, /* using FGPI 3 */
			.worker = demux_worker
		},
	},
};


#define SAA716x_MODEL_TBS6221		"TurboSight TBS 6221 "
#define SAA716x_DEV_TBS6221		"DVB-T/T2/C"

static int saa716x_tbs6221_frontend_attach(struct saa716x_adapter *adapter,
					   int count)
{
	struct saa716x_dev *dev = adapter->saa716x;
	struct saa716x_i2c *i2c = &dev->i2c[SAA716x_I2C_BUS_A];
	struct i2c_adapter *i2cadapter = &i2c->i2c_adapter;
	struct i2c_client *client;
	struct i2c_board_info info;
	struct si2168_config si2168_config;
	struct si2157_config si2157_config;
	u8 mac[6];

	if (count > 0)
		goto err;

	/* attach demod */
	memset(&si2168_config, 0, sizeof(si2168_config));
	si2168_config.i2c_adapter = &i2cadapter;
	si2168_config.fe = &adapter->fe;
	si2168_config.ts_mode = SI2168_TS_PARALLEL;
	si2168_config.ts_clock_gapped = true;
	memset(&info, 0, sizeof(struct i2c_board_info));
	strlcpy(info.type, "si2168", I2C_NAME_SIZE);
	info.addr = 0x64;
	info.platform_data = &si2168_config;
	request_module(info.type);
	client = i2c_new_client_device(&i2c->i2c_adapter, &info);
	if (!client || !client->dev.driver)
		goto err;

	if (!try_module_get(client->dev.driver->owner)) {
		i2c_unregister_device(client);
		goto err;
	}
	adapter->i2c_client_demod = client;

	/* attach tuner */
	memset(&si2157_config, 0, sizeof(si2157_config));
	si2157_config.fe = adapter->fe;
	si2157_config.if_port = 1;
	memset(&info, 0, sizeof(struct i2c_board_info));
	strlcpy(info.type, "si2157", I2C_NAME_SIZE);
	info.addr = 0x60;
	info.platform_data = &si2157_config;
	request_module(info.type);
	client = i2c_new_client_device(i2cadapter, &info);
	if (!client || !client->dev.driver) {
		module_put(adapter->i2c_client_demod->dev.driver->owner);
		i2c_unregister_device(adapter->i2c_client_demod);
		goto err;
	}
	if (!try_module_get(client->dev.driver->owner)) {
		i2c_unregister_device(client);
		module_put(adapter->i2c_client_demod->dev.driver->owner);
		i2c_unregister_device(adapter->i2c_client_demod);
		goto err;
	}
	adapter->i2c_client_tuner = client;

	strlcpy(adapter->fe->ops.info.name, dev->config->model_name, 52);
	strlcat(adapter->fe->ops.info.name, dev->config->dev_type, 52);

	dev_dbg(&dev->pdev->dev, "%s frontend %d attached\n",
		dev->config->model_name, count);

	if (!saa716x_tbs_read_mac(dev, count, mac)) {
		memcpy(adapter->dvb_adapter.proposed_mac, mac, 6);
		dev_notice(&dev->pdev->dev, "%s MAC[%d]=%pM\n",
			   dev->config->model_name, count,
			   adapter->dvb_adapter.proposed_mac);
	}

	return 0;
err:
	dev_err(&dev->pdev->dev, "%s frontend %d attach failed\n",
		dev->config->model_name, count);
	return -ENODEV;
}

static struct saa716x_config saa716x_tbs6221_config = {
	.model_name		= SAA716x_MODEL_TBS6221,
	.dev_type		= SAA716x_DEV_TBS6221,
	.boot_mode		= SAA716x_EXT_BOOT,
	.adapters		= 1,
	.frontend_attach	= saa716x_tbs6221_frontend_attach,
	.irq_handler		= saa716x_budget_pci_irq,
	.i2c_rate		= SAA716x_I2C_RATE_100,
	.i2c_mode		= SAA716x_I2C_MODE_POLLING,
	.adap_config		= {
		{
			/* adapter 0 */
			.ts_port = 3, /* using FGPI 3 */
			.worker = demux_worker
		},
	},
};
#define SAA716x_MODEL_TBS7220		"TurboSight TBS 7220 "
#define SAA716x_DEV_TBS7220		"DVB-T/T2/C"

static struct saa716x_config saa716x_tbs7220_config = {
	.model_name		= SAA716x_MODEL_TBS7220,
	.dev_type		= SAA716x_DEV_TBS7220,
	.boot_mode		= SAA716x_EXT_BOOT,
	.adapters		= 1,
	.frontend_attach	= saa716x_tbs6221_frontend_attach,
	.irq_handler		= saa716x_budget_pci_irq,
	.i2c_rate		= SAA716x_I2C_RATE_100,
	.i2c_mode		= SAA716x_I2C_MODE_POLLING,
	.adap_config	= {
		{
			.ts_port = 1,
			.worker = demux_worker
		},
	},

};
#define SAA716x_MODEL_TBS6281		"TurboSight TBS 6281 "
#define SAA716x_DEV_TBS6281		"DVB-T/T2/C"

static int saa716x_tbs6281_frontend_attach(struct saa716x_adapter *adapter,
					   int count)
{
	struct saa716x_dev *dev = adapter->saa716x;
	struct i2c_adapter *i2cadapter;
	struct i2c_client *client;
	struct i2c_board_info info;
	struct si2168_config si2168_config;
	struct si2157_config si2157_config;
	u8 mac[6];

	if (count > 1)
		goto err;

	/* reset */
	saa716x_gpio_set_output(dev, count ? 2 : 16);
	saa716x_gpio_write(dev, count ? 2 : 16, 0);
	msleep(50);
	saa716x_gpio_write(dev, count ? 2 : 16, 1);
	msleep(100);

	/* attach demod */
	memset(&si2168_config, 0, sizeof(si2168_config));
	si2168_config.i2c_adapter = &i2cadapter;
	si2168_config.fe = &adapter->fe;
	si2168_config.ts_mode = SI2168_TS_PARALLEL;
	si2168_config.ts_clock_gapped = true;
	memset(&info, 0, sizeof(struct i2c_board_info));
	strlcpy(info.type, "si2168", I2C_NAME_SIZE);
	info.addr = 0x64;
	info.platform_data = &si2168_config;
	request_module(info.type);
	client = i2c_new_client_device(&dev->i2c[1 - count].i2c_adapter, &info);
	if (!client || !client->dev.driver)
		goto err;

	if (!try_module_get(client->dev.driver->owner)) {
		i2c_unregister_device(client);
		goto err;
	}
	adapter->i2c_client_demod = client;

	/* attach tuner */
	memset(&si2157_config, 0, sizeof(si2157_config));
	si2157_config.fe = adapter->fe;
	si2157_config.if_port = 1;
	memset(&info, 0, sizeof(struct i2c_board_info));
	strlcpy(info.type, "si2157", I2C_NAME_SIZE);
	info.addr = 0x60;
	info.platform_data = &si2157_config;
	request_module(info.type);
	client = i2c_new_client_device(i2cadapter, &info);
	if (!client || !client->dev.driver) {
		module_put(adapter->i2c_client_demod->dev.driver->owner);
		i2c_unregister_device(adapter->i2c_client_demod);
		goto err;
	}
	if (!try_module_get(client->dev.driver->owner)) {
		i2c_unregister_device(client);
		module_put(adapter->i2c_client_demod->dev.driver->owner);
		i2c_unregister_device(adapter->i2c_client_demod);
		goto err;
	}
	adapter->i2c_client_tuner = client;

	strlcpy(adapter->fe->ops.info.name, dev->config->model_name, 52);
	strlcat(adapter->fe->ops.info.name, dev->config->dev_type, 52);

	dev_dbg(&dev->pdev->dev, "%s frontend %d attached\n",
		dev->config->model_name, count);

	if (!saa716x_tbs_read_mac(dev, count, mac)) {
		memcpy(adapter->dvb_adapter.proposed_mac, mac, 6);
		dev_notice(&dev->pdev->dev, "%s MAC[%d]=%pM\n", dev->config->model_name, count, adapter->dvb_adapter.proposed_mac);
	}

	return 0;
err:
	dev_err(&dev->pdev->dev, "%s frontend %d attach failed\n",
		dev->config->model_name, count);
	return -ENODEV;
}

static struct saa716x_config saa716x_tbs6281_config = {
	.model_name		= SAA716x_MODEL_TBS6281,
	.dev_type		= SAA716x_DEV_TBS6281,
	.boot_mode		= SAA716x_EXT_BOOT,
	.adapters		= 2,
	.frontend_attach	= saa716x_tbs6281_frontend_attach,
	.irq_handler		= saa716x_budget_pci_irq,
	.i2c_rate		= SAA716x_I2C_RATE_400,
	.i2c_mode		= SAA716x_I2C_MODE_POLLING,
	.adap_config		= {
		{
			/* adapter 0 */
			.ts_port = 1, /* using FGPI 1 */
			.worker = demux_worker
		},
		{
			/* adapter 1 */
			.ts_port = 3, /* using FGPI 3 */
			.worker = demux_worker
		},
	},
};


#define SAA716x_MODEL_TBS6285		"TurboSight TBS 6285 "
#define SAA716x_DEV_TBS6285		"DVB-T/T2/C"

static int saa716x_tbs6285_frontend_attach(struct saa716x_adapter *adapter, int count)
{
	struct saa716x_dev *dev = adapter->saa716x;
	struct i2c_adapter *i2cadapter;
	struct i2c_client *client;
	struct i2c_board_info info;
	struct si2168_config si2168_config;
	struct si2157_config si2157_config;
	u8 mac[6];

	if (count > 3)
		goto err;

	/* attach demod */
	memset(&si2168_config, 0, sizeof(si2168_config));
	si2168_config.i2c_adapter = &i2cadapter;
	si2168_config.fe = &adapter->fe;
	si2168_config.ts_mode = SI2168_TS_SERIAL;
	si2168_config.ts_clock_gapped = true;
	si2168_config.ts_clock_inv = 1;
	memset(&info, 0, sizeof(struct i2c_board_info));
	strlcpy(info.type, "si2168", I2C_NAME_SIZE);
	info.addr = ((count == 0) || (count == 2)) ? 0x64 : 0x66;
	info.platform_data = &si2168_config;
	request_module(info.type);
	client = i2c_new_client_device(((count == 0) || (count == 1)) ?
		&dev->i2c[1].i2c_adapter : &dev->i2c[0].i2c_adapter,
		&info);
	if (!client || !client->dev.driver)
		goto err;

	if (!try_module_get(client->dev.driver->owner)) {
		i2c_unregister_device(client);
		goto err;
	}
	adapter->i2c_client_demod = client;

	/* attach tuner */
	memset(&si2157_config, 0, sizeof(si2157_config));
	si2157_config.fe = adapter->fe;
	si2157_config.if_port = 1;
	memset(&info, 0, sizeof(struct i2c_board_info));
	strlcpy(info.type, "si2157", I2C_NAME_SIZE);
	info.addr = ((count == 0) || (count == 2)) ? 0x62 : 0x60;
	info.platform_data = &si2157_config;
	request_module(info.type);
	client = i2c_new_client_device(i2cadapter, &info);
	if (!client || !client->dev.driver) {
		module_put(adapter->i2c_client_demod->dev.driver->owner);
		i2c_unregister_device(adapter->i2c_client_demod);
		goto err;
	}
	if (!try_module_get(client->dev.driver->owner)) {
		i2c_unregister_device(client);
		module_put(adapter->i2c_client_demod->dev.driver->owner);
		i2c_unregister_device(adapter->i2c_client_demod);
		goto err;
	}
	adapter->i2c_client_tuner = client;

	dev_dbg(&dev->pdev->dev, "%s frontend %d attached\n",
		dev->config->model_name, count);

	strlcpy(adapter->fe->ops.info.name, dev->config->model_name, 52);
	strlcat(adapter->fe->ops.info.name, dev->config->dev_type, 52);

	if (!saa716x_tbs_read_mac(dev, count, mac)) {
		memcpy(adapter->dvb_adapter.proposed_mac, mac, 6);
		dev_notice(&dev->pdev->dev, "%s MAC[%d]=%pM\n",
			   dev->config->model_name, count,
			   adapter->dvb_adapter.proposed_mac);
	}

	return 0;
err:
	dev_err(&dev->pdev->dev, "%s frontend %d attach failed\n",
		dev->config->model_name, count);
	return -ENODEV;
}

static struct saa716x_config saa716x_tbs6285_config = {
	.model_name		= SAA716x_MODEL_TBS6285,
	.dev_type		= SAA716x_DEV_TBS6285,
	.boot_mode		= SAA716x_EXT_BOOT,
	.adapters		= 4,
	.frontend_attach	= saa716x_tbs6285_frontend_attach,
	.irq_handler		= saa716x_budget_pci_irq,
	.i2c_rate		= SAA716x_I2C_RATE_400,
	.i2c_mode		= SAA716x_I2C_MODE_POLLING,
	.adap_config		= {
		{
			/* adapter 0 */
			.ts_port = 3,
			.worker = demux_worker
		},
		{
			/* adapter 1 */
			.ts_port = 2,
			.worker = demux_worker
		},
		{
			/* adapter 1 */
			.ts_port = 1,
			.worker = demux_worker
		},
		{
			/* adapter 1 */
			.ts_port = 0,
			.worker = demux_worker
		},
	},
};


#define SAA716x_MODEL_TBS6220		"TurboSight TBS 6220 "
#define SAA716x_DEV_TBS6220		"DVB-T/T2/C"

static int saa716x_tbs6220_frontend_attach(struct saa716x_adapter *adapter,
					   int count)
{
	struct saa716x_dev *dev = adapter->saa716x;
	struct saa716x_i2c *i2c = &dev->i2c[SAA716x_I2C_BUS_A];
	struct i2c_adapter *i2cadapter = &i2c->i2c_adapter;
	u8 mac[6];

	struct i2c_client *client;

	struct i2c_board_info board_info = {
		.type = "tda18212",
		.addr = 0x60,
		.platform_data = &tda18212_config[0],
	};


	if (count > 0)
		goto err;

	/* attach frontend */
	adapter->fe = dvb_attach(cxd2820r_attach, &cxd2820r_config[0],
				 &i2c->i2c_adapter, NULL);
	if (!adapter->fe)
		goto err;

	/* attach tuner */
	tda18212_config[0].fe = adapter->fe;
	request_module("tda18212");
	client = i2c_new_client_device(i2cadapter, &board_info);
	if (!client || !client->dev.driver) {
		dvb_frontend_detach(adapter->fe);
		goto err2;
	}
	if (!try_module_get(client->dev.driver->owner)) {
		i2c_unregister_device(client);
		dvb_frontend_detach(adapter->fe);
		goto err2;
	}
	adapter->i2c_client_tuner = client;

	strlcpy(adapter->fe->ops.info.name, dev->config->model_name, 52);
	strlcat(adapter->fe->ops.info.name, dev->config->dev_type, 52);

	dev_dbg(&dev->pdev->dev, "%s frontend %d attached\n",
		dev->config->model_name, count);

	if (!saa716x_tbs_read_mac(dev, count, mac)) {
		memcpy(adapter->dvb_adapter.proposed_mac, mac, 6);
		dev_notice(&dev->pdev->dev, "%s MAC=%pM\n",
			   dev->config->model_name,
			   adapter->dvb_adapter.proposed_mac);
	}

	return 0;
err2:
	dev_err(&dev->pdev->dev, "%s frontend %d tuner attach failed\n",
		dev->config->model_name, count);
err:
	dev_err(&dev->pdev->dev, "%s frontend %d attach failed\n",
		dev->config->model_name, count);

	adapter->fe = NULL;
	return -ENODEV;
}

static struct saa716x_config saa716x_tbs6220_config = {
	.model_name		= SAA716x_MODEL_TBS6220,
	.dev_type		= SAA716x_DEV_TBS6220,
	.boot_mode		= SAA716x_EXT_BOOT,
	.adapters		= 1,
	.frontend_attach	= saa716x_tbs6220_frontend_attach,
	.irq_handler		= saa716x_budget_pci_irq,
	.i2c_rate		= SAA716x_I2C_RATE_100,
	.i2c_mode		= SAA716x_I2C_MODE_POLLING,
	.adap_config		= {
		{
			/* adapter 0 */
			.ts_port = 3, /* using FGPI 3 */
			.worker = demux_worker
		},
	},
};

#define SAA716x_MODEL_TBS6925		"TurboSight TBS 6925 "
#define SAA716x_DEV_TBS6925		"DVB-S/S2"

static struct stv090x_config tbs6925_stv090x_cfg = {
	.device			= STV0900,
	.demod_mode		= STV090x_SINGLE,
	.clk_mode		= STV090x_CLK_EXT,

	.xtal			= 27000000,
	.address		= 0x68,

	.ts1_mode		= STV090x_TSMODE_PARALLEL_PUNCTURED,
	.ts2_mode		= STV090x_TSMODE_PARALLEL_PUNCTURED,

	.repeater_level		= STV090x_RPTLEVEL_16,
	.adc1_range		= STV090x_ADC_1Vpp,
	.tuner_bbgain		= 6,

	.tuner_get_frequency	= stb6100_get_frequency,
	.tuner_set_frequency	= stb6100_set_frequency,
	.tuner_set_bandwidth	= stb6100_set_bandwidth,
	.tuner_get_bandwidth	= stb6100_get_bandwidth,
};

static struct stb6100_config tbs6925_stb6100_cfg = {
	.tuner_address	= 0x60,
	.refclock	= 27000000
};

static int tbs6925_set_voltage(struct dvb_frontend *fe,
			       enum fe_sec_voltage voltage)
{
	struct saa716x_adapter *adapter = fe->dvb->priv;
	struct saa716x_dev *saa716x = adapter->saa716x;

	saa716x_gpio_set_output(saa716x, 16);
	usleep_range(1000, 1500);
	switch (voltage) {
	case SEC_VOLTAGE_13:
			saa716x_gpio_write(saa716x, 16, 0);
			break;
	case SEC_VOLTAGE_18:
			saa716x_gpio_write(saa716x, 16, 1);
			break;
	case SEC_VOLTAGE_OFF:
			break;
	default:
			return -EINVAL;
	}
	msleep(100);

	return 0;
}

static int tbs6925_frontend_attach(struct saa716x_adapter *adapter,
					       int count)
{
	struct saa716x_dev *dev = adapter->saa716x;
	u8 mac[6];

	dev_dbg(&dev->pdev->dev, "%s frontend %d attaching\n",
		dev->config->model_name, count);

	if (count > 0)
		goto err;

	/* Reset the demodulator */
	saa716x_gpio_set_output(dev, 2);
	saa716x_gpio_write(dev, 2, 0);
	msleep(50);
	saa716x_gpio_write(dev, 2, 1);
	msleep(100);

	adapter->fe = dvb_attach(stv090x_attach, &tbs6925_stv090x_cfg,
				&dev->i2c[SAA716x_I2C_BUS_A].i2c_adapter,
				STV090x_DEMODULATOR_0);

	if (adapter->fe == NULL)
		goto err;

	adapter->fe->ops.set_voltage = tbs6925_set_voltage;

	if (dvb_attach(stb6100_attach, adapter->fe,
			&tbs6925_stb6100_cfg,
			&dev->i2c[SAA716x_I2C_BUS_A].i2c_adapter) == NULL) {
		dvb_frontend_detach(adapter->fe);
		adapter->fe = NULL;
		dev_dbg(&dev->pdev->dev,
			"%s frontend %d tuner attach failed\n",
			dev->config->model_name, count);
		goto err;
	}

	if (adapter->fe->ops.init)
		adapter->fe->ops.init(adapter->fe);

	strlcpy(adapter->fe->ops.info.name, dev->config->model_name, 52);
	strlcat(adapter->fe->ops.info.name, dev->config->dev_type, 52);

	dev_dbg(&dev->pdev->dev, "%s frontend %d attached\n",
		dev->config->model_name, count);

	if (!saa716x_tbs_read_mac(dev, count, mac)) {
		memcpy(adapter->dvb_adapter.proposed_mac, mac, 6);
		dev_notice(&dev->pdev->dev, "%s MAC=%pM\n",
			   dev->config->model_name,
			   adapter->dvb_adapter.proposed_mac);
	}

	return 0;
err:
	dev_err(&dev->pdev->dev, "%s frontend %d attach failed\n",
		dev->config->model_name, count);
	return -ENODEV;
}

static struct saa716x_config saa716x_tbs6925_config = {
	.model_name		= SAA716x_MODEL_TBS6925,
	.dev_type		= SAA716x_DEV_TBS6925,
	.boot_mode		= SAA716x_EXT_BOOT,
	.adapters		= 1,
	.frontend_attach	= tbs6925_frontend_attach,
	.irq_handler		= saa716x_budget_pci_irq,
	.i2c_rate		= SAA716x_I2C_RATE_100,
	.i2c_mode		= SAA716x_I2C_MODE_POLLING,
	.adap_config		= {
		{
			/* Adapter 0 */
			.ts_port = 3, /* using FGPI 1 */
			.worker = demux_worker
		}
	}
};

#define SAA716x_MODEL_TBS6290 "TurboSight TBS 6290 "
#define SAA716x_DEV_TBS6290 "DVB-T/T2/C+2xCI"

static int saa716x_tbs6290_frontend_attach(struct saa716x_adapter *adapter,
					   int count)
{
	struct saa716x_dev *dev = adapter->saa716x;
	struct i2c_adapter *i2cadapter;
	struct i2c_client *client;
	struct i2c_board_info info;
	struct si2168_config si2168_config;
	struct si2157_config si2157_config;
	u8 mac[6];
	int ret;

	if (count > 1)
		goto err;

	/* attach demod */
	memset(&si2168_config, 0, sizeof(si2168_config));
	si2168_config.i2c_adapter = &i2cadapter;
	si2168_config.fe = &adapter->fe;
	si2168_config.ts_mode = SI2168_TS_PARALLEL;
	si2168_config.ts_clock_gapped = true;
	memset(&info, 0, sizeof(struct i2c_board_info));
	strlcpy(info.type, "si2168", I2C_NAME_SIZE);
	info.addr = 0x64;
	info.platform_data = &si2168_config;
	request_module(info.type);
	client = i2c_new_client_device(&dev->i2c[1 - count].i2c_adapter, &info);
	if (!client || !client->dev.driver)
		goto err;

	if (!try_module_get(client->dev.driver->owner)) {
		i2c_unregister_device(client);
		goto err;
	}
	adapter->i2c_client_demod = client;

	/* attach tuner */
	memset(&si2157_config, 0, sizeof(si2157_config));
	si2157_config.fe = adapter->fe;
	si2157_config.if_port = 1;
	memset(&info, 0, sizeof(struct i2c_board_info));
	strlcpy(info.type, "si2157", I2C_NAME_SIZE);
	info.addr = 0x60;
	info.platform_data = &si2157_config;
	request_module(info.type);
	client = i2c_new_client_device(i2cadapter, &info);
	if (!client || !client->dev.driver) {
		module_put(adapter->i2c_client_demod->dev.driver->owner);
		i2c_unregister_device(adapter->i2c_client_demod);
		goto err;
	}
	if (!try_module_get(client->dev.driver->owner)) {
		i2c_unregister_device(client);
		module_put(adapter->i2c_client_demod->dev.driver->owner);
		i2c_unregister_device(adapter->i2c_client_demod);
		goto err;
	}
	adapter->i2c_client_tuner = client;

	saa716x_gpio_set_input(dev, count ? 2 : 6);
	usleep_range(1000, 1500);
	saa716x_gpio_set_input(dev, count ? 14 : 3);
	usleep_range(1000, 1500);
	saa716x_gpio_set_input(dev, count ? 20 : 14);
	usleep_range(1000, 1500);
	ret = tbsci_i2c_probe(adapter, count ? 4 : 3);
	if (!ret)
		tbsci_init(adapter, count, 9);

	strlcpy(adapter->fe->ops.info.name, dev->config->model_name, 52);
	strlcat(adapter->fe->ops.info.name, dev->config->dev_type, 52);

	dev_dbg(&dev->pdev->dev, "%s frontend %d attached\n",
		dev->config->model_name, count);

	if (!saa716x_tbs_read_mac(dev, count, mac)) {
		memcpy(adapter->dvb_adapter.proposed_mac, mac, 6);
		dev_notice(&dev->pdev->dev, "%s MAC[%d]=%pM\n",
			   dev->config->model_name, count,
			   adapter->dvb_adapter.proposed_mac);
	}

	return 0;
err:
	dev_err(&dev->pdev->dev, "%s frontend %d attach failed\n",
		dev->config->model_name, count);
	return -ENODEV;


}

static struct saa716x_config saa716x_tbs6290_config = {
	.model_name		 = SAA716x_MODEL_TBS6290,
	.dev_type		 = SAA716x_DEV_TBS6290,
	.boot_mode		 = SAA716x_EXT_BOOT,
	.adapters		 = 2,
	.frontend_attach = saa716x_tbs6290_frontend_attach,
	.irq_handler	 = saa716x_budget_pci_irq,
	.i2c_rate		 = SAA716x_I2C_RATE_100,
	.i2c_mode		 = SAA716x_I2C_MODE_POLLING,
	.adap_config = {
		{
			.ts_port = 1,
			.worker  = demux_worker
		},
		{
			.ts_port = 3,
			.worker	 = demux_worker
		},
	}
};

static struct pci_device_id saa716x_budget_pci_table[] = {
	MAKE_ENTRY(TWINHAN_TECHNOLOGIES, TWINHAN_VP_3071, SAA7160, &saa716x_vp3071_config),
								/* VP-3071 */
	MAKE_ENTRY(TWINHAN_TECHNOLOGIES, TWINHAN_VP_6002, SAA7160, &saa716x_vp6002_config),
								/* VP-6002 */
	MAKE_ENTRY(KNC_One, KNC_Dual_S2, SAA7160, &saa716x_knc1_duals2_config),
	MAKE_ENTRY(TECHNISAT, SKYSTAR2_EXPRESS_HD, SAA7160, &skystar2_express_hd_config),
	MAKE_ENTRY(TURBOSIGHT_TBS6284, TBS6284, SAA7160, &saa716x_tbs6284_config),
	MAKE_ENTRY(TURBOSIGHT_TBS6280, TBS6280, SAA7160, &saa716x_tbs6280_config),
	MAKE_ENTRY(TURBOSIGHT_TBS6281, TBS6281, SAA7160, &saa716x_tbs6281_config),
	MAKE_ENTRY(TURBOSIGHT_TBS6285, TBS6285, SAA7160, &saa716x_tbs6285_config),
	MAKE_ENTRY(TURBOSIGHT_TBS6220, TBS6220, SAA7160, &saa716x_tbs6220_config),
	MAKE_ENTRY(TURBOSIGHT_TBS6221, TBS6221, SAA7160, &saa716x_tbs6221_config),
	MAKE_ENTRY(TURBOSIGHT_TBS6925, TBS6925, SAA7160, &saa716x_tbs6925_config),
	MAKE_ENTRY(TURBOSIGHT_TBS7220, TBS7220, SAA7160, &saa716x_tbs7220_config),
	MAKE_ENTRY(TURBOSIGHT_TBS6290, TBS6290, SAA7160, &saa716x_tbs6290_config),
	{ }
};
MODULE_DEVICE_TABLE(pci, saa716x_budget_pci_table);

static struct pci_driver saa716x_budget_pci_driver = {
	.name			= DRIVER_NAME,
	.id_table		= saa716x_budget_pci_table,
	.probe			= saa716x_budget_pci_probe,
	.remove			= saa716x_budget_pci_remove,
};

module_pci_driver(saa716x_budget_pci_driver);

MODULE_DESCRIPTION("SAA716x Budget driver");
MODULE_AUTHOR("Manu Abraham");
MODULE_LICENSE("GPL");
