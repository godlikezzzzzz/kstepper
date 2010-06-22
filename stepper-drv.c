/*
 *		Stepper motor driver over gpio with pwm support
 *
 *		Copyright (R) 2010 Claudio Mignanti - <c.mignanti@gmail.com>
 *
 *		This program is free software; you can redistribute it and/or modify
 *		it under the terms of the GNU General Public License as published by
 *		the Free Software Foundation.
 *
 * $Id$
 * ---------------------------------------------------------------------------
*/

#include <linux/init.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/pwm/pwm.h>

#include <asm/ioctl.h>

#include "motor.h"

#define DRV_NAME	"stepper-drv"
#define DRV_DESC	"Stepper motor driver using gpio and pwm pins"
#define DRV_VERSION	"0.0.1"

#define MAX_MOT_NUM 4

/* module var*/
struct class *motor_class;
static dev_t motor_devno = 0;
int steps_max[MAX_MOT_NUM] = {0}, steps[MAX_MOT_NUM] = {0};
struct pwm_channel *pwmc[MAX_MOT_NUM];

/* module parameters */
char *motor_name = NULL;
int g_enable[MAX_MOT_NUM] = {0} , g_dir[MAX_MOT_NUM] = {0}, \
		use_pwm[MAX_MOT_NUM] = {0}, g_step[MAX_MOT_NUM] = {0}, \
		g_lpwr[MAX_MOT_NUM] = {0};


static unsigned int mot0[6] __initdata;
static unsigned int mot1[6] __initdata;
static unsigned int mot2[6] __initdata;
static unsigned int mot3[6] __initdata;

static unsigned int mot_nump[MAX_MOT_NUM] __initdata;

static int mot_map[MAX_MOT_NUM] = {0};
static int mot_map_pwm[MAX_MOT_NUM] = {0};

#define BUS_PARM_DESC \
	" config -> id,en,dir,step[,lowpwr]"

module_param_array(mot0, uint, &mot_nump[0], 0);
MODULE_PARM_DESC(mot0, "mot0" BUS_PARM_DESC);
module_param_array(mot1, uint, &mot_nump[1], 0);
MODULE_PARM_DESC(mot1, "mot1" BUS_PARM_DESC);
module_param_array(mot2, uint, &mot_nump[2], 0);
MODULE_PARM_DESC(mot2, "mot2" BUS_PARM_DESC);
module_param_array(mot3, uint, &mot_nump[3], 0);
MODULE_PARM_DESC(mot3, "mot3" BUS_PARM_DESC);


static int motor_pwm_set(struct pwm_channel *pwmc, unsigned long up, unsigned long period ) {
	struct pwm_channel_config cfg;
	if (up == 0 && period == 0) {
		up = 1000UL;
		period = 1000UL;
	}

	cfg.config_mask = PWM_CONFIG_DUTY_NS
		| PWM_CONFIG_PERIOD_NS;

	cfg.duty_ns = up * 1000000UL;
	cfg.period_ns = period * 1000000UL;

	return pwm_config(pwmc, &cfg);
}

static int cdev_to_id (struct cdev* cdev) {
	int i;
	for (i=0; i < MAX_MOT_NUM; i++) {
		if (cdev == (struct cdev*) (mot_map[i]))
			return i;
	}
	return 0;
}

static int pwmc_to_id (struct pwm_channel *ch) {
	int i;
	for (i=0; i < MAX_MOT_NUM; i++) {
		if (ch == (struct pwm_channel*) (mot_map_pwm[i]))
			return i;
	}
	return 0;
}

/* IRQ handler */
static void irq_steps_handler (struct pwm_channel *ch) {
	int id = pwmc_to_id (ch);

	steps[id]++;
	if (steps[id] >= steps_max[id])
		pwm_stop(ch);
}

/* IOCTL interface */
static int motor_ioctl (struct inode *in, struct file *fl, unsigned int cmd, unsigned long arg) {

	int retval = 0, id;
	struct cdev* p = in->i_cdev;

	id = cdev_to_id (p);

	switch (cmd) {
		case MOTOR_ENABLE:
			if ((int)arg)
				gpio_set_value (g_enable[id], 0);
			else
				gpio_set_value (g_enable[id], 1);
			break;

		case MOTOR_DIR:
			if ((int)arg)
				gpio_set_value (g_dir[id], 1);
			else
				gpio_set_value (g_dir[id], 0);
			break;

		case MOTOR_PWM_ON:
			pwm_start (pwmc[id]);
			break;

		case MOTOR_PWM_OFF:
			pwm_stop (pwmc[id]);
			break;

		case MOTOR_PWM_SET:
			//set the pwm period in ms
			motor_pwm_set (pwmc[id], (int)arg<<2, (int)arg );
			break;

		case MOTOR_STEPS_RESET:
			steps[id] = 0; /* set the actual position as home */
			break;

		case MOTOR_STEPS_MAX:
			steps_max[id] = (int) arg; /* set the steps limit */
			break;

		case MOTOR_STEPS_ENABLE:
			if (g_step[id] == 0) {
			} else {
				if ((int)arg)
					retval = pwm_set_handler (pwmc[id], &irq_steps_handler, NULL);
				else
					retval = pwm_set_handler (pwmc[id], NULL, NULL);
			}
			break;

		case MOTOR_LOWPWR:
			if ((int)arg)
				gpio_set_value (g_lpwr[id], 1);
			else
				gpio_set_value (g_lpwr[id], 0);
			break;

		default:
			retval = -EINVAL;
	}

	return retval;
}

struct file_operations motor_fops = {
	.owner = THIS_MODULE,
	.ioctl = motor_ioctl,
};

static int motor_add_one(unsigned int id, unsigned int *params)
{
	int status;
	struct cdev *motor_cdev;

	if ( mot_nump[id] < 4 ) {
		printk(KERN_INFO "stepper: nothing to register for id: %d.\n", id);
		return 0;
	}

	g_enable[id] = params[1];
	g_dir[id] = params[2];
	g_step[id] = params[3];
	g_lpwr[id] = params[4];
	use_pwm[id] = params[5];

	/* sanity check */
	if ( !( g_enable[id] && g_dir[id] && g_step[id])) {
		printk(KERN_INFO "stepper: missing parameters, exit driver.\n");
		goto err_para;
	}
	printk (KERN_INFO "stepper: B.\n");

	/* request and set pwm channel and gpio pins */
	pwmc[id] = pwm_request("gpio_pwm", g_step[id], "stepper");
	if (IS_ERR(pwmc[id])) {
		goto err_pwm;
	}
	pwm_stop(pwmc[id]);

	if ( gpio_request(g_enable[id], "motor-enable") < 0 ) {
		goto err_gpioenable;
	}
	gpio_direction_output(g_enable[id] ,0);

	if ( gpio_request(g_dir[id], "motor-ccw") < 0) {
		goto err_gpiodir;
	}
	gpio_direction_output(g_dir[id] ,0);

	if (g_lpwr[id] != 0) {
		if ( gpio_request(g_lpwr[id], "motor-lowpwr") < 0 ) {
			goto err_gpiolwr;
		}
		gpio_direction_output(g_lpwr[id] ,0);
	}
		printk (KERN_INFO "stepper: D.\n");

	/* set to home */
	steps[id] = 0;

	/* alloc a new device number (major: dynamic, minor: 0) */
	status = alloc_chrdev_region(&motor_devno, 0, 1, "motor");

		printk (KERN_INFO "stepper: E.\n");
	/* create a new char device  */
	motor_cdev = cdev_alloc();
	if(motor_cdev == NULL) {
		status=-ENOMEM;
		goto err_dev;
	}

	/*save the cdev for id's */
	mot_map[id] = (int) motor_cdev;

	motor_cdev->owner = THIS_MODULE;
	motor_cdev->ops = &motor_fops;
	status = cdev_add(motor_cdev, motor_devno, 1);
	if(status){
		goto err_dev;
	}

	device_create(motor_class, NULL, motor_devno, NULL, "motor%d", params[0]);
	printk(KERN_INFO "stepper: %s registerd on major: %u; minor: %u\n", \
		motor_name, MAJOR(motor_devno), MINOR(motor_devno));

	return 0;

err_dev:
	printk(KERN_INFO "stepper: err_dev\n");
err_gpiolwr:
	printk(KERN_INFO "stepper: err_gpiolwr\n");
err_gpiodir:
	printk(KERN_INFO "stepper: err_gpiodir\n");
err_gpioenable:
	printk(KERN_INFO "stepper: err_gpioenable\n");
err_gpiostep:
	printk(KERN_INFO "stepper: err_gpiostep ");
err_pwm:
	printk(KERN_INFO "stepper: err_pwm\n");
err_para:
	printk(KERN_INFO "stepper: Error management not yet implemented. \
		Please reboot your board %d\n",g_step[id]);
	return -1;
}

static int __init motor_init(void)
{
	int err;

	printk(KERN_INFO DRV_DESC ", version " DRV_VERSION "\n");

	/* register the class *
	motor_class = class_create(THIS_MODULE, "motor_class");
	if(IS_ERR(motor_class)){
		goto err;
	}*/

	err = motor_add_one(0, mot0);
	if (err) goto err;

	err = motor_add_one(1, mot1);
	if (err) goto err;

	err = motor_add_one(2, mot2);
	if (err) goto err;

	err = motor_add_one(3, mot3);
	if (err) goto err;

	return 0;

err:
	class_unregister(motor_class);
	return -1;
}

static void __exit motor_exit(void)
{
	//int i;

	class_unregister(motor_class);

	/*free the gpio_pins*/
//	for (i=0; i < 
}

module_init(motor_init);
module_exit(motor_exit);

MODULE_AUTHOR("Claudio Mignanti <c.mignanti@gmail.com>");
MODULE_DESCRIPTION("Stepper motor driver over gpio with pwm support");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:motor-pwm");

