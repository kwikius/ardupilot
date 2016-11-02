
#include <AP_HAL/AP_HAL.h>

#if CONFIG_HAL_BOARD == HAL_BOARD_QUAN

#if defined QUAN_AERFLITE_BOARD

#include <stm32f4xx.h>
#include <quan/stm32/spi.hpp>
#include <quan/stm32/gpio.hpp>
#include <quan/stm32/tim.hpp>
#include <quan/stm32/tim/temp_reg.hpp>
#include <quan/stm32/get_raw_timer_frequency.hpp>
#include <quan/stm32/rcc.hpp>
#include <quan/three_d/vect.hpp>
#include <quan/stm32/millis.hpp>
#include <quan/stm32/rcc.hpp>
#include <quan/stm32/f4/exti/set_exti.hpp>
#include <quan/stm32/f4/syscfg/module_enable_disable.hpp>
#include <quan/stm32/push_pop_fp.hpp>
#include <cmath>
#include <Filter/LowPassFilter2p.h>
#include "task.h"
#include "semphr.h"
#include <AP_Math/vector3_volatile.h>
#include "imu_task.hpp"
#include "bmi_160.hpp"

extern const AP_HAL::HAL& hal;

float Quan::bmi160::accel_constant;
float Quan::bmi160::gyro_constant;

// NB output data rate must be an integer multiple of main loop rate
uint32_t Quan::bmi160::output_data_rate_Hz = 800;
uint32_t Quan::bmi160::main_loop_rate_Hz = 50;

namespace {
  
   void inertial_sensor_watchdog_init();
   void inertial_sensor_watchdog_enable();

   typedef Vector3<volatile float> vvect3;
   typedef LowPassFilter2p<vvect3> lp_filter;

   // the current filtered values
   lp_filter  accel_filter{};
   lp_filter  gyro_filter{};

   QueueHandle_t h_imu_args_queue = nullptr;

   uint32_t dma_lisr_flags = 0;
   uint32_t dma_hisr_flags = 0;

       // the data to go to the ArduPilot loop
   struct inertial_sensor_args_t{
      vvect3  accel;
      vvect3  gyro;
   };

   uint32_t num_irqs_for_update_message = 1;

} // ~namespace

namespace Quan{ 
   // called in the hal.run loop just before the main loop
   // to init the peripherals
   // could pass the main loop rate
   void init_spi()
   {
      h_imu_args_queue = xQueueCreate(1,sizeof(inertial_sensor_args_t *));

      Quan::spi::setup();
      if (! Quan::bmi160::setup()){
         AP_HAL::panic("bmi160 IMU setup failed\n");
      }
      // num irqs depends on bmi setup
      num_irqs_for_update_message = Quan::bmi160::get_num_irqs_for_update_msg();
      // watchdog depends on bmi setup
      // initialise but not start yet
      inertial_sensor_watchdog_init();
   }

   namespace detail{

      // Called from AP_InertialSensor::detect
      // before this point the spi and bmi160 have been setup by calling Quan::init_spi()
      // to init the Quan inertial sensor ( bmi160)
      // sample rate is from frontend, either 50 Hz, 100 Hz, 200 Hz, 400 Hz
      // Default gyro filter  is 20 Hz
      // Default Accel Filter is 20 Hz
      void inertial_sensor_setup(uint16_t main_loop_rate_Hz, uint8_t acc_cutoff_Hz, uint8_t gyro_cutoff_Hz)
      {
         hal.console->printf("inertial sensor startup mainloop = %u, acc cutoff = %u, gyr cutoff = %u\n"
         ,main_loop_rate_Hz, acc_cutoff_Hz, gyro_cutoff_Hz);
         if (! Quan::bmi160::is_initialised()){
            AP_HAL::panic("IMU has not been setup");
         }
         // sample rate  the main loop rate
         if (! Quan::bmi160::set_main_loop_rate_Hz(main_loop_rate_Hz)){
            AP_HAL::panic("Cannot set IMU main loop rate");
         }
         uint32_t const irq_freq_Hz = Quan::bmi160::get_output_data_rate_Hz();
         accel_filter.set_cutoff_frequency(irq_freq_Hz,acc_cutoff_Hz);
         gyro_filter.set_cutoff_frequency(irq_freq_Hz,gyro_cutoff_Hz); 

         
         vTaskSuspendAll();
         {
//#####################################
            Quan::spi::enable_rx_dma();
//#######################################
            quan::stm32::clear_event_pending<Quan::bmi160::not_DR>();
            Quan::bmi160::enable_interrupt_from_device();
            quan::stm32::enable_exti_interrupt<Quan::bmi160::not_DR>();
//##################### TODO Make sure to enable before flight  but off for tsesting #####################
            //inertial_sensor_watchdog_enable();
//#################################################################
         }
         xTaskResumeAll();
         hal.console->println("inertial sensor started");
      }
   } // Quan::detail

} // Quan

namespace {
   typedef quan::stm32::tim7 mpu_watchdog;

   uint32_t irq_count_led = 0;

   inertial_sensor_args_t imu_args;
   volatile uint32_t irq_count = 0;

   volatile uint32_t rx_buffer_idx = 0;
}

/*
 1) Bidi mode enable irq on rxne
2) in rx ie disable rxie irq and make single dir
   and start rx dma
*/
// data ready nterrupt from IMU
extern "C" void EXTI15_10_IRQHandler() __attribute__ ((interrupt ("IRQ")));
extern "C" void EXTI15_10_IRQHandler()
#if 1
{      
   if (quan::stm32::is_event_pending<Quan::bmi160::not_DR>()){

      Quan::spi::disable();
      // reset the watchdog
      mpu_watchdog::get()->cnt = 0; 
      mpu_watchdog::get()->sr = 0;

      Quan::spi::cs_assert<Quan::bmi160::not_CS>(); // start transaction

      quan::stm32::clear_event_pending<Quan::bmi160::not_DR>();

      DMA2->LIFCR |= ( 0b111101 << 0) ; // Stream 0 clear flags
       
      DMA2_Stream0->NDTR = Quan::bmi160::dma_buffer_size; // RX

      DMA2_Stream0->CR |= (1 << 0); // (EN) enable DMA rx

      Quan::spi::enable();
      while (!Quan::spi::txe()){;}
      rx_buffer_idx = 0;
      Quan::spi::ll_read();
      Quan::spi::ll_write(Quan::bmi160::reg::gyro_data_lsb | 0x80);
      Quan::spi::enable_txeie();
   }else{
      uint32_t const exti_pr_reg = quan::stm32::exti::get()->pr.get();
      // mask interrupts to prevent recur forever
      quan::stm32::exti::get()->emr.set(0);
      // clear interrupt flags
      quan::stm32::exti::get()->pr.set(0);
      AP_HAL::panic("unknown EXTI15_10 event exti.pr = %lu\n", exti_pr_reg);
   }
}
#else

{
    quan::stm32::push_FPregs();
    union u{
      struct{
         quan::three_d::vect<int16_t> gyr;
         quan::three_d::vect<int16_t> acc;
      }v;
      uint8_t arr[12];
      u(){ }
   }u;
   Quan::bmi160::read(Quan::bmi160::reg::gyro_data_lsb,u.arr,12);

   float const gyro_k = Quan::bmi160::get_gyro_constant();
   Vector3f const gyro { 
       u.v.gyr.x * gyro_k
      ,u.v.gyr.y * gyro_k
      ,u.v.gyr.z * gyro_k
   };

   float const accel_k = Quan::bmi160::get_accel_constant();
   Vector3f const accel { 
       u.v.acc.x * accel_k
      ,u.v.acc.y * accel_k
      ,u.v.acc.z * accel_k
   };

   BaseType_t HigherPriorityTaskWoken_imu = pdFALSE;

   bool applied = false;
   if ( ++irq_count == num_irqs_for_update_message){
       irq_count = 0;
       if ( xQueueIsQueueEmptyFromISR(h_imu_args_queue)){
          imu_args.accel = accel_filter.apply(accel);
          imu_args.gyro = gyro_filter.apply(gyro);
        //  imu_args.accel = accel;
        //  imu_args.gyro = gyro;
          auto*  p_imu_args = &imu_args;
          xQueueSendToBackFromISR(h_imu_args_queue,&p_imu_args,&HigherPriorityTaskWoken_imu);
          applied = true;
      }
   }
   if (!applied){  // need to apply to filter
      // we dont need to go through the whole rigmarole
      // just update the filter
       accel_filter.apply(accel);
       gyro_filter.apply(gyro);
   }

   quan::stm32::clear_event_pending<Quan::bmi160::not_DR>();
   quan::stm32::pop_FPregs();
}
#endif

namespace Quan{

   // called by AP_InertailSensor::wait_for_sample
   // blocks 
   bool wait_for_imu_sample(uint32_t usecs_to_wait)
   {
      if (h_imu_args_queue != nullptr){
         inertial_sensor_args_t * p_dummy_result;
         TickType_t const ms_to_wait = usecs_to_wait / 1000  + (((usecs_to_wait % 1000) > 499 )?1:0);
        
         if (xQueuePeek(h_imu_args_queue,&p_dummy_result,ms_to_wait) == pdTRUE) {
            return true;
         }
      }
      return false;
   }

   // called by AP_inertialSensor_Backend::update ( quan version)
   // after AP_InertialSensor::wait_for_sample has unblocked
   bool update_ins(Vector3f & accel,Vector3f & gyro)
   {
      if (h_imu_args_queue != nullptr){
         inertial_sensor_args_t * p_args;
         if (xQueueReceive(h_imu_args_queue,&p_args,0) == pdTRUE ){
            accel = p_args->accel;
            gyro = p_args->gyro;
            return true;
         }
      }
      return false;
   }

} // Quan

extern "C" void  SPI1_IRQHandler() __attribute__ ((interrupt ("IRQ")));
extern "C" void  SPI1_IRQHandler()
{
  if ( (++ rx_buffer_idx) < Quan::bmi160::dma_buffer_size){
     Quan::spi::ll_write(0U);
  }else{
     Quan::spi::disable_txeie();
  }
}

// RX DMA complete
extern "C" void DMA2_Stream0_IRQHandler() __attribute__ ((interrupt ("IRQ")));
extern "C" void DMA2_Stream0_IRQHandler()
{
   quan::stm32::push_FPregs();

   if (++ irq_count_led == 400){
      irq_count_led =0;
      hal.gpio->toggle(1);
   }

   dma_lisr_flags = DMA2->LISR;
   dma_hisr_flags = DMA2->HISR;

   DMA2_Stream5->CR &= ~(1 << 0); // (EN) disable DMA
   DMA2_Stream0->CR &= ~(1 << 0); // (EN) disable DMA

   union {
         uint8_t arr[2];
         int16_t val;
   } u;

   float const gyro_k = Quan::bmi160::get_gyro_constant();
   Vector3f gyro ;

   u.arr[0] = Quan::bmi160::dma_rx_buffer[1];
   u.arr[1] = Quan::bmi160::dma_rx_buffer[2];
   gyro.x = u.val * gyro_k;
   u.arr[0] = Quan::bmi160::dma_rx_buffer[3];
   u.arr[1] = Quan::bmi160::dma_rx_buffer[4];
   gyro.y = u.val * gyro_k;
   u.arr[0] = Quan::bmi160::dma_rx_buffer[5];
   u.arr[1] = Quan::bmi160::dma_rx_buffer[6];
   gyro.z = u.val * gyro_k;


   float const accel_k = Quan::bmi160::get_accel_constant();
   Vector3f  accel;
   u.arr[0] = Quan::bmi160::dma_rx_buffer[7];
   u.arr[1] = Quan::bmi160::dma_rx_buffer[8];
   accel.x = u.val * accel_k;
   u.arr[0] = Quan::bmi160::dma_rx_buffer[9];
   u.arr[1] = Quan::bmi160::dma_rx_buffer[10];
   accel.y = u.val * accel_k;
   u.arr[0] = Quan::bmi160::dma_rx_buffer[11];
   u.arr[1] = Quan::bmi160::dma_rx_buffer[12];
   accel.z = u.val * accel_k;

   BaseType_t HigherPriorityTaskWoken_imu = pdFALSE;

   bool applied = false;
   if ( ++irq_count == num_irqs_for_update_message){
       irq_count = 0;
       if ( xQueueIsQueueEmptyFromISR(h_imu_args_queue)){
         // imu_args.accel = accel_filter.apply(accel);
        //  imu_args.gyro = gyro_filter.apply(gyro);
          imu_args.accel = accel;
          imu_args.gyro = gyro;
          auto*  p_imu_args = &imu_args;
          xQueueSendToBackFromISR(h_imu_args_queue,&p_imu_args,&HigherPriorityTaskWoken_imu);
          applied = true;
//          if (irq_count_led == 0){
//             irq_count_led = 1;
//             hal.gpio->write(1,1);  
//          }
      }
   }
   if (!applied){  // need to apply to filter
      // we dont need to go through the whole rigmarole
      // just update the filter
       accel_filter.apply(accel);
       gyro_filter.apply(gyro);
   }
 
   while( (DMA2_Stream5->CR | DMA2_Stream0->CR ) & (1 << 0) ){;}
// 
//   dma_lisr_flags = DMA2->LISR;
//   dma_hisr_flags = DMA2->HISR;

   DMA2->HIFCR |= ( 0b111101 << 6) ; // Stream 5 clear flags
   DMA2->LIFCR |= ( 0b111101 << 0) ; // Stream 0 clear flags

   Quan::spi::cs_release<Quan::bmi160::not_CS>();
   quan::stm32::pop_FPregs();

   portEND_SWITCHING_ISR(HigherPriorityTaskWoken_imu);
}

namespace {

   // start watchdog 
   void inertial_sensor_watchdog_enable()
   {
      mpu_watchdog::get()->cr1.bb_setbit<0>(); // (CEN)
   }

   // n.b requires to be enabled to start running
   // just setup here
   void inertial_sensor_watchdog_init()
   {
      quan::stm32::module_enable<mpu_watchdog>();
      constexpr uint32_t timer_freq = quan::stm32::get_raw_timer_frequency<mpu_watchdog>();
      constexpr uint32_t psc = (timer_freq / static_cast<uint32_t>(1000000U)) - 1U;
      static_assert((timer_freq % static_cast<uint32_t>(1000000U))==0U,"unexpected raw timer frequency");
      mpu_watchdog::get()->psc = psc;
      mpu_watchdog::get()->cnt = 0;
      uint32_t odr = Quan::bmi160::get_output_data_rate_Hz();
      if ( odr == 0){
         AP_HAL::panic("invalid odr in watch dog init");
      }
      // allows  + 5 % error of bmi160 clock. BMI160 claims +- 1%
      mpu_watchdog::get()->arr =  static_cast<uint32_t>(1050000.0 / odr ); 
      mpu_watchdog::get()->sr = 0;
      mpu_watchdog::get()->dier.setbit<0>(); //(UIE)  

      NVIC_SetPriority(TIM7_IRQn,13);
      NVIC_EnableIRQ(TIM7_IRQn);
      // n.b watchdog not started here
   }
}

// watchdog interrupt
extern "C" void TIM7_IRQHandler() __attribute__ ((interrupt ("IRQ")));
extern "C" void TIM7_IRQHandler()
{
   mpu_watchdog::get()->sr = 0;
   mpu_watchdog::get()->cnt = 0;

   quan::stm32::generate_software_interrupt<Quan::bmi160::not_DR>();

   BaseType_t higher_prio_task_woken = pdFALSE;
   portEND_SWITCHING_ISR(higher_prio_task_woken);
}

#endif  // !defined QUAN_AERFLITE_BOARD

#endif // CONFIG_HAL_BOARD == HAL_BOARD_QUAN
