
#include <AP_HAL/AP_HAL.h>
#if CONFIG_HAL_BOARD == HAL_BOARD_QUAN
#include <quantracker/osd/osd.hpp>
#include <task.h>

#include "HAL_Quan_Class.h"
#include "AP_HAL_Quan_Private.h"

#include <AP_HAL_Quan/AP_HAL_Quan.h>
#include <AP_HAL_Quan/AP_HAL_Quan_Private.h>
#include <AP_HAL_Quan/i2c_task.hpp>

using namespace Quan;

namespace Quan{

  void init_spi();

  template <uint32_t I>
  AP_HAL::UARTDriver*  get_serial_port();
  AP_HAL::AnalogIn*    get_analog_in();
#if !defined QUAN_AERFLITE_BOARD
  AP_HAL::I2CDriver*   get_i2c_driver();
#endif
  AP_HAL::RCInput*     get_rc_inputs();
  AP_HAL::RCOutput*    get_rc_outputs();

}

static Empty::EmptySPIDeviceManager spiDeviceManager;
static QuanStorage storageDriver;
static QuanGPIO gpioDriver;
//static QuanRCOutput rcoutDriver;
static QuanScheduler schedulerInstance;
static QuanUtil utilInstance;

HAL_Quan::HAL_Quan() 
:AP_HAL::HAL(
   Quan::get_serial_port<0>(),//   uartA console  usart1   3.3V
   Quan::get_serial_port<2>(),//   uartB 1st GPS  uart4    5V  with invert
   Quan::get_serial_port<1>(),//   uartC telemetry usart3  5V  with invert
#if defined QUAN_AERFLITE_BOARD
   Quan::get_serial_port<3>(),//   uartD  usart6           3.3V 
#else
   NULL,            /* no uartD */
#endif
   NULL,            /* no uartE */
#if defined QUAN_AERFLITE_BOARD  /* no exposed i2c for AERFLITE */
   NULL,
 #else
   Quan::get_i2c_driver(), 
#endif
   NULL, /* only one i2c */
   NULL, /* only one i2c */
   &spiDeviceManager, // dummy
   Quan::get_analog_in(),
   &storageDriver,
   Quan::get_serial_port<0>(),    // console  member
   &gpioDriver,
   Quan::get_rc_inputs(),
   Quan::get_rc_outputs(),
   &schedulerInstance,
   &utilInstance
)
{}


void AP_HAL::init() {}

// called at start of the apm_task
void HAL_Quan::run(void * params) const
{   
   start_flags flags{ (uint32_t) params};

   if ( gpio && flags.init_gpio ){
      gpio->init();  //leds
   }

   if ( uartA && flags.init_uartA ){
      uartA->begin(115200);  
   }

   if ( uartB && flags.init_uartB ){
      uartB->begin(115200);
   }

   if ( uartC && flags.init_uartC ){
      uartC->begin(115200);  
   }

   if ( uartD &&  flags.init_uartD ){
      uartD->begin(115200);
   }

   if ( scheduler && flags.init_scheduler ){
      scheduler->init(NULL);
   }

   //inits the spi task
   // scheduler must be running for spi
   if ( flags.init_spi ){
      if ( scheduler && flags.init_scheduler ){
         Quan::init_spi(); 
         if ( spi){spi->init(NULL);} // dummy
      }else{
         AP_HAL::panic("must init scheduler before spi");
      }
   }

   if (rcin && flags.init_rcin ){
      rcin->init(NULL);
   }

   if ( rcout && flags.init_rcout ){
      rcout->init(NULL);
   }

   if ( analogin && flags.init_analogin ){
      analogin->init(NULL);
   }

   //note that the APM i2c driver is null not used
   if ( flags.init_i2c ){
      console->write("starting i2c\n");
      Quan::create_i2c_task();
   }

}

namespace {
   const HAL_Quan hal_quan;
}

const AP_HAL::HAL& AP_HAL::get_HAL() 
{
    return hal_quan;
}

void setup();
void loop();

namespace { 
  // char dummy_param = 0; 
   TaskHandle_t task_handle = NULL; 
   void apm_task(void * params) 
   { 
      hal_quan.run(params); 
      setup();      // this is defined by the app e.g ArduPlane, examples etc
      hal_quan.scheduler->system_initialized();  // just sets a flag to say that initialisation is complete
      for(;;){ 
         loop();   // this is defined by the app e.g ArduPlane, examples etc
      } 
   } 
} 

void create_apm_task( uint32_t params) 
{ 
  xTaskCreate( 
      apm_task,"apm task", 
      4000, 
      (void*)params, 
      tskIDLE_PRIORITY + 1, 
      &task_handle 
  ); 
}

#endif  // CONFIG_HAL_BOARD == HAL_BOARD_QUAN
