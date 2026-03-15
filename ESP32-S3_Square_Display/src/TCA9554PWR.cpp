#include "TCA9554PWR.h"
#include <Arduino.h>
#include <Preferences.h>

// ets_printf writes to hardware UART0 even before USB CDC Serial is up
extern "C" int ets_printf(const char *fmt, ...);

// Runtime I2C address for the IO expander (default v3=0x20, v4=0x24)
uint8_t g_tca9554_address = TCA9554_ADDR_V3;
static bool g_board_v4 = true;

bool detect_expander_address()
{
  // Check NVS for a cached board version from a previous successful probe.
  // If found, skip I2C probing (which can leave the expander in a bad state
  // on v4 boards and crash the display).
  Preferences prefs;
  uint8_t cached = 0xFF; // 0xFF = not yet probed
  if (prefs.begin("settings", true)) {
    cached = prefs.getUChar("board_ver", 0xFF);
    prefs.end();
  }

  if (cached == 0) {
    g_tca9554_address = TCA9554_ADDR_V3;
    g_board_v4 = false;
    ets_printf("[BOARD] Cached: v3 (expander 0x20)\r\n");
    return true;
  }
  if (cached == 1) {
    g_tca9554_address = TCA9554_ADDR_V4;
    g_board_v4 = true;
    ets_printf("[BOARD] Cached: v4 (expander 0x24)\r\n");
    return true;
  }

  // First boot: probe both addresses to auto-detect.
  // This may leave the expander in a bad state on v4, but the result is
  // persisted so it only happens once (display may crash → auto-reboot → cached).
  ets_printf("[BOARD] First boot: probing I2C for board version...\r\n");
  Wire.beginTransmission(TCA9554_ADDR_V4);
  uint8_t err_v4 = Wire.endTransmission();
  Wire.beginTransmission(TCA9554_ADDR_V3);
  uint8_t err_v3 = Wire.endTransmission();
  ets_printf("[BOARD] Probe 0x20 (v3): %s, 0x24 (v4): %s\r\n",
             err_v3 == 0 ? "ACK" : "NACK",
             err_v4 == 0 ? "ACK" : "NACK");

  uint8_t detected = 0; // default v3
  if (err_v4 == 0) {
    g_tca9554_address = TCA9554_ADDR_V4;
    g_board_v4 = true;
    detected = 1;
    ets_printf("[BOARD] Auto-detected v4 IO expander at 0x24\r\n");
  } else if (err_v3 == 0) {
    g_tca9554_address = TCA9554_ADDR_V3;
    g_board_v4 = false;
    detected = 0;
    ets_printf("[BOARD] Auto-detected v3 IO expander at 0x20\r\n");
  } else {
    ets_printf("[BOARD] WARNING: No expander found, defaulting to v3 (0x20)\r\n");
    g_tca9554_address = TCA9554_ADDR_V3;
    g_board_v4 = false;
    detected = 0;
  }

  // Cache the result so we never probe again
  if (prefs.begin("settings", false)) {
    prefs.putUChar("board_ver", detected);
    prefs.end();
    ets_printf("[BOARD] Cached board_ver=%d to NVS\r\n", detected);
  }
  return true;
}

bool is_board_v4()
{
  return g_board_v4;
}

/*****************************************************  Operation register REG   ****************************************************/   
uint8_t I2C_Read_EXIO(uint8_t REG)                             // Read the value of the TCA9554PWR register REG
{
  Wire.beginTransmission(TCA9554_ADDRESS);                
  Wire.write(REG);                                        
  uint8_t result = Wire.endTransmission();               
  if (result != 0) {                                     
    if (!g_board_v4) {printf("Data Transfer Failure !!!\r\n");}
  }
  Wire.requestFrom((uint8_t)TCA9554_ADDRESS, (uint8_t)1);                   
  uint8_t bitsStatus = Wire.read();                        
  return bitsStatus;                                     
}
uint8_t I2C_Write_EXIO(uint8_t REG,uint8_t Data)              // Write Data to the REG register of the TCA9554PWR
{
  Wire.beginTransmission(TCA9554_ADDRESS);                
  Wire.write(REG);                                        
  Wire.write(Data);                                       
  uint8_t result = Wire.endTransmission();                  
  if (result != 0) {    
    if (!g_board_v4) {printf("Data write failure!!!\r\n");}
    return -1;
  }
  return 0;                                             
}
/********************************************************** Set EXIO mode **********************************************************/       
void Mode_EXIO(uint8_t Pin,uint8_t State)                 // Set the mode of the TCA9554PWR Pin. State: 0= Output mode 1= Input mode
{
  uint8_t bitsStatus = I2C_Read_EXIO(TCA9554_CONFIG_REG);
  uint8_t Data;
  if (State == 1)
    Data = (0x01 << (Pin-1)) | bitsStatus;   // set bit = input
  else
    Data = (~(0x01 << (Pin-1))) & bitsStatus; // clear bit = output
  uint8_t result = I2C_Write_EXIO(TCA9554_CONFIG_REG,Data); 
  if (result != 0) { 
    if (!g_board_v4) {printf("I/O Configuration Failure !!!\r\n");}
  }
}
void Mode_EXIOS(uint8_t PinState)                         // Set the mode of the 7 pins from the TCA9554PWR with PinState   
{
  uint8_t result = I2C_Write_EXIO(TCA9554_CONFIG_REG,PinState);  
  if (result != 0) {   
    if (!g_board_v4) {printf("I/O Configuration Failure !!!\r\n");}
  }
}
/********************************************************** Read EXIO status **********************************************************/       
uint8_t Read_EXIO(uint8_t Pin)                            // Read the level of the TCA9554PWR Pin
{
  uint8_t inputBits = I2C_Read_EXIO(TCA9554_INPUT_REG);          
  uint8_t bitStatus = (inputBits >> (Pin-1)) & 0x01; 
  return bitStatus;                                  
}
uint8_t Read_EXIOS(uint8_t REG = TCA9554_INPUT_REG)       // Read the level of all pins of TCA9554PWR, the default read input level state, want to get the current IO output state, pass the parameter TCA9554_OUTPUT_REG, such as Read_EXIOS(TCA9554_OUTPUT_REG);
{
  uint8_t inputBits = I2C_Read_EXIO(REG);                     
  return inputBits;     
}

/********************************************************** Set the EXIO output status **********************************************************/  
void Set_EXIO(uint8_t Pin,uint8_t State)                  // Sets the level state of the Pin without affecting the other pins
{
  uint8_t Data;
  if(State < 2 && Pin < 9 && Pin > 0){  
    uint8_t bitsStatus = Read_EXIOS(TCA9554_OUTPUT_REG);
    if(State == 1)                                     
      Data = (0x01 << (Pin-1)) | bitsStatus; 
    else if(State == 0)                  
      Data = (~(0x01 << (Pin-1))) & bitsStatus;      
    uint8_t result = I2C_Write_EXIO(TCA9554_OUTPUT_REG,Data);  
    if (result != 0) {                         
      if (!g_board_v4) {printf("Failed to set GPIO!!!\r\n");}
    }
  }
  else                                           
    if (!g_board_v4) {printf("Parameter error, please enter the correct parameter!\r\n");}
}
void Set_EXIOS(uint8_t PinState)                          // Set 7 pins to the PinState state such as :PinState=0x23, 0010 0011 state (the highest bit is not used)
{
  uint8_t result = I2C_Write_EXIO(TCA9554_OUTPUT_REG,PinState); 
  if (result != 0) {                  
    if (!g_board_v4) {printf("Failed to set GPIO!!!\r\n");}
  }
}
/********************************************************** Flip EXIO state **********************************************************/  
void Set_Toggle(uint8_t Pin)                              // Flip the level of the TCA9554PWR Pin
{
    uint8_t bitsStatus = Read_EXIO(Pin);                 
    Set_EXIO(Pin,(bool)!bitsStatus); 
}
/********************************************************* TCA9554PWR Initializes the device ***********************************************************/  
void TCA9554PWR_Init(uint8_t PinState)                  // Set the seven pins to PinState state, for example :PinState=0x23, 0010 0011 State  (Output mode or input mode) 0= Output mode 1= Input mode. The default value is output mode
{                  
  Mode_EXIOS(PinState);      
}
