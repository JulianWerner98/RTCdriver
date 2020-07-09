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
#include <uapi/linux/string.h>

/* Register Definitionen */
#define DS3231_REG_CONTROL	0x0e
# define DS3231_BIT_nEOSC	0x80
# define DS3231_BIT_INTCN	0x04
# define DS3231_BIT_A2IE	0x02
# define DS3231_BIT_A1IE	0x01
# define DS3231_REG_STATUS	0x0f
# define DS3231_BIT_OSF		0x80
# define DS3231_YEAR		0x06
# define DS3231_MONTH		0x05
# define DS3231_DAY			0x04
# define DS3231_HOUR		0x02
# define DS3231_MINUTE		0x01
# define DS3231_SECOND		0x00
# define DS3231_TEMP_MSB	0x11


static int dev_open(struct inode *inode, struct file *file);
static int dev_close(struct inode *inode, struct file *file);
static ssize_t dev_read(struct file *file, char __user *puffer, size_t bytes, loff_t *offset);
static ssize_t dev_write(struct file *file, const char __user *puffer, size_t bytes, loff_t *offset);
static bool itoa(int n,char *string);
static int atoi(int n,char *string);
void translateMonth(int,char*);		//TO-DO: Konsistente Funktionennamen!
static bool checkDate(int, int, int, int, int, int, int);
static bool check_state(void);
/*
 * Der Zeiger wird bei Initialisierung gesetzt und wird für die
 * i2c Kommunikation mit dem  Device (DS3231) benötigt.
 */
static struct i2c_client *ds3231_client;
static dev_t rtc_dev;
static struct cdev rtc_cdev;
static struct class *rtc_devclass;
static bool busy = false;
static struct statusInfo {
		bool osf;
		bool bsy;
		int temperature;
		bool manualTemp;
}state;

static struct file_operations fops = {

	.owner 		= THIS_MODULE,
	.read		= dev_read,
	.write		= dev_write,
	.open		= dev_open,
	.release 	= dev_close,
};

static bool check_state(void){
	s32 tempMSB,status;
	int zwischenspeicher = 0;	//signed war hier bruder
	status = i2c_smbus_read_byte_data(ds3231_client,DS3231_REG_STATUS);
	tempMSB = i2c_smbus_read_byte_data(ds3231_client,DS3231_TEMP_MSB);
	state.osf = status & 0x80;
	state.bsy = status & 0x04;
	zwischenspeicher = tempMSB & 0x7F; 
	if(tempMSB & 0x80) zwischenspeicher *= -1;
	state.temperature = zwischenspeicher;
	if(!state.osf) return false;
	i2c_smbus_write_byte_data(ds3231_client,DS3231_REG_STATUS,status & 0x7F);
	return true;	
}

static int dev_open(struct inode *inode, struct file *file){
	return 0;
}

static int dev_close(struct inode *inode, struct file *file){
	return 0;
}

static ssize_t dev_read(struct file *file, char __user *puffer, size_t bytes, loff_t *offset){

	int count = 0;
	char string[3];
	bool century = false, format = false;	
	s32 year,month,day,hour,minute,second;
	char date[29] = {""}, monthWord[10]={""};
	
	if(busy){
		printk("DS3231_drv: Das Geraet ist beschaeftigt!\n");
		return -EBUSY;
	}	
    busy = true;

	while(puffer[count++] != '\0');
	if(count < 21){
		if(!state.manualTemp){
			if(check_state()){
				printk("DS3231_drv: OSF nicht aktiv!\n");
				return -EAGAIN;
			}
		}
		else state.manualTemp = false;
		if(state.temperature < -40){
				printk("DS3231_drv: Die Temperatur ist sehr kalt\n");
		}
		else if(state.temperature > 85){
			printk("DS3231_drv: Die Temperatur ist sehr warm\n");
		}
		if(state.bsy){ /*Busy von RTC*/
			printk("DS3231_drv: Die RTC ist beschaeftigt!\n");
			return -EBUSY;
		}
		year = i2c_smbus_read_byte_data(ds3231_client,DS3231_YEAR);
		month = i2c_smbus_read_byte_data(ds3231_client,DS3231_MONTH);
		day = i2c_smbus_read_byte_data(ds3231_client,DS3231_DAY);
		hour = i2c_smbus_read_byte_data(ds3231_client,DS3231_HOUR);
		minute = i2c_smbus_read_byte_data(ds3231_client,DS3231_MINUTE);
		second = i2c_smbus_read_byte_data(ds3231_client,DS3231_SECOND);
		
		if(year < 0 || month < 0 || day < 0 || hour < 0 || minute < 0 || second < 0) {
			busy = false;
			return 0;
		}
		if(month >>7) { /* Centurybit -> 2000-2099 -> false, 2100-2199 -> true */
			century = true;
			month &= 0x7F; /*Bit löschen*/
		}
		if(hour >> 6) {/*12(true) or 24(false) Format*/
			format = true;
		}
		year = ((year>>4)*10) + (year & 0xF);
		month = ((month>>4)*10) + (month & 0xF);
		day = ((day>>4)*10) + (day & 0xF);
		minute = ((minute>>4)*10) + (minute & 0xF);
		second = ((second>>4)*10) + (second & 0xF);
		if(format) { /*12 Stunden Format*/
			if(hour & 0x20) {
				hour = 12 + (hour & 0xF) + (((hour & 0x10)>>4)*10); 
			} 
			else {
				hour = (hour & 0xF) + (((hour & 0x10)>>4)*10);
			}
		}
		else { /*24 Stunden Format*/
			hour = (hour & 0xF) + (((hour & 0x10)>>4)*10) + (((hour & 0x20)>>5)*20);
		}
		/*Ab hier haben die s32 die korrekten Werte!*/	
		translateMonth(month,monthWord);
		if(itoa(day,string)) {
			strcat(date,string);
			strcat(date,". ");
		}
		strcat(date,monthWord);
		strcat(date," ");
		if(itoa(hour,string)) {
                        strcat(date,string);
                        strcat(date,":");
                }
		if(itoa(minute,string)) {
                        strcat(date,string);
                        strcat(date,":");
                }
		if(itoa(second,string)) {
                        strcat(date,string);
                        strcat(date," ");
                }
		if(century){
			strcat(date,"21");
		}
		else {
			strcat(date,"20");
		}
		if(itoa(year,string)) {
                        strcat(date,string);
                        strcat(date,"\n");
                }		
		count = copy_to_user(puffer,date,sizeof(date));
		busy = false;
		return sizeof(date) - count;
	}
	busy = false;
	return 0;
}
void translateMonth(int month,char *string) {
	switch(month) {
		case 1:
			strcpy(string,"Januar");
			break;
		case 2:
                        strcpy(string,"Februar");
                        break;
		case 3:
                        strcpy(string,"Maerz");
                        break;
		case 4:
                        strcpy(string,"April");
                        break;
		case 5:
                        strcpy(string,"Mai");
                        break;
		case 6:
                        strcpy(string,"Juni");
                        break;
		case 7:
                        strcpy(string,"Juli");
                        break;
		case 8:
                        strcpy(string,"August");
                        break;
		case 9:
                        strcpy(string,"September");
                        break;
		case 10:
                        strcpy(string,"Oktober");
                        break;
		case 11:
                        strcpy(string,"November");
                        break;
		case 12:
                        strcpy(string,"Dezember");
	}
}
 
static bool itoa(int n, char *string){
	if(n > 99 || n < 0){
		return false;
	}
	string[2] = '\0';
	if(n < 10){
		string[0] = '0';
	}
	else{
		string[0] = (n/10)+'0';
	}
	string[1] = (n % 10)+'0';
	return true; 
}

static int atoi(int n, char *string){
	int temp;
	int convert = string[n];
	int convert2 = string[n+1];	
	if((convert < '0' || convert > '9') || (convert2 < '0' || convert2 > '9')) return -1;
	temp = (string[n]-'0')*10+(string[n+1]-'0');
	return temp;
}
static int atoiDrei(int n, char *string){
	if(string[n+1] == '\0')return -1;
	if(string[n+2] == '\0') return string[n]-'0';
	if(string[n+3]	== '\0')return ((string[n]-'0')*10+(string[n+1]-'0'));
	return ((string[n]-'0')*100+(string[n+1]-'0')*10+(string[n+2]-'0'));
}

static bool checkDate(int day, int month, int century, int year, int hour, int minute, int second) {
    int jahr = 0;
    if(day < 1 || day > 31) return false;
    if(month < 1 || month > 12) return false;
    if(year < 0) return false;
    if(century != 20 && century != 21) return false;
    if(hour < 0 || hour > 24) return false;
    if(minute < 0 || minute > 59) return false;
    if(second < 0 || second > 59) return false;
    if(day < 29) return true;
    if(month != 2) {
        if(day < 31) return true;
        if(month == 1 || month == 3 ||month == 5 ||month == 7 ||month == 8 ||month == 10 ||month == 12) return true;
        return false;
    }
    else {
        if(day > 29) return false;
        jahr = (1000 * century) + year;
        if( (!(jahr %4)) && jahr%100) return true;
        if( (!(jahr %100)) && jahr%400) return false;
        if((!(jahr %400))) return true;
        return false;
    }
}

static ssize_t dev_write(struct file *file, const char __user *puffer, size_t bytes, loff_t *offset){
	char date[20] = "";
	int count = copy_from_user(date,puffer,bytes),temp;
	int year,month,day,hour,minutes,seconds,century;
	bool century_check;

	if(busy){
		printk("DS3231_drv: Das Geraet ist beschaeftigt!\n");
		return -EBUSY;
	}
	if(check_state()){
		printk("DS3231_drv: OSF nicht aktiv!\n");
		return -EAGAIN;
	}
	if(state.bsy){ /*Busy von RTC*/
		printk("DS3231_drv: Das Geraet ist beschaeftigt!\n");
		return -EBUSY;
	}
    busy = true;
	if(date[0] == '?'){
		state.manualTemp = true;
		if(date[1] == '-'){
			temp = atoiDrei(2,date);
			temp *= -1;
			state.temperature = temp;
		}
		else{
			temp = atoiDrei(1,date);
			state.temperature =temp;
		}
	}	
	else {
		if(state.temperature < -40){
			printk("DS3231_drv: Die Temperatur ist sehr kalt\n");
		}
		else if(state.temperature > 85){
			printk("DS3231_drv: Die Temperatur ist sehr warm\n");
		}
		century = atoi(0, date);
		year = atoi(2, date);
		month = atoi(5, date);
		day = atoi(8,date);
		hour = atoi(11,date);
		minutes = atoi(14,date);
		seconds = atoi(17,date);

		if(date[4] != '-' || date[7] != '-' || date[10] != ' ' || date[13] != ':' || date[16] != ':'){
			printk("DS3231_drv: Format falsch! >:( \n");
			busy = false;
			return -EINVAL;
		}
	 
		if(!checkDate(day,month,century,year,hour,minutes,seconds)){
			if(century < 20 || century > 21){
			 printk("DS3231_drv: Werte ausserhalb des Wertebereichs!\n");
			 busy = false;
			 return -EOVERFLOW;
			}
			printk("DS3231_drv: Datum existiert nicht!\n");
			busy = false;
			return -EINVAL;
		} 
		
		if(century == 21){
			century_check = true;
		} 
		else{
			century_check = false;
		}
		i2c_smbus_write_byte_data(ds3231_client,DS3231_SECOND,(((seconds/10) << 4) | (seconds%10)));
		i2c_smbus_write_byte_data(ds3231_client,DS3231_MINUTE,(((minutes/10) << 4) | (minutes%10))); 
		i2c_smbus_write_byte_data(ds3231_client,DS3231_HOUR,(((hour/10) << 4) | (hour%10)));	
		i2c_smbus_write_byte_data(ds3231_client,DS3231_DAY,(((day/10) << 4) | (day%10)));	
		i2c_smbus_write_byte_data(ds3231_client,DS3231_MONTH,(((month/10) << 4) | (month%10) | (century_check << 7)));	
		i2c_smbus_write_byte_data(ds3231_client,DS3231_YEAR,(((year/10) << 4) | (year%10)));
	}
	busy = false;
	return bytes-count;
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
	state.manualTemp = false;
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
	return ret; /*Alles geklappt*/
	
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
