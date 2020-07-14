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
#include <linux/mutex.h>

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
# define DS3231_DAY		0x04
# define DS3231_HOUR		0x02
# define DS3231_MINUTE		0x01
# define DS3231_SECOND		0x00
# define DS3231_TEMP_MSB	0x11


static int dev_open(struct inode *, struct file *);
static int dev_close(struct inode *, struct file *);
static ssize_t dev_read(struct file *, char __user *, size_t, loff_t *);
static ssize_t dev_write(struct file *, const char __user *, size_t, loff_t *);
static bool itoa(int,char *);
static int atoi(int,char *);
void translateMonth(int,char *);		//TO-DO: Konsistente Funktionennamen!
static bool check_date(int, int, int, int, int, int, int);
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
	/*Überprüfung des OSF, BSF und der Termperatur des RTCs*/
	s32 tempMSB,status_check;
	int rueckgabewert_check_state = 0; /**/
	status_check = i2c_smbus_read_byte_data(ds3231_client,DS3231_REG_STATUS);
	tempMSB = i2c_smbus_read_byte_data(ds3231_client,DS3231_TEMP_MSB);
	state.osf = status_check & 0x80; /*Bit 8 ist das OSF Bit*/
	state.bsy = status_check & 0x04; /*Bit 3 ist das BSY Bit*/
	rueckgabewert_check_state = tempMSB & 0x7F; /*Temperatur ohne Vorzeichen abspeichern*/
	if(tempMSB & 0x80) rueckgabewert_check_state *= -1;/*Vorzeichen Überprüfung*/
	state.temperature = rueckgabewert_check_state;
	if(!state.osf) return false;
	/*OSF war nicht aktiv und wird nun aktiviert*/
	i2c_smbus_write_byte_data(ds3231_client,DS3231_REG_STATUS,status_check & 0x7F);
	return true;	
}

static int dev_open(struct inode *inode_open, struct file *file_open){
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
	return 0;
}

static int dev_close(struct inode *inode_close, struct file *file_close){
	return 0;
}

static ssize_t dev_read(struct file *file_read, char __user *puffer_read, size_t bytes_read, loff_t *offset_read){
	/* 
	 * Read Funktion des Treibers
	 * Liest das Datum aus der RTC und übermittelt es in den Userspace
	 * Außerdem überprüfung verschiedener Parameter 
	 */
	int count_read = 0;
	char string_read[3];
	bool century_read = false, format = false;	
	s32 year_read,month_read,day_read,hour_read,minute_read,second_read;
	char date_read[29] = {""}, monthWord[10]={""};	
	if(busy){ /* Lesen verhindern, falls Driver beschaeftigt */
		printk("DS3231_drv: Das Geraet ist beschaeftigt!\n");
		return -EBUSY;
	}	
    busy = true; /* Treiber auf "beschaeftigt" setzten*/
	
	while(puffer_read[count_read++] != '\0'); /* */
	if(count_read < 21){ /*Es wurde noch kein vollstaendiges Datum geschrieben*/
		/*Alle Werte aus RTC holen*/
		year_read = i2c_smbus_read_byte_data(ds3231_client,DS3231_YEAR);
		month_read = i2c_smbus_read_byte_data(ds3231_client,DS3231_MONTH);
		day_read = i2c_smbus_read_byte_data(ds3231_client,DS3231_DAY);
		hour_read = i2c_smbus_read_byte_data(ds3231_client,DS3231_HOUR);
		minute_read = i2c_smbus_read_byte_data(ds3231_client,DS3231_MINUTE);
		second_read = i2c_smbus_read_byte_data(ds3231_client,DS3231_SECOND);
		
		if(year_read < 0 || month_read < 0 || day_read < 0 || hour_read < 0 || minute_read < 0 || second_read < 0) {
			/*Fehler beim Lesen, nochmal Versuchen*/
			busy = false;
			return 0;
		}
		if(month_read >>7) { /* Centurybit -> 2000-2099 -> false, 2100-2199 -> true */
			century_read = true;
			month_read &= 0x7F; /*Bit löschen*/
		}
		if(hour_read >> 6) {/*12(true) or 24(false) Format*/
			format = true;
		}
		year_read = ((year_read>>4)*10) + (year_read & 0xF);
		month_read = ((month_read>>4)*10) + (month_read & 0xF);
		day_read = ((day_read>>4)*10) + (day_read & 0xF);
		minute_read = ((minute_read>>4)*10) + (minute_read & 0xF);
		second_read = ((second_read>>4)*10) + (second_read & 0xF);
		if(format) { /*12 Stunden Format*/
			if(hour_read & 0x20) {
				hour_read = 12 + (hour_read & 0xF) + (((hour_read & 0x10)>>4)*10); 
			} 
			else {
				hour_read = (hour_read & 0xF) + (((hour_read & 0x10)>>4)*10);
			}
		}
		else { /*24 Stunden Format*/
			hour_read = (hour_read & 0xF) + (((hour_read & 0x10)>>4)*10) + (((hour_read & 0x20)>>5)*20);
		}
		/*Ab hier haben die s32 die korrekten Werte!
		  und der String wird "zusammen gebaut"*/	
		translateMonth(month_read,monthWord);
		if(itoa(day_read,string_read)) {
			strcat(date_read,string_read);
			strcat(date_read,". ");
		}
		strcat(date_read,monthWord);
		strcat(date_read," ");
		if(itoa(hour_read,string_read)) {
                        strcat(date_read,string_read);
                        strcat(date_read,":");
                }
		if(itoa(minute_read,string_read)) {
                        strcat(date_read,string_read);
                        strcat(date_read,":");
                }
		if(itoa(second_read,string_read)) {
                        strcat(date_read,string_read);
                        strcat(date_read," ");
                }
		if(century_read){
			strcat(date_read,"21");
		}
		else {
			strcat(date_read,"20");
		}
		if(itoa(year_read,string_read)) {
                        strcat(date_read,string_read);
                        strcat(date_read,"\n");
                }		
		count_read = copy_to_user(puffer_read,date_read,sizeof(date_read));
		busy = false;
		return sizeof(date_read) - count_read;
	}
	busy = false; /*Nicht mehr beschaeftigt*/
	return 0;
}
void translateMonth(int month_translate,char *string_translate) {
	/*Wandelt die Monatszahl(month_translate) in einen String um und speichert den in string_translate*/
	switch(month_translate) {
		case 1:
			strcpy(string_translate,"Januar");
			break;
		case 2:
			strcpy(string_translate,"Februar");
			break;
		case 3:
			strcpy(string_translate,"Maerz");
			break;
		case 4:
			strcpy(string_translate,"April");
			break;
		case 5:
			strcpy(string_translate,"Mai");
			break;
		case 6:
			strcpy(string_translate,"Juni");
			break;
		case 7:
			strcpy(string_translate,"Juli");
			break;
		case 8:
			strcpy(string_translate,"August");
			break;
		case 9:
			strcpy(string_translate,"September");
			break;
		case 10:
			strcpy(string_translate,"Oktober");
			break;
		case 11:
			strcpy(string_translate,"November");
			break;
		case 12:
			strcpy(string_translate,"Dezember");
	}
}
 
static bool itoa(int zahl_itoa, char *string_itoa){
	/* Integer to Array
	 * Wandelt den Int in ein zweistelliges Char Array um
	 * Ist die Zahl mehr als zweistellig wird false zurück geben, ansonsten true*/
	if(zahl_itoa > 99 || zahl_itoa < 0){
		return false;
	}
	string_itoa[2] = '\0';
	if(zahl_itoa < 10){
		string_itoa[0] = '0';
	}
	else{
		string_itoa[0] = (zahl_itoa/10)+'0';
	}
	string_itoa[1] = (zahl_itoa % 10)+'0';
	return true; 
}

static int atoi(int zahl_atoi, char *string_atoi){
	/* Array to Integer
	 * Wandelt string_atoi in einen Integer um
	 * Zusätzliche Überprüfung von richtigen Daten/Zeichen*/
	int rueckgabe_atoi;
	int convert = string_atoi[zahl_atoi],convert2 = string_atoi[zahl_atoi+1];	
	if((convert < '0' || convert > '9') || (convert2 < '0' || convert2 > '9')) return -1;
	rueckgabe_atoi = (string_atoi[zahl_atoi]-'0')*10+(string_atoi[zahl_atoi+1]-'0');
	return rueckgabe_atoi;
}
static int atoiDrei(int zahl_atoiDrei, char *string_atoiDrei){
	/* Array to Integer
	 * Wandelt string_atoi in einen 0-3 Stelligen Integer um*/
	if(string_atoiDrei[zahl_atoiDrei+1] == '\0')return -1;
	if(string_atoiDrei[zahl_atoiDrei+2] == '\0') return string_atoiDrei[zahl_atoiDrei]-'0';
	if(string_atoiDrei[zahl_atoiDrei+3]	== '\0')return ((string_atoiDrei[zahl_atoiDrei]-'0')*10+(string_atoiDrei[zahl_atoiDrei+1]-'0'));
	return ((string_atoiDrei[zahl_atoiDrei]-'0')*100+(string_atoiDrei[zahl_atoiDrei+1]-'0')*10+(string_atoiDrei[zahl_atoiDrei+2]-'0'));
}

static bool check_date(int day_check, int month_check, int century_check, int year_check, int hour_check, int minute_check, int second_check) {
	/* Prüft das Datum auf Richtigkeit und gibt beim Fehler false zurueck */
    int jahr = 0;
    if(day_check < 1 || day_check > 31) return false;
    if(month_check < 1 || month_check > 12) return false;
    if(year_check < 0) return false;
    if(century_check != 20 && century_check != 21) return false;
    if(hour_check < 0 || hour_check > 24) return false;
    if(minute_check < 0 || minute_check > 59) return false;
    if(second_check < 0 || second_check > 59) return false;
    if(day_check < 29) return true;
    if(month_check != 2) {
        if(day_check < 31) return true;
        if(month_check == 1 || month_check == 3 ||month_check == 5 ||month_check == 7 ||month_check == 8 ||month_check == 10 ||month_check == 12) return true;
        return false;
    }
    else {
        if(day_check > 29) return false;
        jahr = (1000 * century_check) + year_check;
        if( (!(jahr %4)) && jahr%100) return true;
        if( (!(jahr %100)) && jahr%400) return false;
        if((!(jahr %400))) return true;
        return false;
    }
}

static ssize_t dev_write(struct file *file_write, const char __user *puffer_write, size_t bytes, loff_t *offset_write){
	char date_write[20] = "";
	int count_write = copy_from_user(date_write,puffer_write,bytes),temp;
	int year_write,month_write,day_write,hour_write,minutes_write,seconds_write,century_write;
	bool century_check_write;

	if(busy){
		printk("DS3231_drv: Das Geraet ist beschaeftigt!\n");
		return -EBUSY;
	}
    busy = true;
	
	if(date_write[0] == '?'){
		state.manualTemp = true;
		if(date_write[1] == '-'){
			temp = atoiDrei(2,date_write);
			temp *= -1;
			state.temperature = temp;
		}
		else{
			temp = atoiDrei(1,date_write);
			state.temperature = temp;
		}
	}	
	else {
		if(state.temperature < -40){
			printk("DS3231_drv: Die Temperatur ist sehr kalt\n");
		}
		else if(state.temperature > 85){
			printk("DS3231_drv: Die Temperatur ist sehr warm\n");
		}
		century_write = atoi(0, date_write);
		year_write = atoi(2, date_write);
		month_write = atoi(5, date_write);
		day_write = atoi(8,date_write);
		hour_write = atoi(11,date_write);
		minutes_write = atoi(14,date_write);
		seconds_write = atoi(17,date_write);

		if(date_write[4] != '-' || date_write[7] != '-' || date_write[10] != ' ' || date_write[13] != ':' || date_write[16] != ':'){
			printk("DS3231_drv: Format falsch! >:( \n");
			busy = false;
			return -EINVAL;
		}
	 
		if(!check_date(day_write,month_write,century_write,year_write,hour_write,minutes_write,seconds_write)){
			if(century_write < 20 || century_write > 21){
			 printk("DS3231_drv: Werte ausserhalb des Wertebereichs!\n");
			 busy = false;
			 return -EOVERFLOW;
			}
			printk("DS3231_drv: Datum existiert nicht!\n");
			busy = false;
			return -EINVAL;
		} 
		
		if(century_write == 21){
			century_check_write = true;
		} 
		else{
			century_check_write = false;
		}
		i2c_smbus_write_byte_data(ds3231_client,DS3231_SECOND,(((seconds_write/10) << 4) | (seconds_write%10)));
		i2c_smbus_write_byte_data(ds3231_client,DS3231_MINUTE,(((minutes_write/10) << 4) | (minutes_write%10))); 
		i2c_smbus_write_byte_data(ds3231_client,DS3231_HOUR,(((hour_write/10) << 4) | (hour_write%10)));	
		i2c_smbus_write_byte_data(ds3231_client,DS3231_DAY,(((day_write/10) << 4) | (day_write%10)));	
		i2c_smbus_write_byte_data(ds3231_client,DS3231_MONTH,(((month_write/10) << 4) | (month_write%10) | (century_check_write << 7)));	
		i2c_smbus_write_byte_data(ds3231_client,DS3231_YEAR,(((year_write/10) << 4) | (year_write%10)));
	}
	busy = false;
	return bytes-count_write;
}



/*
 * Initialisierung des Treibers und Devices.
 *
 * Diese Funktion wird von Linux-Kernel aufgerufen, aber erst nachdem ein zum
 * Treiber passende Device-Information gefunden wurde. Innerhalb der Funktion
 * wird der Treiber und das Device initialisiert.
 */
static int ds3231_probe(struct i2c_client *client_probe, const struct i2c_device_id *id)
{	
	s32 reg0, reg1;
	u8 reg_cnt, reg_sts;
	printk("DS3231_drv: ds3231_probe called\n");

	/*
	 * Control und Status Register auslesen.
	 */
	reg0 = i2c_smbus_read_byte_data(client_probe, DS3231_REG_CONTROL);
	reg1 = i2c_smbus_read_byte_data(client_probe, DS3231_REG_STATUS);
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
	i2c_smbus_write_byte_data(client_probe, DS3231_REG_CONTROL, reg_cnt);

	/*
	 * Prüfe Oscilator zustand. Falls Fehler vorhanden, wird das Fehlerfalg
	 * zurückgesetzt.
	 */
	if (reg_sts & DS3231_BIT_OSF) {
		reg_sts &= ~DS3231_BIT_OSF;
		i2c_smbus_write_byte_data(client_probe, DS3231_REG_STATUS, reg_sts);
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
static int ds3231_remove(struct i2c_client *client_remove)
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
