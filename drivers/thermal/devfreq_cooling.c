/*
 * devfreq_cooling: Thermal cooling device implementation for devices using
 *                  devfreq
 *
 * Copyright (C) 2014-2015 ARM Limited
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * TODO:
 *    - If OPPs are added or removed after devfreq cooling has
 *      registered, the devfreq cooling won't react to it.
 */

#include <linux/devfreq.h>
#include <linux/devfreq_cooling.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/pm_opp.h>
#include <linux/thermal.h>
#include <trace/events/thermal.h>
#ifdef CONFIG_HISI_IPA_THERMAL
#include <trace/events/thermal_power_allocator.h>
#ifdef CONFIG_HISI_THERMAL_SPM
unsigned int gpu_profile_freq;
extern int get_profile_power(enum ipa_actor actor,
					unsigned int *power);
unsigned long hisi_calc_gpu_static_power(unsigned long voltage,
					int temperature);
int get_soc_target_temp(struct thermal_cooling_device *cdev,
					int *target_temp);
#endif
extern unsigned int g_ipa_freq_limit[];
extern unsigned int g_ipa_soc_freq_limit[];
extern unsigned int g_ipa_board_freq_limit[];
extern unsigned int g_ipa_board_state[];
extern unsigned int g_ipa_soc_state[];
extern int update_devfreq(struct devfreq *devfreq);
#endif

static DEFINE_MUTEX(devfreq_lock);
static DEFINE_IDR(devfreq_idr);

/**
 * struct devfreq_cooling_device - Devfreq cooling device
 * @id:		unique integer value corresponding to each
 *		devfreq_cooling_device registered.
 * @cdev:	Pointer to associated thermal cooling device.
 * @devfreq:	Pointer to associated devfreq device.
 * @cooling_state:	Current cooling state.
 * @power_table:	Pointer to table with maximum power draw for each
 *			cooling state. State is the index into the table, and
 *			the power is in mW.
 * @freq_table:	Pointer to a table with the frequencies sorted in descending
 *		order.  You can index the table by cooling device state
 * @freq_table_size:	Size of the @freq_table and @power_table
 * @power_ops:	Pointer to devfreq_cooling_power, used to generate the
 *		@power_table.
 */
struct devfreq_cooling_device {
	int id;
	struct thermal_cooling_device *cdev;
	struct devfreq *devfreq;
	unsigned long cooling_state;
	u32 *power_table;
	u32 *freq_table;
	size_t freq_table_size;
	struct devfreq_cooling_power *power_ops;
};

/**
 * get_idr - function to get a unique id.
 * @idr: struct idr * handle used to create a id.
 * @id: int * value generated by this function.
 *
 * This function will populate @id with an unique
 * id, using the idr API.
 *
 * Return: 0 on success, an error code on failure.
 */
static int get_idr(struct idr *idr, int *id)
{
	int ret;

	mutex_lock(&devfreq_lock);
	ret = idr_alloc(idr, NULL, 0, 0, GFP_KERNEL);
	mutex_unlock(&devfreq_lock);
	if (unlikely(ret < 0))
		return ret;
	*id = ret;

	return 0;
}

/**
 * release_idr - function to free the unique id.
 * @idr: struct idr * handle used for creating the id.
 * @id: int value representing the unique id.
 */
static void release_idr(struct idr *idr, int id)
{
	mutex_lock(&devfreq_lock);
	idr_remove(idr, id);
	mutex_unlock(&devfreq_lock);
}

#ifndef CONFIG_HISI_IPA_THERMAL
/**
 * partition_enable_opps() - disable all opps above a given state
 * @dfc:	Pointer to devfreq we are operating on
 * @cdev_state:	cooling device state we're setting
 *
 * Go through the OPPs of the device, enabling all OPPs until
 * @cdev_state and disabling those frequencies above it.
 */
static int partition_enable_opps(struct devfreq_cooling_device *dfc,
				 unsigned long cdev_state)
{
	int i;
	struct device *dev = dfc->devfreq->dev.parent;

	for (i = 0; i < dfc->freq_table_size; i++) {
		struct dev_pm_opp *opp;
		int ret = 0;
		unsigned int freq = dfc->freq_table[i];
		bool want_enable = i >= cdev_state ? true : false;

		rcu_read_lock();
		opp = dev_pm_opp_find_freq_exact(dev, freq, !want_enable);
		rcu_read_unlock();

		if (PTR_ERR(opp) == -ERANGE)
			continue;
		else if (IS_ERR(opp))
			return PTR_ERR(opp);

		if (want_enable)
			ret = dev_pm_opp_enable(dev, freq);
		else
			ret = dev_pm_opp_disable(dev, freq);

		if (ret)
			return ret;
	}

	return 0;
}
#endif

static int devfreq_cooling_get_max_state(struct thermal_cooling_device *cdev,
					 unsigned long *state)
{
	struct devfreq_cooling_device *dfc = cdev->devdata;

	*state = dfc->freq_table_size - 1;

	return 0;
}

static int devfreq_cooling_get_cur_state(struct thermal_cooling_device *cdev,
					 unsigned long *state)
{
	struct devfreq_cooling_device *dfc = cdev->devdata;

	*state = dfc->cooling_state;

	return 0;
}

static int devfreq_cooling_set_cur_state(struct thermal_cooling_device *cdev,
					 unsigned long state)
{
	struct devfreq_cooling_device *dfc = cdev->devdata;
	struct devfreq *df = dfc->devfreq;
	struct device *dev = df->dev.parent;
#ifdef CONFIG_HISI_IPA_THERMAL
	unsigned long freq;
	unsigned long limit_state;
#endif
	int ret;

#ifdef CONFIG_HISI_IPA_THERMAL
	if(g_ipa_soc_state[IPA_GPU] < dfc->freq_table_size)
		g_ipa_soc_freq_limit[IPA_GPU] = dfc->freq_table[g_ipa_soc_state[IPA_GPU]];

	if(g_ipa_board_state[IPA_GPU] < dfc->freq_table_size)
		g_ipa_board_freq_limit[IPA_GPU] = dfc->freq_table[g_ipa_board_state[IPA_GPU]];

	limit_state = max(g_ipa_soc_state[IPA_GPU], g_ipa_board_state[IPA_GPU]);/*lint !e1058*/
	if(limit_state < dfc->freq_table_size)
		state = max(state, limit_state);
#endif

	if (state == dfc->cooling_state)
		return 0;

	dev_dbg(dev, "Setting cooling state %lu\n", state);

#ifdef CONFIG_HISI_IPA_THERMAL
	if (state == THERMAL_NO_LIMIT) {
		freq = 0;
	} else {
		if (state >= dfc->freq_table_size)
			return -EINVAL;

		freq = dfc->freq_table[state];
	}

	g_ipa_freq_limit[IPA_GPU] = freq;
	trace_IPA_actor_gpu_cooling(freq/1000, state);

	if (df->max_freq != freq) {
		/*NOTE use devfreq_qos_set_max,because gpufreq not support VOTE */
		mutex_lock(&df->lock);
		ret = update_devfreq(df);
		mutex_unlock(&df->lock);
		if (ret)
			dev_dbg(dev, "update devfreq fail %d\n", ret);
	}
#else
	if (state >= dfc->freq_table_size)
		return -EINVAL;

	ret = partition_enable_opps(dfc, state);
	if (ret)
		return ret;
#endif

	dfc->cooling_state = state;

	return 0;
}

/**
 * freq_get_state() - get the cooling state corresponding to a frequency
 * @dfc:	Pointer to devfreq cooling device
 * @freq:	frequency in Hz
 *
 * Return: the cooling state associated with the @freq, or
 * THERMAL_CSTATE_INVALID if it wasn't found.
 */
static unsigned long
freq_get_state(struct devfreq_cooling_device *dfc, unsigned long freq)
{
	int i;

	for (i = 0; i < dfc->freq_table_size; i++) {/*lint !e574*/
		if (dfc->freq_table[i] == freq)
			return i; /* [false alarm]:fortify */
	}

	return THERMAL_CSTATE_INVALID;/*lint !e501*/
}

/**
 * get_static_power() - calculate the static power
 * @dfc:	Pointer to devfreq cooling device
 * @freq:	Frequency in Hz
 *
 * Calculate the static power in milliwatts using the supplied
 * get_static_power().  The current voltage is calculated using the
 * OPP library.  If no get_static_power() was supplied, assume the
 * static power is negligible.
 */
static unsigned long
get_static_power(struct devfreq_cooling_device *dfc, unsigned long freq)
{
	struct devfreq *df = dfc->devfreq;
	struct device *dev = df->dev.parent;
	unsigned long voltage;
	struct dev_pm_opp *opp;

	if (!dfc->power_ops->get_static_power)
		return 0;

	rcu_read_lock();

	opp = dev_pm_opp_find_freq_exact(dev, freq, true);
	if (IS_ERR(opp) && (PTR_ERR(opp) == -ERANGE))
		opp = dev_pm_opp_find_freq_exact(dev, freq, false);

	voltage = dev_pm_opp_get_voltage(opp) / 1000; /* mV */

	rcu_read_unlock();

	if (voltage == 0) {
		dev_warn_ratelimited(dev,
				     "Failed to get voltage for frequency %lu: %ld\n",
				     freq, IS_ERR(opp) ? PTR_ERR(opp) : 0);
		return 0;
	}

	return dfc->power_ops->get_static_power(voltage);
}

/**
 * get_dynamic_power - calculate the dynamic power
 * @dfc:	Pointer to devfreq cooling device
 * @freq:	Frequency in Hz
 * @voltage:	Voltage in millivolts
 *
 * Calculate the dynamic power in milliwatts consumed by the device at
 * frequency @freq and voltage @voltage.  If the get_dynamic_power()
 * was supplied as part of the devfreq_cooling_power struct, then that
 * function is used.  Otherwise, a simple power model (Pdyn = Coeff *
 * Voltage^2 * Frequency) is used.
 */
static unsigned long
get_dynamic_power(struct devfreq_cooling_device *dfc, unsigned long freq,
		  unsigned long voltage)
{
	u64 power;
	u32 freq_mhz;
	struct devfreq_cooling_power *dfc_power = dfc->power_ops;

	if (dfc_power->get_dynamic_power)
		return dfc_power->get_dynamic_power(freq, voltage);

	freq_mhz = freq / 1000000;
	power = (u64)dfc_power->dyn_power_coeff * freq_mhz * voltage * voltage;
	do_div(power, 1000000000);

	return power;
}

static int devfreq_cooling_get_requested_power(struct thermal_cooling_device *cdev,
					       struct thermal_zone_device *tz,
					       u32 *power)
{
	struct devfreq_cooling_device *dfc = cdev->devdata;
	struct devfreq *df = dfc->devfreq;
	struct devfreq_dev_status *status = &df->last_status;
	unsigned long state;
	unsigned long freq = status->current_frequency;
	u32 dyn_power, static_power;
#ifdef CONFIG_HISI_IPA_THERMAL
	unsigned long load = 0;
#endif

	/* Get dynamic power for state */
	state = freq_get_state(dfc, freq);
	if (state == THERMAL_CSTATE_INVALID)/*lint !e501*/
		return -EAGAIN;

	dyn_power = dfc->power_table[state];

	/* Scale dynamic power for utilization */
#ifdef CONFIG_HISI_IPA_THERMAL
	if (status->total_time)
#endif
		dyn_power = (dyn_power * status->busy_time) / status->total_time;

	/* Get static power */
	static_power = get_static_power(dfc, freq);

	trace_thermal_power_devfreq_get_power(cdev, status, freq, dyn_power,
					      static_power);

	*power = dyn_power + static_power;

#ifdef CONFIG_HISI_IPA_THERMAL
	if(status->total_time)
		load = 100* status->busy_time /status->total_time;
	if (tz->is_soc_thermal)
		trace_IPA_actor_gpu_get_power((freq/1000), load, dyn_power, static_power, *power);
#endif

#ifdef CONFIG_HISI_IPA_THERMAL
	cdev->current_load = load;
	cdev->current_freq = freq;
#endif

	return 0;
}

static int devfreq_cooling_state2power(struct thermal_cooling_device *cdev,
				       struct thermal_zone_device *tz,
				       unsigned long state,
				       u32 *power)
{
	struct devfreq_cooling_device *dfc = cdev->devdata;
	unsigned long freq;
	u32 static_power;

	if (state >= dfc->freq_table_size)
		return -EINVAL;

	freq = dfc->freq_table[state];
	static_power = get_static_power(dfc, freq);

	*power = dfc->power_table[state] + static_power;
	return 0;
}

static int devfreq_cooling_power2state(struct thermal_cooling_device *cdev,
				       struct thermal_zone_device *tz,
				       u32 power, unsigned long *state)
{
	struct devfreq_cooling_device *dfc = cdev->devdata;
	struct devfreq *df = dfc->devfreq;
	struct devfreq_dev_status *status = &df->last_status;
	unsigned long freq = status->current_frequency;
	unsigned long busy_time;
	s32 dyn_power;
	u32 static_power;
	int i;

	static_power = get_static_power(dfc, freq);

	dyn_power = power - static_power;
	dyn_power = dyn_power > 0 ? dyn_power : 0;

	/* Scale dynamic power for utilization */
	busy_time = status->busy_time ?: 1;
	dyn_power = (dyn_power * status->total_time) / busy_time;

	/*
	 * Find the first cooling state that is within the power
	 * budget for dynamic power.
	 */
	for (i = 0; i < dfc->freq_table_size - 1; i++)/*lint !e574*/
		if (dyn_power >= dfc->power_table[i])/*lint !e574*/
			break;

	*state = i;

	trace_thermal_power_devfreq_limit(cdev, freq, *state, power);
#ifdef CONFIG_HISI_IPA_THERMAL
	trace_IPA_actor_gpu_limit(freq/1000, *state, power);
#endif
	return 0;
}

static struct thermal_cooling_device_ops devfreq_cooling_ops = {
	.get_max_state = devfreq_cooling_get_max_state,
	.get_cur_state = devfreq_cooling_get_cur_state,
	.set_cur_state = devfreq_cooling_set_cur_state,
};

/**
 * devfreq_cooling_gen_tables() - Generate power and freq tables.
 * @dfc: Pointer to devfreq cooling device.
 *
 * Generate power and frequency tables: the power table hold the
 * device's maximum power usage at each cooling state (OPP).  The
 * static and dynamic power using the appropriate voltage and
 * frequency for the state, is acquired from the struct
 * devfreq_cooling_power, and summed to make the maximum power draw.
 *
 * The frequency table holds the frequencies in descending order.
 * That way its indexed by cooling device state.
 *
 * The tables are malloced, and pointers put in dfc.  They must be
 * freed when unregistering the devfreq cooling device.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int devfreq_cooling_gen_tables(struct devfreq_cooling_device *dfc)
{
	struct devfreq *df = dfc->devfreq;
	struct device *dev = df->dev.parent;
	int ret, num_opps;
	unsigned long freq;
	u32 *power_table = NULL;
	u32 *freq_table;
	int i;
#ifdef CONFIG_HISI_IPA_THERMAL
	unsigned long power_static;
#endif

	num_opps = dev_pm_opp_get_opp_count(dev);

	if (dfc->power_ops) {
		power_table = kcalloc(num_opps, sizeof(*power_table),
				      GFP_KERNEL);
		if (!power_table)
			return -ENOMEM;
	}

	freq_table = kcalloc(num_opps, sizeof(*freq_table),
			     GFP_KERNEL);
	if (!freq_table) {
		ret = -ENOMEM;
		goto free_power_table;
	}

	for (i = 0, freq = ULONG_MAX; i < num_opps; i++, freq--) {
		unsigned long power_dyn, voltage;
		struct dev_pm_opp *opp;

		rcu_read_lock();

		opp = dev_pm_opp_find_freq_floor(dev, &freq);
		if (IS_ERR(opp)) {
			rcu_read_unlock();
			ret = PTR_ERR(opp);
			goto free_tables;
		}

		voltage = dev_pm_opp_get_voltage(opp) / 1000; /* mV */

		rcu_read_unlock();

		if (dfc->power_ops) {
			power_dyn = get_dynamic_power(dfc, freq, voltage);

#ifdef CONFIG_HISI_IPA_THERMAL
			power_static = get_static_power(dfc, freq);
			pr_debug("%lu MHz @ %lu mV: %lu + %lu = %lu mW\n",
					freq / 1000000, voltage,
					power_dyn, power_static, power_dyn + power_static);
#else
			dev_dbg(dev, "Dynamic power table: %lu MHz @ %lu mV: %lu = %lu mW\n",
				freq / 1000000, voltage, power_dyn, power_dyn);
#endif

			power_table[i] = power_dyn; /* [false alarm]:fortify *//*lint !e613*/
		}

		freq_table[i] = freq;
	}

	if (dfc->power_ops)
		dfc->power_table = power_table;

	dfc->freq_table = freq_table;
	dfc->freq_table_size = num_opps;

	return 0;/*lint !e593*/

free_tables:
	kfree(freq_table);
free_power_table:
	kfree(power_table);

	return ret;
}

#ifdef CONFIG_HISI_THERMAL_SPM
unsigned int get_profile_gpu_freq(void)
{
	return gpu_profile_freq;
}
EXPORT_SYMBOL(get_profile_gpu_freq);

static int devfreq_freq2volt(struct devfreq_cooling_device *dfc, unsigned long freq,
				unsigned long *voltage)
{
	struct devfreq *df = dfc->devfreq;
	struct device *dev = df->dev.parent;
	struct dev_pm_opp *opp;

	rcu_read_lock();

	opp = dev_pm_opp_find_freq_exact(dev, freq, (bool)true);
	if (IS_ERR(opp) && (PTR_ERR(opp) == -ERANGE))
		opp = dev_pm_opp_find_freq_exact(dev, freq, (bool)false);

	*voltage = dev_pm_opp_get_voltage(opp) / 1000; /* mV */

	rcu_read_unlock();

	if (*voltage == 0) {
		/*lint -e64 -e570 -e785 -esym(64,570,785,*)*/
		dev_warn_ratelimited(dev,
				     "Failed to get voltage for frequency %lu: %ld\n",
				     freq, IS_ERR(opp) ? PTR_ERR(opp) : 0);
		/*lint -e64 -e570 -e785 +esym(64,570,785,*)*/
	}

	return 0;
}

static int devfreq_power2freq(struct thermal_cooling_device *cdev, u32 power, u32 *freq)
{
	int target_temp = 0;
	struct devfreq_cooling_device *dfc = cdev->devdata;
	int i, ret;
	unsigned long voltage = 0;
	unsigned long static_power;

	ret = get_soc_target_temp(cdev, &target_temp);
	if (ret)
		return ret;

	for (i = 0; i < (int)dfc->freq_table_size - 1; i++) {
		devfreq_freq2volt(dfc, (unsigned long)dfc->freq_table[i], &voltage);
		static_power = hisi_calc_gpu_static_power(voltage, target_temp);
		if (power >= (dfc->power_table[i] + static_power))
			break;
	}
	*freq = dfc->freq_table[i];

	return 0;
}
#endif

/**
 * of_devfreq_cooling_register_power() - Register devfreq cooling device,
 *                                      with OF and power information.
 * @np:	Pointer to OF device_node.
 * @df:	Pointer to devfreq device.
 * @dfc_power:	Pointer to devfreq_cooling_power.
 *
 * Register a devfreq cooling device.  The available OPPs must be
 * registered on the device.
 *
 * If @dfc_power is provided, the cooling device is registered with the
 * power extensions.  For the power extensions to work correctly,
 * devfreq should use the simple_ondemand governor, other governors
 * are not currently supported.
 */
struct thermal_cooling_device *
of_devfreq_cooling_register_power(struct device_node *np, struct devfreq *df,
				  struct devfreq_cooling_power *dfc_power)
{
	struct thermal_cooling_device *cdev;
	struct devfreq_cooling_device *dfc;
	char dev_name[THERMAL_NAME_LENGTH];/*lint !e578*/
	int err;
#ifdef CONFIG_HISI_THERMAL_SPM
	u32 power;
#endif

	dfc = kzalloc(sizeof(*dfc), GFP_KERNEL);
	if (!dfc)
		return ERR_PTR(-ENOMEM);

	dfc->devfreq = df;

	if (dfc_power) {
		dfc->power_ops = dfc_power;

		devfreq_cooling_ops.get_requested_power =
			devfreq_cooling_get_requested_power;
		devfreq_cooling_ops.state2power = devfreq_cooling_state2power;
		devfreq_cooling_ops.power2state = devfreq_cooling_power2state;
	}

	err = devfreq_cooling_gen_tables(dfc);
	if (err)
		goto free_dfc;

	err = get_idr(&devfreq_idr, &dfc->id);
	if (err)
		goto free_tables;

	snprintf(dev_name, sizeof(dev_name), "thermal-devfreq-%d", dfc->id);

	cdev = thermal_of_cooling_device_register(np, dev_name, dfc,
						  &devfreq_cooling_ops);
	if (IS_ERR(cdev)) {
		err = PTR_ERR(cdev);
		dev_err(df->dev.parent,
			"Failed to register devfreq cooling device (%d)\n",
			err);
		goto release_idr;
	}

	dfc->cdev = cdev;

#ifdef CONFIG_HISI_THERMAL_SPM
	err = get_profile_power(IPA_GPU, &power);

	if (err)
		goto free_dfc;

	devfreq_power2freq(cdev, power, &gpu_profile_freq);
	pr_err("IPA: GPU freq: %d\n", gpu_profile_freq);
#endif
	return cdev;

release_idr:
	release_idr(&devfreq_idr, dfc->id);
free_tables:
	kfree(dfc->power_table);
	kfree(dfc->freq_table);
free_dfc:
	kfree(dfc);

	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(of_devfreq_cooling_register_power);

/**
 * of_devfreq_cooling_register() - Register devfreq cooling device,
 *                                with OF information.
 * @np: Pointer to OF device_node.
 * @df: Pointer to devfreq device.
 */
struct thermal_cooling_device *
of_devfreq_cooling_register(struct device_node *np, struct devfreq *df)
{
	return of_devfreq_cooling_register_power(np, df, NULL);
}
EXPORT_SYMBOL_GPL(of_devfreq_cooling_register);

/**
 * devfreq_cooling_register() - Register devfreq cooling device.
 * @df: Pointer to devfreq device.
 */
struct thermal_cooling_device *devfreq_cooling_register(struct devfreq *df)
{
	return of_devfreq_cooling_register(NULL, df);
}
EXPORT_SYMBOL_GPL(devfreq_cooling_register);

/**
 * devfreq_cooling_unregister() - Unregister devfreq cooling device.
 * @dfc: Pointer to devfreq cooling device to unregister.
 */
void devfreq_cooling_unregister(struct thermal_cooling_device *cdev)
{
	struct devfreq_cooling_device *dfc;

	if (!cdev)
		return;

	dfc = cdev->devdata;

	thermal_cooling_device_unregister(dfc->cdev);
	release_idr(&devfreq_idr, dfc->id);
	kfree(dfc->power_table);
	kfree(dfc->freq_table);

	kfree(dfc);
}
EXPORT_SYMBOL_GPL(devfreq_cooling_unregister);
