#include "peltierControl.h"

#include <xc.h>
#include <stdint.h>
#include <math.h>
#include <stdbool.h>

#include "mcc_generated_files/system/system.h"
#include "utility.h"
#include "fanControl.h"
#include "tempMonitor.h"
#include "settings.h"
#include "config.h"
#include "currentSense.h"
#include "compactPrint.h"

#define FET_PWM_DISABLE_PERIOD_INT() do { PIR3bits.PWM1PIF = 0; PIE3bits.PWM1PIE = 0; } while (0)
#define FET_PWM_ENABLE_PERIOD_INT() do { PIR3bits.PWM1PIF = 0; PIE3bits.PWM1PIE = 1; } while (0)

#define CWG_ENABLE() do { CWG1CON0bits.EN = 1; } while (0)

#define CWG_DISABLE_OUTPUT() do { CWG1STRbits.CWG1STRA = 0;} while (0) 
#define CWG_ENABLE_OUTPUT() do { CWG1STRbits.CWG1STRA = 1; } while (0)

//State machine for DAC updates
enum CurrentLimitState {
    CURRENT_LIMIT_NO_CHANGE = 0, CURRENT_LIMIT_SET_DAC, CURRENT_LIMIT_SETTLE
};

//DAC Update State Machine w/ Anti-Glitch Operation
static enum CurrentLimitState dacUpdateState = CURRENT_LIMIT_NO_CHANGE;
static uint8_t newDACValue = 0;


//Peltier State Machine
static PeltierState peltierState = PELTIER_STATE_DISABLED;

//Average Duty Cycle of FET
static uint8_t averageDutyCycle = 0;

//Error for Peltier
static PeltierError error = PELTIER_ERROR_NONE;

//Init the Peltier Current Controller
void peltierControl_init(void)
{    
    //Disable Period Interrupt for PWM
    FET_PWM_DISABLE_PERIOD_INT();
    
    //Enable CWG
    CWG_ENABLE();
    CWG_DISABLE_OUTPUT();
    
    //For freq. counting, increment is 1
    NCO1INC = 1;
    NCO1CONbits.EN = 1;
        
    //Enable Int. PWM Signal for CWG
    //Does not go to output yet
    FET_PWM_Enable();    
}

//Performs a self-test of the Peltier element. This function will block when executing. 
bool peltierControl_selfTest(void)
{
    return false;
}

//This function is used to check for faults and to adjust the regulator. 
//If this function is not called, the device will RESET.
//ONLY CALL THIS FUNCTION FROM MAIN
void peltierControl_periodicCheck(void)
{    
    //Clear the WWDT
    WWDT_reset();    
    
    if (fanControl_getFan1RPM() == 0)
    {
        //Fan Error!
        error = PELTIER_FAN1_ERROR;
    }
    if (tempMonitor_getLastHotTemp() > settings_getSetting(SETTINGS_MAX_HEATSINK_TEMP))
    {
        //Overheat - Heatsink
        error = PELTIER_HEATSINK_OVERHEAT;
    }
    if (tempMonitor_getLastHotTemp() < TEMP_NTC_HOT_OPEN)
    {
        //Possible bad NTC connection on the hot side
        //Usually appears as a really low temperature
        error = PELTIER_SENSE_HOT_OPEN;
    }
    if (tempMonitor_getLastIntTemp() > settings_getSetting(SETTINGS_MAX_INT_TEMP))
    {
        //Overheat - Int. Temperature
        error = PELTIER_INT_OVERHEAT;
    }
    if (tempMonitor_getLastColdTemp() < (TEMP_LIMIT_LOW))
    {
        //Overcooled - Cold Plate is below safe temperature
        error = PELTIER_PLATE_TEMP_LIMIT;
    }
    if (tempMonitor_getLastColdTemp() > TEMP_NTC_COLD_OPEN)
    {
        //Possible bad NTC connection on the cold side
        //This is likely a rare condition (expect overcool to detect this), but worth testing for
        error = PELTIER_SENSE_COLD_OPEN;
    }
    if (currentSense_hasOvercurrentOccurred())
    {
        //Overcurrent
        error = PELTIER_OVERCURRENT_ERROR;
    }

    int8_t stopTemp, startTemp, currentTemp;
    stopTemp = settings_getSetting(SETTINGS_LAST_SET_TEMP);
    startTemp = stopTemp;
    currentTemp = tempMonitor_getLastColdTemp();

    //Update fan speed based on temperature
    fanControl_updateSpeedFromTemp(tempMonitor_getLastHotTemp());
    
    //Setup Temperature Hysteresis
    stopTemp -= settings_getSetting(SETTINGS_HYSTER_OVER);
    startTemp += settings_getSetting(SETTINGS_HYSTER_UNDER);

    switch (peltierState)
    {
        case PELTIER_STATE_DISABLED:
        {
            //System is disabled
            break;
        }
        case PELTIER_STATE_STARTUP:
        {
            //Check to see if system is ready

            if (error != PELTIER_ERROR_NONE)
            {
                //Something went wrong
                peltierControl_stop();
            }
            else
            {
                if (currentTemp <= stopTemp)
                {
                    //Plate is already cooled
                    peltierState = PELTIER_STATE_AT_TEMP;
                    
#ifdef DEBUG_PRINT
                    compactPrint_sendStringWithNewline("-- PELTIER IDLE --");
#endif
                }
                else
                {
                    //Plate needs to be cooled
                    peltierState = PELTIER_STATE_COOLING;
                    CWG_ENABLE_OUTPUT();
                    
#ifdef DEBUG_PRINT
                    compactPrint_sendStringWithNewline("-- PELTIER ON --");
#endif
                }
            }

            break;
        }
        case PELTIER_STATE_AT_TEMP:
        {
            //Plate is cooled, waiting to restart

            if (currentTemp >= startTemp)
            {
                //Need to restart the peltier
                peltierState = PELTIER_STATE_COOLING;
                CWG_ENABLE_OUTPUT();
                
#ifdef DEBUG_PRINT
                compactPrint_sendStringWithNewline("-- PELTIER ON --");
#endif
            }
            break;
        }
        case PELTIER_STATE_COOLING:
        {
            //Actively Cooling the Plate

            if (currentTemp <= stopTemp)
            {
                //At Temperature
                peltierState = PELTIER_STATE_AT_TEMP;
                CWG_DISABLE_OUTPUT();
                
#ifdef DEBUG_PRINT
                compactPrint_sendStringWithNewline("-- PELTIER IDLE --");
#endif
            }
            break;
        }
    }

    if (error != PELTIER_ERROR_NONE)
    {
        peltierControl_stop();
    }    
}

void peltierControl_calculateDutyCycle(void)
{
    //Calculate the number of pulses
    
    //Turn off NCO
    NCO1CONbits.EN = 0;
    
    //Clear the duty cycle count
    averageDutyCycle = 0;
    
    //Assumes a 32MHz Clock, once every 10ms
    uint32_t pulses = NCO1ACC;
    
    while (pulses >= 3200)
    {
        pulses -= 3200;
        averageDutyCycle++;
    }
    
    //Basic Rounding - if over 50%, round up
    if (pulses > 1600)
    {
        averageDutyCycle++;
    }

    //If we somehow got over 100% (probably due to delays)
    if (averageDutyCycle > 100)
    {
        averageDutyCycle = 100;
    }
    
    //Clear Accumulator
    NCO1ACCU = 0;
    NCO1ACCH = 0;
    NCO1ACCL = 0;
    
    //Restart NCO
    NCO1CONbits.EN = 1;
}

//Calculate the average duty cycle of the PWM
uint8_t peltierControl_getAverageDutyCycle(void)
{
    return averageDutyCycle;
}

//Returns the current state of the Peltier
PeltierState peltierControl_getState(void)
{
    return peltierState;
}

//This function directly modifies the current threshold in the loop.
void peltierControl_adjustThreshold(void)
{
    switch (dacUpdateState)
    {
        case CURRENT_LIMIT_SET_DAC:
        {
            //Steer CWG to Logic-0
            CWG_DISABLE_OUTPUT();
            
            //Update DAC Value
            DAC1_SetOutput(newDACValue);
            
            //Update State
            dacUpdateState = CURRENT_LIMIT_SETTLE;            
            break;
        }
        case CURRENT_LIMIT_SETTLE:
        {            
            //Release CWG
            CWG_ENABLE_OUTPUT();
            
            //Update State
            dacUpdateState = CURRENT_LIMIT_NO_CHANGE;
            
            //Disable Period Interrupts
            FET_PWM_DISABLE_PERIOD_INT();
            break;
        }
        case CURRENT_LIMIT_NO_CHANGE:
        default:
        {
            //Update State
            dacUpdateState = CURRENT_LIMIT_NO_CHANGE;
            
            //Disable Period Interrupts
            FET_PWM_DISABLE_PERIOD_INT();
        }
    }
}


//Attempt to start the Peltier Regulator. Returns false if an error has occurred
void peltierControl_start(void)
{
    //Update State Machine
    peltierState = PELTIER_STATE_STARTUP;
    
    //Clear Errors
    error = PELTIER_ERROR_NONE;
    
    //NOTE: CWG will start in state machine
    
    //Enable WWDT
    WWDT_start();
    
    //TODO: Add Sanity Checking
#ifdef DEBUG_PRINT
    compactPrint_sendStringWithNewline("-- PELTIER STARTUP --");
#endif
}

//Stop the Peltier Regulator
void peltierControl_stop(void)
{   
#ifdef DEBUG_PRINT
    if (error != PELTIER_ERROR_NONE)
    {
        compactPrint_sendErrorCode("Peltier Control Error, Code ", error);
    }
#endif
    
    //Disable Output
    CWG_DISABLE_OUTPUT();

    //Disable WWDT
    WWDT_stop();
    
    if (peltierState != PELTIER_STATE_DISABLED)
    {
        //Update State Machine
        peltierState = PELTIER_STATE_DISABLED;
        
#ifdef DEBUG_PRINT
        compactPrint_sendStringWithNewline("-- PELTIER OFF --");
#endif
    }
}

void peltierControl_fastStop(void)
{
    //Disable Output
    CWG_DISABLE_OUTPUT();
    
    //Note, peltierState is not set here so the message prints later
}

//Set the max current through the loop
//void peltierControl_setMaxCurrent(uint8_t lim)
//{
//    static uint8_t counter = 10;
//    newDACValue = counter;
//    
//    counter++;
//    
//    if (counter == 0)
//        counter = 10;
//    
//    dacUpdateState = CURRENT_LIMIT_SET_DAC;
//    FET_PWM_ENABLE_PERIOD_INT();
//}

//Returns the error code from the Peltier regulator. Does NOT clear the error
PeltierError peltierControl_getError(void)
{
    return error;
}

