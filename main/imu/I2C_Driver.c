#include "I2C_Driver.h"

#include "esp_check.h"
#include "soc/soc_caps.h"
#define I2C_TRANS_BUF_MINIMUM_SIZE     (sizeof(i2c_cmd_desc_t) + \
                                        sizeof(i2c_cmd_link_t) * 8) /* It is required to have allocate one i2c_cmd_desc_t per command:
                                                                     * start + write (device address) + write buffer +
                                                                     * start + write (device address) + read buffer + read buffer for NACK +
                                                                     * stop */
static const char *I2C_TAG = "I2C";

static bool gpio_valid_required(int pin)
{
    return pin >= 0 && pin < SOC_GPIO_PIN_COUNT;
}
/**
 * @brief i2c master initialization
 */
static esp_err_t i2c_master_init(void)
{
    int i2c_master_port = I2C_MASTER_NUM;

    if (!gpio_valid_required(I2C_Touch_SDA_IO) || !gpio_valid_required(I2C_Touch_SCL_IO)) {
        ESP_LOGW(I2C_TAG, "Invalid I2C GPIOs for this target (sda=%d, scl=%d)",
                 I2C_Touch_SDA_IO, I2C_Touch_SCL_IO);
        return ESP_ERR_NOT_SUPPORTED;
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_Touch_SDA_IO,
        .scl_io_num = I2C_Touch_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    ESP_RETURN_ON_ERROR(i2c_param_config(i2c_master_port, &conf), I2C_TAG, "i2c_param_config failed");

    return i2c_driver_install(i2c_master_port, conf.mode, I2C_MASTER_RX_BUF_DISABLE,
                              I2C_MASTER_TX_BUF_DISABLE, 0);
}
esp_err_t I2C_Init(void)
{
    /********************* I2C *********************/
    esp_err_t ret = i2c_master_init();
    if (ret == ESP_OK) {
        ESP_LOGI(I2C_TAG, "I2C initialized successfully");
    } else {
        ESP_LOGW(I2C_TAG, "I2C init skipped: %s", esp_err_to_name(ret));
    }
    return ret;
}


// Reg addr is 8 bit
esp_err_t I2C_Write(uint8_t Driver_addr, uint8_t Reg_addr, const uint8_t *Reg_data, uint32_t Length)
{
    uint8_t buf[Length+1];
    
    buf[0] = Reg_addr;
    // Copy Reg_data to buf starting at buf[1]
    memcpy(&buf[1], Reg_data, Length);
    return i2c_master_write_to_device(I2C_MASTER_NUM, Driver_addr, buf, Length+1, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
}



esp_err_t I2C_Read(uint8_t Driver_addr, uint8_t Reg_addr, uint8_t *Reg_data, uint32_t Length)
{
    return i2c_master_write_read_device(I2C_MASTER_NUM, Driver_addr, &Reg_addr, 1, Reg_data, Length, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
}
