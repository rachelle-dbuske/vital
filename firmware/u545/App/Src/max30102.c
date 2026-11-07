#include "sensor.h"
#include "i2c.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stddef.h>
#include <stdint.h>

#define MAX30102_ADDRESS              (0x57U << 1)
#define MAX30102_I2C_TIMEOUT_MS        10U
#define MAX30102_RESET_TIMEOUT_MS      50U
#define MAX30102_SAMPLE_TIMEOUT_MS     20U

#define MAX30102_REG_FIFO_WR_PTR       0x04U
#define MAX30102_REG_FIFO_RD_PTR       0x06U
#define MAX30102_REG_FIFO_DATA         0x07U
#define MAX30102_REG_FIFO_CONFIG       0x08U
#define MAX30102_REG_MODE_CONFIG       0x09U
#define MAX30102_REG_SPO2_CONFIG       0x0AU
#define MAX30102_REG_LED1_PA           0x0CU
#define MAX30102_REG_LED2_PA           0x0DU
#define MAX30102_REG_PART_ID           0xFFU

#define MAX30102_PART_ID               0x15U
#define MAX30102_MODE_RESET            0x40U
#define MAX30102_MODE_SPO2             0x03U
#define MAX30102_FIFO_POINTER_MASK     0x1FU
#define MAX30102_FIFO_CONFIG           0x0FU
#define MAX30102_SPO2_CONFIG           0x27U
#define MAX30102_LED_CURRENT           0x24U
#define MAX30102_FIFO_SAMPLE_BYTES     6U

static HAL_StatusTypeDef read_registers(uint8_t reg, uint8_t *data, uint16_t size)
{
    return HAL_I2C_Mem_Read(&hi2c1, MAX30102_ADDRESS, reg, I2C_MEMADD_SIZE_8BIT,
                            data, size, MAX30102_I2C_TIMEOUT_MS);
}

static HAL_StatusTypeDef write_register(uint8_t reg, uint8_t value)
{
    return HAL_I2C_Mem_Write(&hi2c1, MAX30102_ADDRESS, reg, I2C_MEMADD_SIZE_8BIT,
                             &value, 1U, MAX30102_I2C_TIMEOUT_MS);
}

static HAL_StatusTypeDef clear_fifo(void)
{
    if (write_register(MAX30102_REG_FIFO_WR_PTR, 0U) != HAL_OK ||
        write_register(0x05U, 0U) != HAL_OK ||
        write_register(MAX30102_REG_FIFO_RD_PTR, 0U) != HAL_OK) {
        return HAL_ERROR;
    }
    return HAL_OK;
}

sensor_status_t sensor_init(void)
{
    uint8_t part_id;
    if (read_registers(MAX30102_REG_PART_ID, &part_id, 1U) != HAL_OK) {
        return SENSOR_ERR_COMM;
    }
    if (part_id != MAX30102_PART_ID) {
        return SENSOR_ERR_NOT_FOUND;
    }

    if (write_register(MAX30102_REG_MODE_CONFIG, MAX30102_MODE_RESET) != HAL_OK) {
        return SENSOR_ERR_COMM;
    }

    const uint32_t reset_start = HAL_GetTick();
    for (;;) {
        uint8_t mode;
        if (read_registers(MAX30102_REG_MODE_CONFIG, &mode, 1U) != HAL_OK) {
            return SENSOR_ERR_COMM;
        }
        if ((mode & MAX30102_MODE_RESET) == 0U) {
            break;
        }
        if ((HAL_GetTick() - reset_start) >= MAX30102_RESET_TIMEOUT_MS) {
            return SENSOR_ERR_COMM;
        }
        vTaskDelay(pdMS_TO_TICKS(1U));
    }

    if (write_register(MAX30102_REG_FIFO_CONFIG, MAX30102_FIFO_CONFIG) != HAL_OK ||
        write_register(MAX30102_REG_SPO2_CONFIG, MAX30102_SPO2_CONFIG) != HAL_OK ||
        write_register(MAX30102_REG_LED1_PA, MAX30102_LED_CURRENT) != HAL_OK ||
        write_register(MAX30102_REG_LED2_PA, MAX30102_LED_CURRENT) != HAL_OK ||
        clear_fifo() != HAL_OK ||
        write_register(MAX30102_REG_MODE_CONFIG, MAX30102_MODE_SPO2) != HAL_OK) {
        return SENSOR_ERR_COMM;
    }

    return SENSOR_OK;
}

sensor_status_t sensor_read(sensor_sample_t *out)
{
    if (out == NULL) {
        return SENSOR_ERR_COMM;
    }

    const uint32_t wait_start = HAL_GetTick();
    for (;;) {
        uint8_t write_ptr;
        uint8_t read_ptr;
        if (read_registers(MAX30102_REG_FIFO_WR_PTR, &write_ptr, 1U) != HAL_OK ||
            read_registers(MAX30102_REG_FIFO_RD_PTR, &read_ptr, 1U) != HAL_OK) {
            return SENSOR_ERR_COMM;
        }

        if ((write_ptr & MAX30102_FIFO_POINTER_MASK) !=
            (read_ptr & MAX30102_FIFO_POINTER_MASK)) {
            break;
        }

        if ((HAL_GetTick() - wait_start) >= MAX30102_SAMPLE_TIMEOUT_MS) {
            return SENSOR_ERR_COMM;
        }
        vTaskDelay(pdMS_TO_TICKS(1U));
    }

    uint8_t fifo_data[MAX30102_FIFO_SAMPLE_BYTES];
    if (read_registers(MAX30102_REG_FIFO_DATA, fifo_data,
                       (uint16_t)sizeof(fifo_data)) != HAL_OK) {
        return SENSOR_ERR_COMM;
    }

    out->red = ((uint32_t)(fifo_data[0] & 0x03U) << 16) |
               ((uint32_t)fifo_data[1] << 8) | (uint32_t)fifo_data[2];
    out->ir = ((uint32_t)(fifo_data[3] & 0x03U) << 16) |
              ((uint32_t)fifo_data[4] << 8) | (uint32_t)fifo_data[5];
    return SENSOR_OK;
}