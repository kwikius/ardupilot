

#include <AP_HAL/AP_HAL.h>
#if CONFIG_HAL_BOARD == HAL_BOARD_QUAN


#include <stm32f4xx.h>
#include <quan/stm32/spi.hpp>
#include <quan/stm32/gpio.hpp>
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
#include <quan/acceleration.hpp>
#include <quan/angle.hpp>
#include <quan/reciprocal_time.hpp>
#include "imu_task.hpp"

extern const AP_HAL::HAL& hal;

extern "C" void DMA2_Stream0_IRQHandler() __attribute__ ((interrupt ("IRQ")));
extern "C" void EXTI15_10_IRQHandler() __attribute__ ((interrupt ("IRQ")));

namespace {

   // handle to the  1 element ins args queue
   QueueHandle_t h_INS_ArgsQueue = 0U;

   void panic(const char* text)
   {
      hal.scheduler->panic(text);
   }

   void hal_printf(const char* text)
   {
      taskENTER_CRITICAL();
      hal.console->write(text);
      taskEXIT_CRITICAL();
   }
  
   // ignore the hal function for delay
   // actually is only used in apm_task for setup
   // so would be ok
   TickType_t wakeup_time = 0;
   void delay( uint32_t n)
   {
      vTaskDelayUntil(&wakeup_time,n);
   }

  void mpu6000_init();

  struct spi_device_driver  {

      typedef quan::stm32::spi1 spi1;

      static void init() 
      {
         quan::stm32::rcc::get()->apb2enr.bb_setbit<12>(); 
         quan::stm32::rcc::get()->apb2rstr.bb_setbit<12>();
         quan::stm32::rcc::get()->apb2rstr.bb_clearbit<12>();
         setup_spi_pins();
         setup_spi_regs(); 
         mpu6000_init();
         start_spi();
      }
private:
       static uint8_t transfer(uint8_t data)
       {
         while (!txe()){;}
         ll_write(data);
         while ( !rxne() ){;}
         return ll_read();
       }

       static void transfer (const uint8_t *tx, uint8_t* rx, uint16_t len)
       {
          for ( uint16_t i = 0; i < len; ++i){
            rx[i]= transfer(tx[i]);
          }
       }
 public:  

      template <typename nCS_Pin>
      static void cs_assert()
      {
         quan::stm32::clear<nCS_Pin>();
         (void)ll_read();
      }

      // worst case 1 cycle at 168 Mhz == 6 ns
      template <typename nCS_Pin>
      static void cs_release()
      {
         while (busy()) { asm volatile ("nop":::);}
         if ( !m_fast_speed){
            // 500 ns hold
            for (uint8_t i = 0; i < 50; ++i){
               asm volatile ("nop":::);
            }
         }
         quan::stm32::set<nCS_Pin>();
      }
       template <typename CS_Pin>
       static bool transaction_no_crit(const uint8_t *tx, uint8_t *rx, uint16_t len) 
       {
         cs_assert<CS_Pin>();
         transfer(tx,rx,len);
         cs_release<CS_Pin>();
         return true;
       }

       template <typename CS_Pin>
       static bool transaction(const uint8_t *tx, uint8_t *rx, uint16_t len) 
       {
         taskENTER_CRITICAL();
         bool const result = transaction_no_crit<CS_Pin>(tx,rx,len);
         taskEXIT_CRITICAL();
         return result;
       }
   private:
      static constexpr uint16_t spi_slow_brr = (0b110 << 3);
      static constexpr uint16_t spi_fast_brr = (0b001 << 3);
      static constexpr uint16_t spi_brr_and_clear_mask = ~(0b111 << 3);
   public:
      static void set_bus_speed(int speed) 
      {
         switch (speed){
      
            case 0: 
               if ( m_fast_speed){
                  spi1::get()->cr1 = (spi1::get()->cr1 & spi_brr_and_clear_mask) | spi_slow_brr;
                  m_fast_speed = false;
               }
               break;

            case 1:
               if ( m_fast_speed == false){
                  spi1::get()->cr1 = (spi1::get()->cr1 & spi_brr_and_clear_mask) | spi_fast_brr;
                  m_fast_speed = true;
               }
               break;
            default:
               break;
         }
      }

      static void stop_spi()
      {
         spi1::get()->cr1.bb_clearbit<6>(); //( SPE)
      }

      static void start_spi()
      {
         spi1::get()->cr1.bb_setbit<6>(); //( SPE)
      }

      static bool txe(){ return spi1::get()->sr.bb_getbit<1>();}
      static bool rxne(){ return spi1::get()->sr.bb_getbit<0>();}
      static bool busy(){ return spi1::get()->sr.bb_getbit<7>();}
private:

      static uint8_t ll_read() { return spi1::get()->dr;}
      static void ll_write( uint8_t val){ spi1::get()->dr = val;}
      // Quan::QuanSemaphore m_semaphore;
      static bool m_fast_speed;

      typedef quan::mcu::pin<quan::stm32::gpiob,5>  spi1_mosi;
      typedef quan::mcu::pin<quan::stm32::gpiob,4>  spi1_miso;
      typedef quan::mcu::pin<quan::stm32::gpiob,3>  spi1_sck;
public:

      typedef quan::mcu::pin<quan::stm32::gpiob,1> fram_ncs;
private:
      static void setup_spi_pins()
      {
         //spi1
         quan::stm32::module_enable<quan::stm32::gpioa>();
         quan::stm32::module_enable<quan::stm32::gpiob>();
         quan::stm32::module_enable<quan::stm32::gpioc>();

         quan::stm32::apply<
            spi1_mosi
            ,quan::stm32::gpio::mode::af5  
            ,quan::stm32::gpio::pupd::none
            ,quan::stm32::gpio::ospeed::medium_fast
         >();

         quan::stm32::apply<
            spi1_miso
            ,quan::stm32::gpio::mode::af5  
            ,quan::stm32::gpio::pupd::none
         >();

         quan::stm32::apply<
            spi1_sck
            ,quan::stm32::gpio::mode::af5  
            ,quan::stm32::gpio::pupd::none
            ,quan::stm32::gpio::ospeed::medium_fast
         >();

        quan::stm32::apply<
            fram_ncs
            ,quan::stm32::gpio::mode::output
            ,quan::stm32::gpio::pupd::none
            ,quan::stm32::gpio::ospeed::medium_fast
            ,quan::stm32::gpio::ostate::high
         >();
      }

      static void setup_spi_regs()
      {
         // SPE = false // periphal disabled for now?
         // LSBFIRST =  false, 
         // RXONLY = false
         // DFF = false 8  bit
         // crcnext = false
         // crcen = false
         // BIDIMODE = false; // 2 line mode
         // BIDIOE = false ;//dont care
         spi1::get()->cr1 = 
         ( 1 << 0)      // (CPHA)
         | ( 1 << 1)      // (CPOL)
         | ( 1 << 2)      // (MSTR)
         |  spi_slow_brr  // (BRR)
         |  (1 << 8)      // (SSI)
         |  (1 << 9)      // (SSM)
         ;
      }

public:
      static void enable_dma()
      { 
         spi1::get()->cr2 |= (( 1 << 1 ) | ( 1 << 0)) ;// (TXDMAEN ) | ( RXDMAEN)
      }

      static void disable_dma()
      { 
         spi1::get()->cr2 &= ~(( 1 << 1 ) | ( 1 << 0)) ;// (TXDMAEN ) | ( RXDMAEN)
      }
            
   };

   bool spi_device_driver::m_fast_speed = false;

//-----------------------------------------------------------------------

   struct mpu6000{

      typedef Vector3<volatile float> vvect3;
      typedef LowPassFilter2p<vvect3> lp_filter;

      static lp_filter  gyro_filter;
      static lp_filter  accel_filter;

   // number of irqs before the  MPU ready irqhandler
   // unblocks the AP_InertialSensor::wait_for_sample function
   // to fulfill the requested sample frequency 
      static uint32_t num_irqs_for_update_message ;

   // accel should be in units of m.s-2
   // gyro should be in units of rad.s
      struct inertial_sensor_args_t{
         Vector3<volatile float>  accel;
         Vector3<volatile float>  gyro;
      };

      // required gyro rate
      // as in Ap_InertialSensor_MPU6000
      // can be 250, 500, 100,2000 degrees.sec[-1]
      static constexpr uint32_t gyro_fsr_deg_s = 2000U;
      // to scale from the raw reg output to rad.s-1 
      static constexpr float gyro_constant(uint32_t gyro_fsr_deg_s_in)
      {
         typedef quan::angle::deg deg;
         typedef quan::angle::rad rad;
         typedef quan::reciprocal_time_<deg>::per_s deg_per_s;
         typedef quan::reciprocal_time_<rad>::per_s rad_per_s;
         return static_cast<float>(((rad_per_s{deg_per_s{deg{gyro_fsr_deg_s_in}}}/32768).numeric_value()).numeric_value());
      };
      // accel full scale
      // can be 2 , 4 , 8 , 16 g
      static constexpr uint32_t accel_fsr_g = 8U;
      // to scale from the raw reg output to m.s-2
      static constexpr float accel_constant(uint32_t accel_fsr_g_in)
      {
        return static_cast<float>(((quan::acceleration::g * accel_fsr_g_in)/32768).numeric_value());
      };

      typedef quan::mcu::pin<quan::stm32::gpioa,12> chip_select;
      typedef quan::mcu::pin<quan::stm32::gpioc,14> data_ready_irq;

      static void init()
      {
         setup_pins();
         setup_exti();
         setup_dma();
      }

       // imu register values
      struct val{
         static constexpr uint8_t device_wakeup = 0U;
         static constexpr uint8_t device_reset = (1 << 7);
         static constexpr uint8_t i2c_if_dis = (1U << 4U);
        // for MPU9250 
         // static constexpr uint8_t whoami = 0x71;
         // for MPU6000
         static constexpr uint8_t whoami =   0x68  ;
      };

      // imu register indexes
      struct reg{
         static constexpr uint8_t product_id          = 12U;
         static constexpr uint8_t sample_rate_div     = 25U;
         static constexpr uint8_t config              = 26U;
         static constexpr uint8_t gyro_config         = 27U;
         static constexpr uint8_t accel_config        = 28U;
         static constexpr uint8_t fifo_enable         = 35U;
         static constexpr uint8_t intr_bypass_en_cfg  = 55U;
         static constexpr uint8_t intr_enable         = 56U;
         static constexpr uint8_t intr_status         = 58U;
         static constexpr uint8_t accel_measurements  = 59U; // 59 to 64
         static constexpr uint8_t temp_measurements   = 65U; // 65 to 66
         static constexpr uint8_t gyro_measurements   = 67U; // 67 to 72
         static constexpr uint8_t signal_path_reset   = 104U; 
         static constexpr uint8_t accel_intr_ctrl     = 105U; 
         // user ctrl bit 7 enable DMP
         // user ctrl bit 3 reset DMP
         static constexpr uint8_t user_ctrl           = 106U;
         static constexpr uint8_t pwr_mgmt1           = 107U;
         static constexpr uint8_t pwr_mgmt2           = 108U;

         // --- DMP specific regs here ---

         static constexpr uint8_t fifo_count          = 114U; // 114 to 115
         static constexpr uint8_t fifo_read_write     = 116U; // 59 to 64
         static constexpr uint8_t whoami              = 117U; 
         static constexpr uint8_t accel_offsets       = 119U; // 119 to 126
      };

      struct product_id{
         static constexpr uint8_t ES_C4 = 0x14;
         static constexpr uint8_t ES_C5 = 0x15;
         static constexpr uint8_t ES_D6 = 0x16;
         static constexpr uint8_t ES_D7 = 0x17;
         static constexpr uint8_t ES_D8 = 0x18;

         static constexpr uint8_t C4 = 0x54;
         static constexpr uint8_t C5 = 0x55;
         static constexpr uint8_t D6 = 0x56;
         static constexpr uint8_t D7 = 0x57;
         static constexpr uint8_t D8 = 0x58;
         static constexpr uint8_t D9 = 0x59;
      };

      static uint8_t dma_tx_buffer[16] ;
      static volatile uint8_t  dma_rx_buffer[16];

      static void setup_pins()
      {
         quan::stm32::module_enable<chip_select::port_type>();
         quan::stm32::apply<
            chip_select
            ,quan::stm32::gpio::mode::output
            ,quan::stm32::gpio::pupd::none
            ,quan::stm32::gpio::ospeed::medium_fast
            ,quan::stm32::gpio::ostate::high
         >();
      }

      static void setup_exti()
      {
         quan::stm32::apply<
            data_ready_irq
            , quan::stm32::gpio::mode::input
            , quan::stm32::gpio::pupd::pull_up // make this pullup ok as mpu6000 is on 3v
         >();
         quan::stm32::module_enable<quan::stm32::syscfg>(); 
         quan::stm32::set_exti_syscfg<data_ready_irq>();
         quan::stm32::set_exti_falling_edge<data_ready_irq>();
         quan::stm32::nvic_enable_exti_irq<data_ready_irq>();
         NVIC_SetPriority(
            quan::stm32::detail::get_exti_irq_num<data_ready_irq::pin_value>::value
            ,14
         );
         //quan::stm32::enable_exti_interrupt<data_ready_irq>(); 
      }

      static void setup_dma()
      {
         // DMA2
         quan::stm32::rcc::get()->ahb1enr |= (1 << 22);
         for ( uint8_t i = 0; i < 20; ++i){
            asm volatile ("nop" : : :);
         }

         // for now we use both tx and rx dma
         // RX
         DMA_Stream_TypeDef * dma_stream = DMA2_Stream0;
         constexpr uint32_t  dma_channel = 3;
         constexpr uint32_t  dma_priority = 0b01; // medium
         dma_stream->CR = (dma_stream->CR & ~(0b111 << 25)) | ( dma_channel << 25); //(CHSEL) select channel
         dma_stream->CR = (dma_stream->CR & ~(0b11 << 16)) | (dma_priority << 16U); // (PL) priority
         dma_stream->CR = (dma_stream->CR & ~(0b11 << 13)) ; // (MSIZE) 8 bit memory transfer
         dma_stream->CR = (dma_stream->CR & ~(0b11 << 11)) ; // (PSIZE) 8 bit transfer
         dma_stream->CR |= (1 << 10);// (MINC)
         dma_stream->CR &= ~(0b11 << 6) ; // (DIR ) peripheral to memory
         dma_stream->CR |= ( 1 << 4) ; // (TCIE)

         dma_stream->PAR = (uint32_t)&SPI1->DR;  // periph addr
         dma_stream->M0AR = (uint32_t)dma_rx_buffer; 
         dma_stream->NDTR = 16;

         NVIC_SetPriority(DMA2_Stream0_IRQn,15); 
         NVIC_EnableIRQ(DMA2_Stream0_IRQn);

         // TX
         dma_stream = DMA2_Stream5;
         constexpr uint32_t  dma_channel1 = 3;
         dma_stream->CR = (dma_stream->CR & ~(0b111 << 25)) | ( dma_channel1 << 25); //(CHSEL) select channel
         dma_stream->CR = (dma_stream->CR & ~(0b11 << 16)) | (dma_priority << 16U); // (PL) priority
         dma_stream->CR = (dma_stream->CR & ~(0b11 << 13)) ; // (MSIZE) 8 bit memory transfer
         dma_stream->CR = (dma_stream->CR & ~(0b11 << 11)) ; // (PSIZE) 8 bit transfer
         dma_stream->CR |= (1 << 10);// (MINC)
         dma_stream->CR = (dma_stream->CR & ~(0b11 << 6)) | (0b01 << 6) ; // (DIR )  memory to peripheral
        // dma_stream->CR |= ( 1 << 4) ; // (TCIE)
         dma_stream->PAR = (uint32_t)&SPI1->DR;  // periph addr
         dma_stream->M0AR = (uint32_t)dma_tx_buffer; 
         dma_stream->NDTR = 16;
         
         DMA2->HIFCR |= ( 0b111101 << 6) ; // Stream 5 clear flags
         DMA2->LIFCR |= ( 0b111101 << 0) ; // Stream 0 clear flags
      }

      static void reg_write( uint8_t r, uint8_t v)
      {
         uint8_t arr[2] = {r, v};
         spi_device_driver::transaction<chip_select>(arr,arr,2U);
      }

      static uint8_t reg_read( uint8_t r)
      {
         uint8_t arr[2] = {static_cast<uint8_t>(r | 0x80),0U};
         spi_device_driver::transaction<chip_select>(arr,arr,2U);
         return arr[1];
      }

      // assumes sample rate == 400, 200, 100, 50 Hz
      static void setup_registers( uint16_t sample_rate_Hz, uint8_t acc_cutoff_Hz, uint8_t gyro_cutoff_Hz)
      {
         h_INS_ArgsQueue = xQueueCreate(1,sizeof(inertial_sensor_args_t *));

         hal_printf("spi_setup\n");

         // reset
         mpu6000::reg_write(mpu6000::reg::pwr_mgmt1, 1U << 7U);
         delay(100);
        
         // wakeup
         mpu6000::reg_write(mpu6000::reg::pwr_mgmt1, 3U);
         delay(100);

         // disable I2C
         mpu6000::reg_write(mpu6000::reg::user_ctrl, 1U << 4U);
         delay(100);

         while (! whoami_test() )
         {
            delay(100);
         }
        
         hal_printf("whaomi succeeded\n");

         mpu6000::reg_write(mpu6000::reg::fifo_enable, 0U);
         delay(1);

         mpu6000::reg_write(mpu6000::reg::config, 0x00);
         delay(1);

         uint32_t const irq_freq_Hz = (sample_rate_Hz == 400)?2000:1000;

         num_irqs_for_update_message = irq_freq_Hz/sample_rate_Hz;

         uint8_t const sample_rate_divider =  static_cast<uint8_t>(8000 / irq_freq_Hz) -1;
         
         mpu6000::reg_write(mpu6000::reg::sample_rate_div, sample_rate_divider);
         delay(1);

         accel_filter.set_cutoff_frequency(irq_freq_Hz,acc_cutoff_Hz);
         gyro_filter.set_cutoff_frequency(irq_freq_Hz,gyro_cutoff_Hz);

         delay(1);

         // bits 4:3
         // available scales = (reg, val}
         //  0  250 deg.s-1
         //  1  500 deg.s-1
         //  2 1000 deg.s-1
         //  3 2000 deg.s-1
         auto get_gyro_reg_val =[](uint32_t gyro_fsr) 
         { return static_cast<uint8_t>(static_cast<uint32_t>(log2(gyro_fsr/250U)) << 3U);};

         mpu6000::reg_write(mpu6000::reg::gyro_config, get_gyro_reg_val(gyro_fsr_deg_s));

         delay(1);

         // accel reg value dependent on produxct version
         // want a fsr of 8g
         uint8_t const product_id = mpu6000::reg_read(mpu6000::reg::product_id);

   //      generic TODO
   //      uint8_t get_accel_reg_bits [] (uint32_t accel_fsr)
   //      { return static_cast<uint8_t>(static_cast<uint32_t>(logs2(accel_fsr/2)) & 0b11);};
   //      
         // for 8g accel fsr
         switch( product_id){
            case mpu6000::product_id::C4:
            case mpu6000::product_id::C5:
            case mpu6000::product_id::ES_C4:
            case mpu6000::product_id::ES_C5:
               mpu6000::reg_write(mpu6000::reg::accel_config, 1 << 3);
               break;
            default:
               mpu6000::reg_write(mpu6000::reg::accel_config, 1 << 4);
               break;
         }
         delay(1);

         // want active low     bit 7 = true
         // hold until cleared   bit 5 = true
         mpu6000::reg_write(mpu6000::reg::intr_bypass_en_cfg, 0b10100000);
         delay(1);

         // Should be initialised at startup
         // so check init linker flags etc
         mpu6000::dma_tx_buffer[0] = (mpu6000::reg::intr_status | 0x80);
         memset(mpu6000::dma_tx_buffer+1,0,15);

         hal_printf("imu init done\n");

         spi_device_driver::enable_dma();
   //####################################
         taskENTER_CRITICAL();
         quan::stm32::disable_exti_interrupt<mpu6000::data_ready_irq>(); 
         quan::stm32::clear_event_pending<mpu6000::data_ready_irq>();
         mpu6000::reg_write(mpu6000::reg::intr_enable, 0b00000001);
   // TODO get correct enum for setting bus speed
         spi_device_driver::set_bus_speed(1) ;
   //##########################################
         quan::stm32::enable_exti_interrupt<mpu6000::data_ready_irq>(); 
         taskEXIT_CRITICAL();
      }

      static bool whoami_test()
      {
         uint8_t value = mpu6000::reg_read(mpu6000::reg::whoami);

         if ( value == mpu6000::val::whoami){
            return true;
         }else{
            hal_printf("whoami failed\n");
            return false;
         }
      }
   }; // mpu6000

   void mpu6000_init()
   {
      mpu6000::init();
   }

   volatile uint8_t  mpu6000::dma_rx_buffer[16] __attribute__((section(".telem_buffer"))) = {0};
   uint8_t mpu6000::dma_tx_buffer[16] __attribute__((section(".telem_buffer"))) = {
      (mpu6000::reg::intr_status | 0x80),0,0,0,
      0,0,0,0,
      0,0,0,0,
      0,0,0,0
   };

   mpu6000::lp_filter  mpu6000::gyro_filter{};
   mpu6000::lp_filter  mpu6000::accel_filter{};

   uint32_t mpu6000::num_irqs_for_update_message = 1;
}

namespace Quan{ 

   void init_spi()
   {
     spi_device_driver::init();
     delay(100);
   }

namespace detail{

   void spi_setup(uint16_t sample_rate_Hz, uint8_t acc_cutoff_Hz, uint8_t gyro_cutoff_Hz)
   {
      mpu6000::setup_registers(sample_rate_Hz, acc_cutoff_Hz,gyro_cutoff_Hz);
   }

}} // Quan::detail

// Interrupt from MPU6000
extern "C" void EXTI15_10_IRQHandler() __attribute__ ((interrupt ("IRQ")));
extern "C" void EXTI15_10_IRQHandler()
{      
   // if data has been read by app then switch buffers
   // structure for accumulating samples
   // v filter
   if (quan::stm32::is_event_pending<mpu6000::data_ready_irq>()){

      spi_device_driver::cs_assert<mpu6000::chip_select>(); // start transaction
      spi_device_driver::stop_spi();
      
      quan::stm32::clear_event_pending<mpu6000::data_ready_irq>();
       
      DMA2_Stream5->NDTR = 16; // TX
      DMA2_Stream0->NDTR = 16; // RX

      DMA2_Stream0->CR |= (1 << 0); // (EN) enable DMA rx
      DMA2_Stream5->CR |= (1 << 0); // (EN) enable DMA  tx
      spi_device_driver::start_spi();
     // hal_printf("£");
   }else{
      panic("unknown EXTI15_10 event\n");
   }
}

namespace {

    mpu6000::inertial_sensor_args_t imu_args;
    volatile uint32_t irq_count = 0;
}

namespace Quan{

    bool eeprom_write(unsigned short, void const*, unsigned int) {return false;}
    bool eeprom_read(void*, unsigned short, unsigned int){return false;}

   // called by AP_InertailSensor::wait_for_sample
   // blocks 
   bool wait_for_imu_sample(uint32_t usecs_to_wait)
   {
      if (h_INS_ArgsQueue != nullptr){
         mpu6000::inertial_sensor_args_t * p_dummy_result;
         TickType_t const ms_to_wait = usecs_to_wait / 1000  + (((usecs_to_wait % 1000) > 499 )?1:0);
         return xQueuePeek(h_INS_ArgsQueue,&p_dummy_result,ms_to_wait) == pdTRUE ;
      }else{
         return false;
      }
   }

   // called by AP_inertialSensor_Backend::update ( quan version)
   // after AP_InertailSensor::wait_for_sample has unblocked
   bool update_ins(Vector3f & accel,Vector3f & gyro)
   {
      if (h_INS_ArgsQueue != nullptr){
         mpu6000::inertial_sensor_args_t * p_args;
         if (xQueueReceive(h_INS_ArgsQueue,&p_args,0) == pdTRUE ){
             accel = p_args->accel;
             gyro = p_args->gyro;
             return true;
         }
      }
      return false;
   }
}

// RX DMA complete
extern "C" void DMA2_Stream0_IRQHandler() __attribute__ ((interrupt ("IRQ")));
extern "C" void DMA2_Stream0_IRQHandler()
{
   quan::stm32::push_FPregs();

   DMA2_Stream5->CR &= ~(1 << 0); // (EN) disable DMA
   DMA2_Stream0->CR &= ~(1 << 0); // (EN) disable DMA

   // get the dma buffer
   volatile uint8_t * arr = mpu6000::dma_rx_buffer;

   // device to convert the buffer values
   // TODO use CMSIS intrinsics
   union{
      uint8_t arr[2];
      int16_t val;
   }u; 

   Vector3f  accel;
   Vector3f  gyro;

   /*
      according to MPU6000 backend::_accumulate()

      accel_out.x  <-- accel_in.y
      accel_out.y  <-- accel_in.x
      accel_out.z  <-- -accel_in.z

      gyro_out.x   <-- gyro_in.y
      gyro_out.y   <-- gyro_in.x
      gyro_out.z   <-- -gyro_in.z
   */

   constexpr float accel_k = mpu6000::accel_constant(mpu6000::accel_fsr_g);
  
   u.arr[1] = arr[2];
   u.arr[0] = arr[3];
   accel.y = u.val * accel_k;

   u.arr[1] = arr[4];
   u.arr[0] = arr[5];
   accel.x = u.val * accel_k;

   u.arr[1] = arr[6];
   u.arr[0] = arr[7];
   accel.z = -u.val * accel_k ;

   constexpr float gyro_k = mpu6000::gyro_constant(mpu6000::gyro_fsr_deg_s);

   u.arr[1] = arr[10];
   u.arr[0] = arr[11];
   gyro.y = u.val * gyro_k;

   u.arr[1] = arr[12];
   u.arr[0] = arr[13];
   gyro.x = u.val * gyro_k;

   u.arr[1] = arr[14];
   u.arr[0] = arr[15];
   gyro.z = -u.val * gyro_k;

   while( (DMA2_Stream5->CR | DMA2_Stream0->CR ) & (1 << 0) ){;}
 
   DMA2->HIFCR |= ( 0b111101 << 6) ; // Stream 5 clear flags
   DMA2->LIFCR |= ( 0b111101 << 0) ; // Stream 0 clear flags

   spi_device_driver::cs_release<mpu6000::chip_select>();

   BaseType_t HigherPriorityTaskWoken_imu = pdFALSE;

   if ( ++irq_count == mpu6000::num_irqs_for_update_message){
       irq_count = 0;
       if ( xQueueIsQueueEmptyFromISR(h_INS_ArgsQueue)){
          imu_args.accel = mpu6000::accel_filter.apply(accel);
          imu_args.gyro = mpu6000::gyro_filter.apply(gyro);
          mpu6000::inertial_sensor_args_t*  p_imu_args = &imu_args;
          xQueueSendToBackFromISR(h_INS_ArgsQueue,&p_imu_args,&HigherPriorityTaskWoken_imu);
      }else{
         // message that apm task missed it
         // this may be ok at startup though
         
      }
   }else{
      // we dont need to go through the whole rigmarole
      // just update the filter
       mpu6000::accel_filter.apply(accel);
       mpu6000::gyro_filter.apply(gyro);
   }
   quan::stm32::pop_FPregs();
   portEND_SWITCHING_ISR(HigherPriorityTaskWoken_imu);
}

#endif // CONFIG_HAL_BOARD == HAL_BOARD_QUAN
