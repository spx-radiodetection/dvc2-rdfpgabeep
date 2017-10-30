#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/pci.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/i2c.h>

#define	RDFPGABEEP_NAME		"rdfpgabeep"

#define MAX_DURATION 2550	/* in milliseconds */
#define MAX_FREQUENCY 8192	/* in Hz */

MODULE_AUTHOR("James Covey-Crump <james.covey-crump@spx.com>");
MODULE_DESCRIPTION("I2C Beeper Driver for RD DVC2 FPGA");
MODULE_VERSION("0.1");
MODULE_LICENSE("GPL");

#ifdef GIT_REVISION
MODULE_INFO(gitrev, GIT_REVISION);
#endif

static int suppress_i2c;
module_param(suppress_i2c, int, 0644);
MODULE_PARM_DESC(suppress_i2c, " set to non-zero to suppress I2C traffic");

struct rdfpgabeep_data {
	struct i2c_client *i2c_client;
	unsigned int frequency;		/* in Hz */
	unsigned int duration_ms;	/* in milliseconds */
	int i2c_failures;
	bool muted;
};

static bool rdfpgabeep_i2cwrite(struct rdfpgabeep_data *p, char *buf, int size)
{
	struct device *dev = &p->i2c_client->dev;

	if (suppress_i2c) {
		pr_debug("%s: suppressed i2c write to 0x%02x\n",
			 __func__, p->i2c_client->addr);
	} else {
		if (i2c_master_send(p->i2c_client, buf, size) != size)
			p->i2c_failures++;
		else
			p->i2c_failures = 0;
	}

	/* limit the number of error messages */
	if (p->i2c_failures > 1 && p->i2c_failures <= 5) {
		dev_warn(dev, "%s: i2c write failed for address 0x%02x\n",
			 __func__, p->i2c_client->addr);
	}

	return (p->i2c_failures == 0);
}

static bool rdfpgabeep_sound_beep(struct rdfpgabeep_data *p,
				  unsigned int freq, unsigned int duration_ms)
{
	unsigned int period = 100000/freq;
	unsigned int duration = duration_ms/10;
	char buf[3];

	if (period > 65535)
		period = 65535;

	if (duration > 255)
		duration = 255;

	buf[0] = (char) (period / 256);
	buf[1] = (char) (period % 256);
	buf[2] = (char) (duration);

	/* allow people to silence the buzzer with a zero duration */
	if (duration_ms == 0 || p->muted)
		return true;
	else
		return (rdfpgabeep_i2cwrite(p, buf, sizeof(buf)));
}

static bool frequency_valid(int frequency)
{
	return (frequency > 0 && frequency <= MAX_FREQUENCY);
}

static ssize_t frequency_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buffer, size_t count)
{
	struct rdfpgabeep_data *p = dev_get_drvdata(dev);
	unsigned int param1;

	if (kstrtouint(buffer, 0, &param1) == 0
	    && frequency_valid(param1)) {
		p->frequency = param1;
		return count;
	}

	return -EINVAL;
}

static ssize_t frequency_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buffer)
{
	struct rdfpgabeep_data *p = dev_get_drvdata(dev);

	return scnprintf(buffer, 20, "%d\n", (int)p->frequency);
}

static bool duration_ms_valid(int duration_ms)
{
	return (duration_ms >= 0 && duration_ms <= MAX_DURATION);
}

static ssize_t duration_ms_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buffer, size_t count)
{
	struct rdfpgabeep_data *p = dev_get_drvdata(dev);
	unsigned int param1;

	if (kstrtouint(buffer, 0, &param1) == 0
	    && duration_ms_valid(param1)) {
		p->duration_ms = param1;
		return count;
	}

	return -EINVAL;
}

static ssize_t duration_ms_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buffer)
{
	struct rdfpgabeep_data *p = dev_get_drvdata(dev);

	return scnprintf(buffer, 20, "%u\n", (unsigned int)p->duration_ms);
}

static ssize_t beep_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buffer, size_t count)
{
	struct rdfpgabeep_data *p = dev_get_drvdata(dev);
	unsigned int param1, param2;
	int num_ret;

	num_ret = sscanf(buffer, "%u%u", &param1, &param2);

	/* (for convenience short cut to avoid having to do 3 sysfs writes)
	 * two incoming parameters are treated as frequency, duration.
	 */
	if (num_ret == 2
	    && frequency_valid(param1) && duration_ms_valid(param2)) {
		p->frequency = param1;
		p->duration_ms = param2;
	}

	dev_dbg(dev, "Beeper %u %u\n", p->frequency, p->duration_ms);
	rdfpgabeep_sound_beep(p, p->frequency, p->duration_ms);

	return count;
}

static ssize_t muted_show(struct device *dev,
			 struct device_attribute *attr,
			 char *buffer)
{
	struct rdfpgabeep_data *p = dev_get_drvdata(dev);

	return scnprintf(buffer, 20, "%d\n", (int)p->muted);
}

static ssize_t muted_store(struct device *dev,
			  struct device_attribute *attr,
			  const char *buffer, size_t count)
{
	struct rdfpgabeep_data *p = dev_get_drvdata(dev);
	unsigned int param1;

	if (kstrtouint(buffer, 0, &param1) == 0)
		p->muted = param1;

	return count;
}

/* Attach the sysfs access methods */
DEVICE_ATTR_RW(frequency);	/* frequency_store() / frequency show() */
DEVICE_ATTR_RW(duration_ms);	/* duration_ms_store() / duration_ms_show() */
DEVICE_ATTR_RW(muted);		/* muted_store() / muted_show() */
DEVICE_ATTR_WO(beep);		/* beeper_store() */

/* Attribute Descriptor */
static struct attribute *rdfpgabeep_sysfs_attrs[] = {
	&dev_attr_frequency.attr,
	&dev_attr_duration_ms.attr,
	&dev_attr_muted.attr,
	&dev_attr_beep.attr,
	NULL
};

/* Attribute group */
static struct attribute_group rdfpgabeep_attr_group = {
	.attrs = rdfpgabeep_sysfs_attrs,
};

/* Driver Initialisation */
static int rdfpgabeep_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct rdfpgabeep_data *p;
	struct device *dev = &client->dev;
	struct device_node *np = dev->of_node;

	int error;

	p = devm_kzalloc(dev, sizeof(struct rdfpgabeep_data), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	/* defaults */
	p->frequency = 440;
	p->duration_ms = 1000;
	p->muted = false;

	if (np) {
		u32 v;

		p->muted = of_property_read_bool(np, "muted");

		if (of_property_read_u32(np, "frequency", &v) == 0)
			p->frequency = (unsigned int)v;

		if (of_property_read_u32(np, "duration_ms", &v) == 0)
			p->duration_ms = (unsigned int)v;

	}

	/* Create a sysfs node to read simulated coordinates */
	error = sysfs_create_group(&dev->kobj, &rdfpgabeep_attr_group);
	if (error) {
		dev_err(dev, "Unable create sysfs entry\n");
		return error;
	}

	p->i2c_client = client;
	i2c_set_clientdata(client, p);

	dev_info(dev, "RD FPGA Beep Driver Initialised\n");
	return 0;
}

/* Driver Exit */
static int rdfpgabeep_remove(struct i2c_client *client)
{
	struct device *dev = &client->dev;

	/* Cleanup sysfs node */
	sysfs_remove_group(&dev->kobj, &rdfpgabeep_attr_group);

	dev_info(dev, "RD FPGA Beep Driver Removed\n");
	return 0;
}

static const struct i2c_device_id rdfpgabeep_id[] = {
	{ RDFPGABEEP_NAME, 0001, },
	{ }
};
MODULE_DEVICE_TABLE(i2c, rdfpgabeep_id);

static const struct of_device_id rdfpgabeep_dt_ids[] = {
	{ .compatible = "rd,rdfpgabeep", },
	{ }
};
MODULE_DEVICE_TABLE(of, rdfpgabeep_dt_ids);

static struct i2c_driver rdfpgabeep_driver = {
	.driver = {
		.name	= RDFPGABEEP_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(rdfpgabeep_dt_ids),
	},
	.probe		= rdfpgabeep_probe,
	.remove         = rdfpgabeep_remove,
	.id_table	= rdfpgabeep_id,
};

static int __init rdfpgabeep_init(void)
{
	return i2c_add_driver(&rdfpgabeep_driver);
}
subsys_initcall(rdfpgabeep_init);

static void __exit rdfpgabeep_exit(void)
{
	i2c_del_driver(&rdfpgabeep_driver);
}
module_exit(rdfpgabeep_exit);
