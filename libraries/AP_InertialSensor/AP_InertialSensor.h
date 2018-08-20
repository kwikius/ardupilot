
#ifndef AERFPILOT_LIBRARIES_AP_INERTIAL_SENSOR_H_INCLUDED
#define AERFPILOT_LIBRARIES_AP_INERTIAL_SENSOR_H_INCLUDED

#include <stdint.h>
#include <AP_HAL/AP_HAL.h>
#include <AP_Math/AP_Math.h>
#include "AP_InertialSensor_UserInteract.h"
#include <Filter/LowPassFilter.h>
#include <Filter/LowPassFilter2p.h>

class AP_InertialSensor_Backend;

/*
  forward declare DataFlash class. We can't include DataFlash.h
  because of mutual dependencies
 */
class DataFlash_Class;

/* AP_InertialSensor is an abstraction for gyro and accel measurements
 * which are correctly aligned to the body axes and scaled to SI units.
 *
 * Gauss-Newton accel calibration routines borrowed from Rolfe Schmidt
 * blog post describing the method: http://chionophilous.wordpress.com/2011/10/24/accelerometer-calibration-iv-1-implementing-gauss-newton-on-an-atmega/
 * original sketch available at http://rolfeschmidt.com/mathtools/skimetrics/adxl_gn_calibration.pde
 */

class AP_InertialSensor;

template <typename Board> 
AP_InertialSensor_Backend * connect_inertial_sensor_driver(AP_InertialSensor & imu);

class AP_InertialSensor
{
    friend class AP_InertialSensor_Backend;

public:
    static constexpr uint8_t m_max_instances = 3U;
    static constexpr uint8_t m_max_backends = 6;
    static constexpr uint8_t m_vibration_check_instances  = 2U;

    AP_InertialSensor();
   // static AP_InertialSensor *get_instance();

    // the rate that updates will be available to the application
    enum Sample_rate {
        RATE_50HZ  = 50,
        RATE_100HZ = 100,
        RATE_200HZ = 200,
        RATE_400HZ = 400
    };

    enum Gyro_Calibration_Timing {
        GYRO_CAL_NEVER = 0,
        GYRO_CAL_STARTUP_ONLY = 1
    };

    /// Perform startup initialisation.
    ///
    /// Called to initialise the state of the IMU.
    ///
    /// Gyros will be calibrated unless INS_GYRO_CAL is zero
    ///
    /// @param style	The initialisation startup style.
    ///
    void init(Sample_rate sample_rate);

    // update gyro and accel values from accumulated samples
    void update(void);

    // wait for a sample to be available
    void wait_for_sample(void);

    /// Register a new gyro/accel driver, allocating an instance
    /// number
    uint8_t register_gyro(uint16_t raw_sample_rate_hz);
    uint8_t register_accel(uint16_t raw_sample_rate_hz);

    // perform accelerometer calibration including providing user instructions
    // and feedback
    bool calibrate_accel(AP_InertialSensor_UserInteract *interact,
                         float& trim_roll,
                         float& trim_pitch);
    bool calibrate_trim(float &trim_roll, float &trim_pitch);

    /// calibrating - returns true if the gyros or accels are currently being calibrated
    bool calibrating() const { return _calibrating; }

    /// Perform cold-start initialisation for just the gyros.
    ///
    /// @note This should not be called unless ::init has previously
    ///       been called, as ::init may perform other work
    ///
    void init_gyro(void);

    /// Fetch the current gyro values
    ///
    /// @returns	vector of rotational rates in radians/sec
    ///
    // radians/sec
    const Vector3f& get_gyro(void) const { return get_gyro(_primary_gyro); }
    // get gyro offsets in radians/sec
    const Vector3f& get_gyro_offsets(void) const { return get_gyro_offsets(_primary_gyro); }
    bool get_delta_angle(Vector3f &delta_angle) const { return get_delta_angle(_primary_gyro, delta_angle); }
    bool get_delta_velocity(Vector3f &delta_velocity) const { return get_delta_velocity(_primary_accel, delta_velocity); }
    float get_delta_velocity_dt() const { return get_delta_velocity_dt(_primary_accel); }
    const Vector3f     &get_accel(void) const { return get_accel(_primary_accel); }
    bool get_gyro_health(void) const { return get_gyro_health(_primary_gyro); }
    uint8_t get_gyro_count(void) const { return _gyro_count; }
    const Vector3f &get_accel_offsets(void) const { return get_accel_offsets(_primary_accel); }
    bool get_accel_health(void) const { return get_accel_health(_primary_accel); }
    const Vector3f &get_accel_scale(void) const { return get_accel_scale(_primary_accel); }
    // retrieve latest calculated vibration levels
    Vector3f get_vibration_levels() const { return get_vibration_levels(_primary_accel); }
    uint16_t error_count(void) const { return 0; }
    float get_delta_time() const { return _delta_time; }

    void set_accel(const Vector3f &accel){set_accel(_primary_accel,accel);}
    void set_gyro(const Vector3f &gyro){set_gyro(_primary_gyro,gyro);}

    // return the maximum gyro drift rate in radians/s/s. This
    // depends on what gyro chips are being used
    float get_gyro_drift_rate(void) const { return ToRad(0.5f/60); }

    // class level parameters
    static const struct AP_Param::GroupInfo var_info[];

    // set overall board orientation
    void set_board_orientation(enum Rotation orientation) {
        _board_orientation = orientation;
    }

    // return the selected sample rate
    Sample_rate get_sample_rate(void) const { return _sample_rate; }

    bool healthy(void) const { return get_gyro_health() && get_accel_health(); }

    uint8_t get_primary_accel(void) const { return _primary_accel; }
    uint8_t get_primary_gyro(void) const { return _primary_gyro; }

    // enable HIL mode
    void set_hil_mode(void) { _hil_mode = true; }

    // enable/disable raw gyro/accel logging
    void set_raw_logging(bool enable) { _log_raw_data = enable; }

    Gyro_Calibration_Timing gyro_calibration_timing() { return (Gyro_Calibration_Timing)_gyro_cal_timing.get(); }
    uint8_t get_accel_count(void) const { return _accel_count; };

    // get the gyro filter rate in Hz
    uint8_t get_gyro_filter_hz(void) const { return _gyro_filter_cutoff; }

    // get the accel filter rate in Hz
    uint8_t get_accel_filter_hz(void) const { return _accel_filter_cutoff; }
    void set_delta_time(float delta_time);

    // pass in a pointer to DataFlash for raw data logging
    void set_dataflash(DataFlash_Class *dataflash) { _dataflash = dataflash; }

    uint32_t get_gyro_error_count() const { return get_gyro_error_count(_primary_gyro); }
//
    bool get_gyro_health_all(void) const;
    bool gyro_calibrated_ok_all() const;
    bool accel_calibrated_ok_all() const;
    bool get_accel_health_all(void) const;
    uint32_t get_accel_clip_count() const{return get_accel_clip_count(_primary_accel);}

/// todo remove these

    const Vector3f  &get_gyro(uint8_t i) const { return _gyro[i]; }
    bool get_gyro_health(uint8_t instance) const { return (instance<_gyro_count) ? _gyro_healthy[instance] : false; }
    bool get_delta_angle(uint8_t i, Vector3f &delta_angle) const;
    bool get_accel_health(uint8_t instance) const { return (instance<_accel_count) ? _accel_healthy[instance] : false; }
    bool get_delta_velocity(uint8_t i, Vector3f &delta_velocity) const;
    float get_delta_velocity_dt(uint8_t i) const;

    uint32_t get_accel_error_count()const { return get_accel_error_count(_primary_accel);}
    // get accel offsets in m/s/s

    bool use_accel(uint8_t instance) const;
    // gyro accel ?
    float get_temperature() const { return get_temperature(0); }
    // check for vibration movement. True when all axis show nearly zero movement
    bool is_still() const;

    const Vector3f &get_gyro_offsets(uint8_t i) const { return _gyro_offset[i]; }

    const Vector3f &get_accel_offsets(uint8_t i) const { return _accel_offset[i]; }

    /// Fetch the current accelerometer values
    ///
    /// @returns	vector of current accelerations in m/s/s
    ///
    const Vector3f  &get_accel(uint8_t i) const { return _accel[i]; }

    uint32_t get_gyro_error_count(uint8_t i) const { return _gyro_error_count[i]; }
    uint32_t get_accel_error_count(uint8_t i) const { return _accel_error_count[i]; }
    // multi-device interface

    bool gyro_calibrated_ok(uint8_t instance) const { return _gyro_cal_ok[instance]; }

    bool use_gyro(uint8_t instance) const;


    // get accel scale
    const Vector3f &get_accel_scale(uint8_t i) const { return _accel_scale[i]; }
    // return the temperature if supported. Zero is returned if no
    // temperature is available
    float get_temperature(uint8_t instance) const { return _temperature[instance]; }

    /* get_delta_time returns the time period in seconds
     * overwhich the sensor data was collected
     */
    Vector3f get_vibration_levels(uint8_t instance) const;

    // retrieve and clear accelerometer clipping count
    uint32_t get_accel_clip_count(uint8_t instance) const;
private:

    // calculate vibration levels and check for accelerometer clipping (called by a backends)
    void calc_vibration_and_clipping(uint8_t instance, const Vector3f &accel, float dt);

    /*
      HIL set functions. The minimum for HIL is set_accel() and
      set_gyro(). The others are option for higher fidelity log
      playback
     */
    void set_accel(uint8_t instance, const Vector3f &accel);
    void set_gyro(uint8_t instance, const Vector3f &gyro);

    void set_delta_velocity(uint8_t instance, float deltavt, const Vector3f &deltav);
    void set_delta_angle(uint8_t instance, const Vector3f &deltaa);

    void detect_backends(void);

    // load backend drivers
    void _add_backend(AP_InertialSensor_Backend *backend);
    void _start_backends();
    AP_InertialSensor_Backend *_find_backend(int16_t backend_id, uint8_t instance);

    // gyro initialisation
    void _init_gyro();

    // Calibration routines borrowed from Rolfe Schmidt
    // blog post describing the method: http://chionophilous.wordpress.com/2011/10/24/accelerometer-calibration-iv-1-implementing-gauss-newton-on-an-atmega/
    // original sketch available at http://rolfeschmidt.com/mathtools/skimetrics/adxl_gn_calibration.pde

    // _calibrate_accel - perform low level accel calibration
    bool _calibrate_accel(const Vector3f accel_sample[6],
                          Vector3f& accel_offsets,
                          Vector3f& accel_scale,
                          float max_abs_offsets,
                          enum Rotation rotation);
    bool _check_sample_range(const Vector3f accel_sample[6], enum Rotation rotation, 
                             AP_InertialSensor_UserInteract* interact);
    void _calibrate_update_matrices(float dS[6], float JS[6][6], float beta[6], float data[3]);
    void _calibrate_reset_matrices(float dS[6], float JS[6][6]);
    void _calibrate_find_delta(float dS[6], float JS[6][6], float delta[6]);
    bool _calculate_trim(const Vector3f &accel_sample, float& trim_roll, float& trim_pitch);

    // save parameters to eeprom
    void  _save_parameters();

    // backend objects
    AP_InertialSensor_Backend *_backends[m_max_backends];

    // number of gyros and accel drivers. Note that most backends
    // provide both accel and gyro data, so will increment both
    // counters on initialisation
    uint8_t _gyro_count;
    uint8_t _accel_count;
    uint8_t _backend_count;

    // the selected sample rate
    Sample_rate _sample_rate;
    
    // Most recent accelerometer reading
    Vector3f _accel[m_max_instances];
    Vector3f _delta_velocity[m_max_instances];
    float _delta_velocity_dt[m_max_instances];
    bool _delta_velocity_valid[m_max_instances];
    // delta velocity accumulator
    Vector3f _delta_velocity_acc[m_max_instances];
    // time accumulator for delta velocity accumulator
    float _delta_velocity_acc_dt[m_max_instances];

    // Low Pass filters for gyro and accel
    LowPassFilter2pVector3f _accel_filter[m_max_instances];
    LowPassFilter2pVector3f _gyro_filter[m_max_instances];
    Vector3f _accel_filtered[m_max_instances];
    Vector3f _gyro_filtered[m_max_instances];
    bool _new_accel_data[m_max_instances];
    bool _new_gyro_data[m_max_instances];
    
    // Most recent gyro reading
    Vector3f _gyro[m_max_instances];
    Vector3f _delta_angle[m_max_instances];
    bool _delta_angle_valid[m_max_instances];
    Vector3f _delta_angle_acc[m_max_instances];
    Vector3f _last_delta_angle[m_max_instances];
    Vector3f _last_raw_gyro[m_max_instances];

    // product id
    AP_Int16 _product_id;

    // accelerometer scaling and offsets
    AP_Vector3f _accel_scale[m_max_instances];
    AP_Vector3f _accel_offset[m_max_instances];
    AP_Vector3f _gyro_offset[m_max_instances];

    // accelerometer max absolute offsets to be used for calibration
    float _accel_max_abs_offsets[m_max_instances];

    // accelerometer and gyro raw sample rate in units of Hz
    uint16_t _accel_raw_sample_rates[m_max_instances];
    uint16_t _gyro_raw_sample_rates[m_max_instances];

    // temperatures for an instance if available
    float _temperature[m_max_instances];

    // filtering frequency (0 means default)
    AP_Int8     _accel_filter_cutoff;
    AP_Int8     _gyro_filter_cutoff;
    AP_Int8     _gyro_cal_timing;

    // use for attitude, velocity, position estimates
    AP_Int8     _use[m_max_instances];

    // board orientation from AHRS
    enum Rotation _board_orientation;

    // calibrated_ok flags
    bool _gyro_cal_ok[m_max_instances];

    // primary accel and gyro
    uint8_t _primary_gyro;
    uint8_t _primary_accel;

    // has wait_for_sample() found a sample?
    bool _have_sample:1;

    // are we in HIL mode?
    bool _hil_mode:1;

    // are gyros or accels currently being calibrated
    bool _calibrating:1;

    // should we log raw accel/gyro data?
    bool _log_raw_data:1;

    bool _backends_detected:1;

    // the delta time in seconds for the last sample
    float _delta_time;

    // last time a wait_for_sample() returned a sample
    uint32_t _last_sample_usec;

    // target time for next wait_for_sample() return
    uint32_t _next_sample_usec;
    
    // time between samples in microseconds
    uint32_t _sample_period_usec;

    // health of gyros and accels
    bool _gyro_healthy[m_max_instances];
    bool _accel_healthy[m_max_instances];

    uint32_t _accel_error_count[m_max_instances];
    uint32_t _gyro_error_count[m_max_instances];

    // vibration and clipping
    uint32_t _accel_clip_count[m_max_instances];
    LowPassFilterVector3f _accel_vibe_floor_filter[m_vibration_check_instances];
    LowPassFilterVector3f _accel_vibe_filter[m_vibration_check_instances];

    // threshold for detecting stillness
    AP_Float _still_threshold;

    /*
      state for HIL support
     */
    struct {
        float delta_time;
    } _hil {};

    DataFlash_Class *_dataflash;

};


#endif  // AERFPILOT_LIBRARIES_AP_INERTIAL_SENSOR_H_INCLUDED
