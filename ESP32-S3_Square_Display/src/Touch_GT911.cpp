#include "Touch_GT911.h"
#include <Preferences.h>

// Runtime GT911 I2C address — set by Touch_Init() auto-detect
uint8_t gt911_addr = GT911_ADDR_PRIMARY;

struct GT911_Touch touch_data = {0};

// Rate-limit noisy I2C error messages (every 5 seconds max)
static unsigned long last_i2c_err_log = 0;
#define I2C_ERR_LOG_INTERVAL_MS 5000


bool I2C_Read_Touch(uint8_t Driver_addr, uint16_t Reg_addr, uint8_t *Reg_data, uint32_t Length)
{
  Wire.beginTransmission(Driver_addr);
  Wire.write((uint8_t)(Reg_addr >> 8)); 
  Wire.write((uint8_t)Reg_addr);         
  if ( Wire.endTransmission(true)){
    unsigned long now = millis();
    if (now - last_i2c_err_log >= I2C_ERR_LOG_INTERVAL_MS) {
      last_i2c_err_log = now;
      printf("[TOUCH] I2C Read failed (addr=0x%02X, reg=0x%04X)\r\n", Driver_addr, Reg_addr);
    }
    return false;
  }
  Wire.requestFrom(Driver_addr, Length);
  for (int i = 0; i < Length; i++) {
    *Reg_data++ = Wire.read();
  }
  return true;
}
bool I2C_Write_Touch(uint8_t Driver_addr, uint16_t Reg_addr, const uint8_t *Reg_data, uint32_t Length)
{
  Wire.beginTransmission(Driver_addr);
  Wire.write((uint8_t)(Reg_addr >> 8));
  Wire.write((uint8_t)Reg_addr);        
  for (int i = 0; i < Length; i++) {
    Wire.write(*Reg_data++);
  }
  if ( Wire.endTransmission(true))
  {
    unsigned long now = millis();
    if (now - last_i2c_err_log >= I2C_ERR_LOG_INTERVAL_MS) {
      last_i2c_err_log = now;
      printf("[TOUCH] I2C Write failed (addr=0x%02X, reg=0x%04X)\r\n", Driver_addr, Reg_addr);
    }
    return false;
  }
  return true;
}

uint8_t Touch_Init(void) {

  GT911_Touch_Reset();

  // Check NVS for a cached GT911 address from a previous successful probe.
  // After a soft reset (crash-reboot) the GT911 may not respond to probe
  // because it didn't get a power-cycle. Use the cached address in that case.
  Preferences prefs;
  uint8_t cached_addr = 0;
  if (prefs.begin("settings", true)) {
    cached_addr = prefs.getUChar("touch_addr", 0);
    prefs.end();
  }

  // Auto-detect GT911 I2C address (v3=0x5D, v4=0x14)
  bool probed = false;
  Wire.beginTransmission(GT911_ADDR_PRIMARY);
  if (Wire.endTransmission() == 0) {
    gt911_addr = GT911_ADDR_PRIMARY;
    probed = true;
    printf("[TOUCH] GT911 found at 0x%02X (v3 board)\n", gt911_addr);
  } else {
    Wire.beginTransmission(GT911_ADDR_SECONDARY);
    if (Wire.endTransmission() == 0) {
      gt911_addr = GT911_ADDR_SECONDARY;
      probed = true;
      printf("[TOUCH] GT911 found at 0x%02X (v4 board)\n", gt911_addr);
    } else if (cached_addr == GT911_ADDR_PRIMARY || cached_addr == GT911_ADDR_SECONDARY) {
      // GT911 didn't respond (probably soft reset without power cycle).
      // Use the cached address from a previous successful probe.
      gt911_addr = cached_addr;
      printf("[TOUCH] GT911 not responding, using cached addr 0x%02X\n", gt911_addr);
    } else {
      gt911_addr = GT911_ADDR_PRIMARY;
      printf("[TOUCH] GT911 not found, no cache, defaulting to 0x%02X\n", gt911_addr);
    }
  }

  // Cache the address so crash-reboots use the right one
  if (probed) {
    if (prefs.begin("settings", false)) {
      if (prefs.getUChar("touch_addr", 0) != gt911_addr) {
        prefs.putUChar("touch_addr", gt911_addr);
        printf("[TOUCH] Cached touch_addr=0x%02X to NVS\n", gt911_addr);
      }
      prefs.end();
    }
  }

  GT911_Read_cfg();

  attachInterrupt(GT911_INT_PIN, Touch_GT911_ISR, interrupt); 

  return true;
}
/* Reset controller */
uint8_t GT911_Touch_Reset(void)
{
  printf("[TOUCH] GT911_Touch_Reset: board=%s, expander=0x%02X\n",
         is_board_v4() ? "v4" : "v3", g_tca9554_address);

  // Drive INT pin LOW before releasing reset → GT911 latches I2C address 0x5D
  pinMode(GT911_INT_PIN, OUTPUT);                   
  digitalWrite(GT911_INT_PIN, LOW);                  

  Set_EXIO(EXIO_PIN1,Low);
  vTaskDelay(pdMS_TO_TICKS(10));
  Set_EXIO(EXIO_PIN1,High);
  vTaskDelay(pdMS_TO_TICKS(200));

  digitalWrite(GT911_INT_PIN, HIGH);                
  pinMode(GT911_INT_PIN, INPUT);                     

  printf("[TOUCH] GT911_Touch_Reset: done\n");
  return true;
}
void GT911_Read_cfg(void) {
  uint8_t buf[4];
  I2C_Read_Touch(GT911_ADDR, ESP_LCD_TOUCH_GT911_PRODUCT_ID_REG, buf, 3);
  printf("TouchPad_ID:0x%02x,0x%02x,0x%02x\r\n", buf[0], buf[1], buf[2]);
  I2C_Read_Touch(GT911_ADDR, ESP_LCD_TOUCH_GT911_CONFIG_REG, buf, 1);
  printf("TouchPad_Config_Version:%d \r\n", buf[0]);
}

// reads sensor and touches
// updates Touch Points, but if not touched, resets all Touch Point Information
uint8_t Touch_Read_Data(void) {
  uint8_t buf[41];
  uint8_t touch_cnt = 0;
  uint8_t clear = 0;
  uint8_t Over = 0xAB;
  size_t i = 0,num=0;
  if (!I2C_Read_Touch(GT911_ADDR, ESP_LCD_TOUCH_GT911_READ_XY_REG, buf, 1)) {
    return true; // I2C failed — don't process garbage data
  }
  if ((buf[0] & 0x80) == 0x00) {                                              
    I2C_Write_Touch(GT911_ADDR, ESP_LCD_TOUCH_GT911_READ_XY_REG, &clear, 1);  // No touch data
  } else {
    /* Count of touched points */
    touch_cnt = buf[0] & 0x0F;
    if (touch_cnt > GT911_LCD_TOUCH_MAX_POINTS || touch_cnt == 0) {
      I2C_Write_Touch(GT911_ADDR, ESP_LCD_TOUCH_GT911_READ_XY_REG, &clear, 1);
      return true;
    }
    /* Read all points */
    if (!I2C_Read_Touch(GT911_ADDR, ESP_LCD_TOUCH_GT911_READ_XY_REG+1, &buf[1], touch_cnt * 8)) {
      I2C_Write_Touch(GT911_ADDR, ESP_LCD_TOUCH_GT911_READ_XY_REG, &clear, 1);
      return true; // I2C failed — discard partial read
    }
    /* Clear all */
    I2C_Write_Touch(GT911_ADDR, ESP_LCD_TOUCH_GT911_READ_XY_REG, &clear, 1);
    // printf(" points=%d \r\n",touch_cnt);
    noInterrupts(); 

    /* Number of touched points */
    if(touch_cnt > GT911_LCD_TOUCH_MAX_POINTS)
        touch_cnt = GT911_LCD_TOUCH_MAX_POINTS;
    touch_data.points = (uint8_t)touch_cnt;
    /* Fill all coordinates */
    for (i = 0; i < touch_cnt; i++) {
      touch_data.coords[i].x = (uint16_t)(((uint16_t)buf[(i * 8) + 3] << 8) + buf[(i * 8) + 2]);               
      touch_data.coords[i].y = (uint16_t)(((uint16_t)buf[(i * 8) + 5] << 8) + buf[(i * 8) + 4]);;
      touch_data.coords[i].strength = (uint16_t)(((uint16_t)buf[(i * 8) + 7] << 8) + buf[(i * 8) + 6]);
    }
    interrupts(); 
    // printf(" points=%d \r\n",touch_data.points);
  }
  return true;
}
void Touch_Loop(void){
  if(Touch_interrupts){
    Touch_interrupts = false;
    example_touchpad_read();
  }
}
uint8_t Touch_Get_XY(uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num) {

  assert(x != NULL);
  assert(y != NULL);
  assert(point_num != NULL);
  assert(max_point_num > 0);
  
  noInterrupts(); 
  /* Count of points */
  if(touch_data.points > max_point_num)
    touch_data.points = max_point_num;
  for (size_t i = 0; i < touch_data.points; i++) {
      x[i] = touch_data.coords[i].x;
      y[i] = touch_data.coords[i].y;
      if (strength) {
          strength[i] = touch_data.coords[i].strength;
      }
  }
  *point_num = touch_data.points;
  /* Invalidate */
  touch_data.points = 0;
  interrupts(); 
  return (*point_num > 0);
}
void example_touchpad_read(void){
  uint16_t touchpad_x[GT911_LCD_TOUCH_MAX_POINTS] = {0};
  uint16_t touchpad_y[GT911_LCD_TOUCH_MAX_POINTS] = {0};
  uint16_t strength[GT911_LCD_TOUCH_MAX_POINTS]   = {0};
  uint8_t touchpad_cnt = 0;
  Touch_Read_Data();
  uint8_t touchpad_pressed = Touch_Get_XY(touchpad_x, touchpad_y, strength, &touchpad_cnt, GT911_LCD_TOUCH_MAX_POINTS);
  if (touchpad_pressed && touchpad_cnt > 0) {
      // data->point.x = touchpad_x[0];
      // data->point.y = touchpad_y[0];
      // data->state = LV_INDEV_STATE_PR;
      printf("Touch : X=%u Y=%u num=%d\r\n", touchpad_x[0], touchpad_y[0],touchpad_cnt);
  } else {
      // data->state = LV_INDEV_STATE_REL;
  }
}
/*!
    @brief  handle interrupts
*/
uint8_t Touch_interrupts;
void IRAM_ATTR Touch_GT911_ISR(void) {
  Touch_interrupts = true;
}
