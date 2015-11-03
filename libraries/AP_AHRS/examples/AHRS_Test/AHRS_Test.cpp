// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-

//
// Simple test for the AP_AHRS interface
//

#include <AP_HAL/AP_HAL.h>
#include <AP_Common/AP_Common.h>
#include <AP_Progmem/AP_Progmem.h>
#include <AP_Math/AP_Math.h>
#include <AP_Param/AP_Param.h>
#include <AP_InertialSensor/AP_InertialSensor.h>
#include <AP_ADC/AP_ADC.h>
#include <AP_ADC_AnalogSource/AP_ADC_AnalogSource.h>
#include <AP_Baro/AP_Baro.h>            // ArduPilot Mega Barometer Library
#include <AP_GPS/AP_GPS.h>
#include <AP_AHRS/AP_AHRS.h>
#include <AP_Compass/AP_Compass.h>
#include <AP_Declination/AP_Declination.h>
#include <AP_Airspeed/AP_Airspeed.h>
#include <AP_Baro/AP_Baro.h>
#include <GCS_MAVLink/GCS_MAVLink.h>
#include <AP_Mission/AP_Mission.h>
#include <StorageManager/StorageManager.h>
#include <AP_Terrain/AP_Terrain.h>
#include <Filter/Filter.h>
#include <SITL/SITL.h>
#include <AP_Buffer/AP_Buffer.h>
#include <AP_Notify/AP_Notify.h>
#include <AP_Vehicle/AP_Vehicle.h>
#include <DataFlash/DataFlash.h>
#include <AP_NavEKF/AP_NavEKF.h>
#include <AP_Rally/AP_Rally.h>
#include <AP_Scheduler/AP_Scheduler.h>

#include <AP_HAL_AVR/AP_HAL_AVR.h>
#include <AP_HAL_SITL/AP_HAL_SITL.h>
#include <AP_HAL_Empty/AP_HAL_Empty.h>
#include <AP_HAL_Linux/AP_HAL_Linux.h>
#include <AP_HAL_PX4/AP_HAL_PX4.h>
#include <AP_HAL_Quan/AP_HAL_Quan.h>
#include <AP_BattMonitor/AP_BattMonitor.h>
#include <AP_SerialManager/AP_SerialManager.h>
#include <RC_Channel/RC_Channel.h>
#include <AP_RangeFinder/AP_RangeFinder.h>
#include <AP_OSD/AP_OSD_enqueue.h>

const AP_HAL::HAL& hal = AP_HAL_BOARD_DRIVER;

// INS and Baro declaration
AP_InertialSensor ins;

#if CONFIG_HAL_BOARD == HAL_BOARD_APM1
AP_ADC_ADS7844 apm1_adc;
#endif

Compass compass;

AP_GPS gps;
AP_Baro baro;
AP_SerialManager serial_manager;
AP_BattMonitor  battery_monitor;

static AP_Vehicle::FixedWing aparm;

AP_Airspeed airspeed(aparm);

// choose which AHRS system to use
AP_AHRS_DCM  ahrs(ins, baro, gps);

#define HIGH 1
#define LOW 0

void setup(void)
{

#ifdef APM2_HARDWARE
    // we need to stop the barometer from holding the SPI bus
    hal.gpio->pinMode(40, HAL_HAL_GPIO_OUTPUT);
    hal.gpio->write(40, HIGH);
#endif
    battery_monitor.init();
    ins.init(AP_InertialSensor::RATE_50HZ);
    ahrs.init();
    serial_manager.init();
    AP_OSD::enqueue::initialise();
    baro.init();
    baro.calibrate();

    airspeed.init();
    airspeed.calibrate(false);
    

    if( compass.init() ) {
        hal.console->printf("Enabling compass\n");
        compass.set_offsets(0, {10.6004,-35.4656,-48.9482});
        ahrs.set_compass(&compass);
    } else {
        hal.console->printf("No compass detected\n");
    }
    gps.init(NULL, serial_manager);
 
}

namespace {

  uint16_t print_counter =0;
  uint16_t counter10_Hz;
 
}

void loop(void)
{
    
    ins.wait_for_sample();
    float heading = 0;
    if (++counter10_Hz == 5){
        counter10_Hz = 0;
        compass.read();
        heading = compass.calculate_heading(ahrs.get_dcm_matrix());
        AP_OSD::enqueue::heading(ToDeg(heading));

        baro.update();
        AP_OSD::enqueue::baro_altitude(baro.get_altitude());

        gps.update();
        AP_OSD::enqueue::gps_status(gps.status());
        AP_OSD::enqueue::gps_location(
            {gps.location().lat,gps.location().lng,gps.location().alt}
        );

        airspeed.read();
        AP_OSD::enqueue::airspeed(airspeed.get_airspeed());

        battery_monitor.read();
        AP_OSD::enqueue::battery(
            {battery_monitor.voltage(),
               battery_monitor.current_amps(),
                  battery_monitor.current_total_mah()}
        );

    }

    ahrs.update();
    AP_OSD::enqueue::attitude({ToDeg(ahrs.pitch),ToDeg(ahrs.roll),ToDeg(ahrs.yaw)});

    if (++print_counter == 20) {
      print_counter = 0;
      Vector3f drift  = ahrs.get_gyro_drift();
      hal.console->printf_P(
         PSTR("r:%4.1f  p:%4.1f y:%4.1f "
         "drift=(%5.1f %5.1f %5.1f) hdg=%.1f \n"),
            static_cast<double>(ToDeg(ahrs.roll)),
            static_cast<double>(ToDeg(ahrs.pitch)),
            static_cast<double>(ToDeg(ahrs.yaw)),
            static_cast<double>(ToDeg(drift.x)),
            static_cast<double>(ToDeg(drift.y)),
            static_cast<double>(ToDeg(drift.z)),
            compass.use_for_yaw() ? static_cast<double>(ToDeg(heading)) : 0.0
      );
   }
}

AP_HAL_MAIN();
