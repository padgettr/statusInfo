/* Config */

#define STATUS_TIMEOUT 10000  /* Time between status updates (ms) */
#define NOTIFY_TIMEOUT 2000   /* Time to display notifications for (ms) */
#define MX_STATUS_CHARS 192   /* Maximum allowed status characters for info line (all elements including alsa and udev notifications */
#define MX_ELEMENT_CHARS 32   /* Maximum allowed characters in any status element (including separator and NULL termination character */
#define MX_NUMBER_ELEMENTS MX_STATUS_CHARS/MX_ELEMENT_CHARS

/* Status info */
#define THERMAL_ZONE "/sys/class/hwmon/hwmon" /* Path to thermal_zone<n> or hwmon<n> */
#define THERMAL_NAME "cpu_thermal\nacpitz\nk10temp\namdgpu\n"   /* Names of the temperature input to monitor: first matched will be displayed. Each search term must end in new line '\n' character. Maximum of 9 search terms. */
#define TEMP_INPUT "temp1_input"    /* Filename for temperature input to monitor: only one temperature is reported */

/* Status line only displays info for one battery defined below
 */
#define POWER_SUPPLY "/sys/class/power_supply"
#define BATTERY_NAME "BAT1"
#define ADAPTOR_NAME "AC"
//#define ADAPTOR_NAME "ADP1"

/**********************/
/* alsa monitor setup */
/**********************/

/* Use "default" for defined default system card;
 * NOTE: default can be a virtual device (e.g. pipewire) which may not exist on startup.
 * Examples "hw:0", "hw:VT82xx" etc.
 * Mixer control names are the same as those shown by 'alsamixer' or 'amixer scontrols'
 */
//#define ALSA_HW_DEVICE "hw:VT82xx"    
#define ALSA_HW_DEVICE "default"
static char *si_alsaMonitor[]={ "Master", "PCM", "Headphone", "Speaker", NULL };

/* Separator character between elements */
static char si_separator=' ';

/* Network
 * No setup here: Interface names discovered via getifaddrs(). Assumption:
 * ethernet device names start with 'e', wireless names start with 'w' bridge device names start with 'b'
 */

/**********************/
/* udev monitor setup */
/**********************/

/* Udev callback functions:
 *    int (*func)(struct udev_device *dev, char *displayInfo);
 *    Only called on change event (add / remove events reported but these not called.
 *    These return -1 on error, or 0 on success.
 *    dev:           device which trigged the change event
 *    displayInfo:   information to display, limited to MX_ELEMENT_CHARS
 */
static int backlightStatus(struct udev_device *dev, char *brightnessLevel) {
   long value;

   value=100*atol(udev_device_get_sysattr_value(dev, "actual_brightness"))/atol(udev_device_get_sysattr_value(dev, "max_brightness"));
   snprintf(brightnessLevel, MX_ELEMENT_CHARS, "LCD: %li%%%c", value, si_separator);

   return 0;
}

static int rfKillStatus(struct udev_device *dev, char *rfkillInfo) {
   int rfStatus=-1;
   const char *soft, *hard, *index, *type;
   
   index=udev_device_get_sysattr_value(dev, "index");
   type=udev_device_get_sysattr_value(dev, "type");
   soft=udev_device_get_sysattr_value(dev, "soft");
   hard=udev_device_get_sysattr_value(dev, "hard");
   if (soft!=NULL || hard!=NULL) {
      if (soft[0]=='0' && hard[0]=='0')
         rfStatus=1;
      else
         rfStatus=0;
   }
   if (rfStatus==-1)
      return -1;

   snprintf(rfkillInfo, MX_ELEMENT_CHARS, "%s [rfkill index:%s]: %s%c", type, index, (rfStatus==0) ? "Off": "On", si_separator);

   return 0;
}

static int powerStatus(struct udev_device *dev, char *powerInfo) {
   const char *udevSubsystem=udev_device_get_subsystem(dev);

   if (strcoll(BATTERY_NAME, udev_device_get_sysname(dev))==0) {
      powerInfo[0]='\0'; /* battery reports periodic drops in charge. Just return here with empty powerInfo, status info will update */
      return 0;
   }

   if (strcoll(ADAPTOR_NAME, udev_device_get_sysname(dev))==0)
      snprintf(powerInfo, MX_ELEMENT_CHARS, "%s: %s: %s%c", udevSubsystem, udev_device_get_sysname(dev), (*(udev_device_get_sysattr_value(dev, "online"))=='0') ? "Unplugged": "Plugged", si_separator);
   else
      snprintf(powerInfo, MX_ELEMENT_CHARS, "%s: %s: %s%c", udevSubsystem, udev_device_get_sysname(dev), udev_device_get_action(dev), si_separator);
   
   return 0;
}

/* Set subsystem to monitor and associated callback function for display
 */
static si_udevActions udevActions[] = {
   /* subSystem,    callback function */
   { "backlight",       backlightStatus },
   { "rfkill",          rfKillStatus },
   { "power_supply",    powerStatus },
};
