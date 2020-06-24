#include <linux/slab.h>
#include <linux/bcd.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/bcd.h>
#include <linux/rtc.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <asm/errno.h>
#include <asm/delay.h>


/* Register Definitionen */
#define DS3231_REG_CONTROL	0x0e
# define DS3231_BIT_nEOSC	0x80
# define DS3231_BIT_INTCN	0x04
# define DS3231_BIT_A2IE	0x02
# define DS3231_BIT_A1IE	0x01
# define DS3231_REG_STATUS	0x0f
# define DS3231_BIT_OSF		0x80
char date[21] = {"2020-06-24 17:19:23\n"};

static int dev_open(struct inode *inode, struct file *file);
static int dev_close(struct inode *inode, struct file *file);
static ssize_t dev_read(struct file *file, char __user *puffer, size_t bytes, loff_t *offset);
static ssize_t dev_write(struct file *file, const char __user *puffer, size_t bytes, loff_t *offset);

/*
 * Der Zeiger wird bei Initialisierung gesetzt und wird für die
 * i2c Kommunikation mit dem  Device (DS3231) benötigt.
 */
static struct i2c_client *ds3231_client;
static dev_t rtc_dev;
static struct cdev rtc_cdev;
static struct class *rtc_devclass;

static struct file_operations fops = {

	.owner 		= THIS_MODULE,
//	.llseek		= no_llseek,
	.read		= dev_read,
	.write		= dev_write,
	.open		= dev_open,
	.release 	= dev_close,
};
static int dev_open(struct inode *inode, struct file *file){
	return 0;
}

static int dev_close(struct inode *inode, struct file *file){
	return 0;
}

static ssize_t dev_read(struct file *file, char __user *puffer, size_t bytes, loff_t *offset){
	int count = 0;
	while(puffer[count++] != '\0');
	if(count < 21){
		count = copy_to_user(puffer,date,21);
		return 21 - count;
	}
	return 0;
}

static ssize_t dev_write(struct file *file, const char __user *puffer, size_t bytes, loff_t *offset){
	copy_from_user(date,puffer,bytes);
	printk("dev_write called yeah\n"); 
	return bytes;
}

/*
 * Initialisierung des Treibers und Devices.
 *
 * Diese Funktion wird von Linux-Kernel aufgerufen, aber erst nachdem ein zum
 * Treiber passende Device-Information gefunden wurde. Innerhalb der Funktion
 * wird der Treiber und das Device initialisiert.
 */
static int ds3231_probe(struct i2c_client *client, const struct i2c_device_id *id)
{	
	s32 reg0, reg1;
	u8 reg_cnt, reg_sts;

	printk("DS3231_drv: ds3231_probe called\n");

	/*
	 * Control und Status Register auslesen.
	 */
	reg0 = i2c_smbus_read_byte_data(client, DS3231_REG_CONTROL);
	reg1 = i2c_smbus_read_byte_data(client, DS3231_REG_STATUS);
	if(reg0 < 0 || reg1 < 0) {
		printk("DS3231_drv: Fehler beim Lesen von Control oder Status Register.\n");
		return -ENODEV;
	}
	reg_cnt = (u8)reg0;
	reg_sts = (u8)reg1;
	printk("DS3231_drv: Control: 0x%02X, Status: 0x%02X\n", reg_cnt, reg_sts);

	/* 
	 * Prüfen ob der Oscilattor abgeschaltet ist, falls ja, muss dieser
	 * eingeschltet werden (damit die Zeit läuft).
	 */
	if (reg_cnt & DS3231_BIT_nEOSC) {
		printk("DS3231_drv: Oscilator einschalten\n");
		reg_cnt &= ~DS3231_BIT_nEOSC;
	}

	printk("DS3231_drv: Interrupt und Alarms abschalten\n");
	reg_cnt &= ~(DS3231_BIT_INTCN | DS3231_BIT_A2IE | DS3231_BIT_A1IE);

	/* Control-Register setzen */
	i2c_smbus_write_byte_data(client, DS3231_REG_CONTROL, reg_cnt);

	/*
	 * Prüfe Oscilator zustand. Falls Fehler vorhanden, wird das Fehlerfalg
	 * zurückgesetzt.
	 */
	if (reg_sts & DS3231_BIT_OSF) {
		reg_sts &= ~DS3231_BIT_OSF;
		i2c_smbus_write_byte_data(client, DS3231_REG_STATUS, reg_sts);
		printk("DS3231_drv: Oscilator Stop Flag (OSF) zurückgesetzt.\n");
	}

	/* DS3231 erfolgreich initialisiert */
	return 0;
}


/*
 * Freigebe der Resourcen.
 *
 * Diese Funktion wird beim Entfernen des Treibers oder Gerätes
 * von Linux-Kernel aufgerufen. Hier sollten die Resourcen, welche
 * in der "ds3231_probe()" Funktion angefordert wurden, freigegeben.
 */
static int ds3231_remove(struct i2c_client *client)
{
	printk("DS3231_drv: ds3231_remove called\n");
	return 0;
}


/*
 * Device-Id. Wird für die Zuordnung des Treibers zum Gerät benötigt.
 * Das für den Treiber passendes Gerät mit der hier definierten Id wird
 * bei der Initialisierung des Moduls hinzugefügt.
 */
static const struct i2c_device_id ds3231_dev_id[] = {
	{ "ds3231_drv", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ds3231_dev_id);


/*
 * I2C Treiber-Struktur. Wird für die Registrierung des Treibers im
 * Linux-Kernel benötigt.
 */
static struct i2c_driver ds3231_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name  = "ds3231_drv",
	},
	.id_table = ds3231_dev_id,
	.probe	  = ds3231_probe,
	.remove	  = ds3231_remove,
};


/*
 * Initialisierungsroutine des Kernel-Modules.
 * 
 * Wird beim Laden des Moduls aufgerufen. Innerhalb der Funktion
 * wird das neue Device (DS3231) registriert und der I2C Treiber
 * hinzugefügt.
 */
static int __init ds3231_init(void)
{	
	int ret;	
	struct i2c_adapter *adapter;
	const struct i2c_board_info info = {
		I2C_BOARD_INFO("ds3231_drv", 0x68)
	};
	printk("DS3231_drv: ds3231_module_init aufgerufen\n");
	
	/*
	* Gerätenummer für 1 Gerät allozieren
	*/
	ret = alloc_chrdev_region(&rtc_dev, 0, 1, "ds3231_drv");
	if(ret < 0) {
		printk(KERN_ALERT " Fehler bei alloc_chrdev_region()\n");
	return ret;
	}
	/*
	* cdev-Struktur initialiesieren und dem Kernel bekannt machen.
	*/
	cdev_init(&rtc_cdev, &fops);
	ret = cdev_add(&rtc_cdev, rtc_dev, 1);
	if(ret < 0) {
		printk(KERN_ALERT "Fehler bei registrierung von CDEV-Struktur");
		goto unreg_chrdev;
	}
	/*
	* Eintrag im sysfs registrieren. Dadurch wird die Device-Datei
	* automatisch von udev Dienst erstellt.
	*/
	rtc_devclass = class_create(THIS_MODULE, "chardev");
	if(rtc_devclass == NULL) {
		printk(KERN_ALERT "Class konnte nicht erstellt werden.\n" );
		goto clenup_cdev;
	}
	if(device_create(rtc_devclass,NULL,rtc_dev,NULL,"ds3231_drv") == NULL) {
		printk(KERN_ALERT "Device konnte nicht erstellt werden.\n");
		goto cleanup_chrdev_class;
	}
	/* Mein_treiber wurde erfolgreich initialisiert */
	/*I2C Init*/	

	ds3231_client = NULL;
	adapter = i2c_get_adapter(1);
	if(adapter == NULL) {
		printk("DS3231_drv: I2C Adapter nicht gefunden\n");
		return -ENODEV;
	}

	/* Neues I2C Device registrieren */
	ds3231_client = i2c_new_device(adapter, &info);
	if(ds3231_client == NULL) {
		printk("DS3231_drv: I2C Client: Registrierung fehlgeschlagen\n");
		return -ENODEV;
	}
	/* Treiber registrieren */
	ret = i2c_add_driver(&ds3231_driver);
	if(ret < 0) {
		printk("DS3231_drv: Treiber konnte nicht hinzugefügt werden (errorn = %d)\n", ret);
		i2c_unregister_device(ds3231_client);
		ds3231_client = NULL;
	}
	return ret; // Alles geklappt
	
	/* Resourcen freigeben und Fehler melden. */
	cleanup_chrdev_class:
		class_destroy(rtc_devclass);
	clenup_cdev:
		cdev_del(&rtc_cdev);
	unreg_chrdev:
		unregister_chrdev_region(rtc_dev, 1);
	return -EIO;
	
	
}
module_init(ds3231_init);


/*
 * Aufräumroutine des Kernel-Modules.
 * 
 * Wird beim Enladen des Moduls aufgerufen. Innerhalb der Funktion
 * werden alle Resourcen wieder freigegeben.
 */
static void __exit ds3231_exit(void)
{
	printk("DS3231_drv: ds3231_module_exit aufgerufen\n");	
	device_destroy(rtc_devclass, rtc_dev);
	class_destroy(rtc_devclass);
	cdev_del(&rtc_cdev);
	unregister_chrdev_region(rtc_dev, 1);
	printk("Treiber entladen\n");
	if(ds3231_client != NULL) {
		i2c_del_driver(&ds3231_driver);
		i2c_unregister_device(ds3231_client);
	}
}
module_exit(ds3231_exit);


/* Module-Informationen. */
MODULE_AUTHOR("Alexander Golke & Julian Werner");
MODULE_DESCRIPTION("RTC driver for DS3231");
MODULE_LICENSE("GPL");
