/*
 * PWM Fan Controller:
 *   PWM fan controller is developed using pigpio C library for RPi4. It is
 *   intended for "Noctua NF-A4x10 5V PWM" fan, it may work for any PWM fan
 *   with slight adjustment according to the fan specs.
 *   Copyright (c) 2021 - ar51an
 */

#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <pigpio.h>
#include <systemd/sd-journal.h>

int PWM_PIN             = 18;    // HW PWM works at GPIO [12, 13, 18 & 19] on RPi4B
int FREQUENCY           = 25000; // Noctua Specs: Target_Frequency=25kHz
int RPM_MAX             = 5000;  // Noctua Specs: Max=5000
int RPM_MIN             = 1500;  // Noctua Specs: Min=1000 [Kept 1500 as Min]
int RPM_OFF             = 0;
int TEMP_MAX            = 55;    // Above this temperature [FAN=ON At Max speed]
int TEMP_LOW            = 40;    // Below this temperature [FAN=OFF]
int WAIT                = 5000;  // Milliseconds before adjusting RPM
int origPwmPinMode      = -1;
float tempLimitDiffPct  = 0.0f;
char thermalFilename[]  = "/sys/class/thermal/thermal_zone0/temp";
static volatile sig_atomic_t keepRunning = 1;

void logConfParams () {
    char logFormat[] = "Config values loaded: PWM_PIN=%d | | RPM_MAX=%d | RPM_MIN=%d | RPM_OFF=%d "
                       "| TEMP_MAX=%d | TEMP_LOW=%d | WAIT=%d | | THERMAL_FILE=%s";
    sd_journal_print(LOG_INFO, logFormat, PWM_PIN, RPM_MAX, RPM_MIN, RPM_OFF, \
                     TEMP_MAX, TEMP_LOW, WAIT, thermalFilename);
}

void initFanControl () {
    /* Assign global vars with config file (if provided) values */
    FILE *confFile;
    char confFilename[]  = "/opt/gpio/fan/params.conf";
    confFile = fopen(confFilename, "r");
    if (confFile != NULL) {
        char confFormat[] = "PWM_PIN=%d RPM_MAX=%d RPM_MIN=%d RPM_OFF=%d TEMP_MAX=%d "
                            "TEMP_LOW=%d WAIT=%d THERMAL_FILE=%s";
        fscanf(confFile, confFormat, &PWM_PIN, &RPM_MAX, &RPM_MIN, &RPM_OFF, \
               &TEMP_MAX, &TEMP_LOW, &WAIT, thermalFilename);
        logConfParams();
        fclose(confFile);
    }
    else
        sd_journal_print(LOG_WARNING, "params.conf not found - Default values loaded");
    /* Calculate values of global vars */
    tempLimitDiffPct = (float) (TEMP_MAX-TEMP_LOW)/100;
}

int initPigpio () {
    int config = gpioCfgGetInternals();
    config |= PI_CFG_NOSIGHANDLER;
    gpioCfgSetInternals(config);
    if (gpioInitialise() < 0) {
        sd_journal_print(LOG_ERR, "pigpio initialization failed ...");
        return -1;
    }
    return 0;
}

int getPinMode (int pin) {
    /* Mode Name Mapping: INPUT=0, OUTPUT=1, ALT0=4, ALT1=5, ALT2=6, ALT3=7, ALT4=3, ALT5=2 */
    return gpioGetMode(pin);
}

void setFanSpeed (int pin, int speed) {
    gpioPWM(pin, speed);
}

int getCurrTemp () {
    int currTemp = 0;
    FILE *thermalFile;
    thermalFile = fopen(thermalFilename, "r");
    fscanf(thermalFile, "%d", &currTemp);
    fclose(thermalFile);
    currTemp = ((float) currTemp/1000)+0.5;
    return currTemp;
}

void setupPwm () {
    origPwmPinMode = getPinMode(PWM_PIN);
    gpioSetMode(PWM_PIN, PI_OUTPUT);
    gpioSetPWMfrequency(PWM_PIN, FREQUENCY);
    gpioSetPWMrange(PWM_PIN, RPM_MAX);          // Set PWM range to Max RPM
    setFanSpeed(PWM_PIN, RPM_OFF);              // Set Fan speed to 0 initially
    sd_journal_print(LOG_DEBUG, "[PWM] GPIO:Mode | %d:%d\n", PWM_PIN, origPwmPinMode);
    return;
}

void setFanRpm () {
    int rpm = RPM_OFF;
    static int lastRpm = 0;
    float currTempDiffPct = 0.0f;
    int currTemp = getCurrTemp();
    int tempDiff = (currTemp-TEMP_LOW);
    if (tempDiff > 0) {
        currTempDiffPct = (tempDiff/tempLimitDiffPct);
        rpm = (int) (currTempDiffPct*RPM_MAX)/100;
        rpm = rpm < RPM_MIN ? RPM_MIN : rpm > RPM_MAX ? RPM_MAX : rpm;
        sd_journal_print(LOG_DEBUG, "[PWM] Temp: %d | TempDiff: %.1f%% | RPM: %d", currTemp, currTempDiffPct, rpm);
    }
    if (lastRpm != rpm) setFanSpeed(PWM_PIN, rpm);
    lastRpm = rpm;
    return;
}

void delay (unsigned int waitMillisec)
{
    const int msInSec = 1000;
    struct timespec sleepInterval;
    sleepInterval.tv_sec  = (time_t) (waitMillisec/msInSec);
    sleepInterval.tv_nsec = (long) (waitMillisec%msInSec)*1000000L;
    nanosleep (&sleepInterval, NULL);
}

void start () {
    while (keepRunning) { setFanRpm(); delay(WAIT); }
    return;
}

static void signalHandler (int _) {
    // Exit controller on Ctrl+C
    (void)_;
    keepRunning = 0;
    //printf("\r");
}

void cleanup () {
    // PWM pin cleanup
    setFanSpeed(PWM_PIN, RPM_OFF);
    gpioSetMode(PWM_PIN, origPwmPinMode);
    gpioTerminate();
    sd_journal_print(LOG_INFO, "Cleaned up - Exiting ...");
    return;
}

int main (void)
{
    signal(SIGINT, signalHandler);
    initFanControl();
    if (initPigpio() < 0) return 1;
    setupPwm();
    sd_journal_print(LOG_INFO, "Initialized and running ...");
    start();
    cleanup();
    return 0;
}
