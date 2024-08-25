/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_check.h"

#include "driver/gpio.h"

#include "freertos/queue.h"
#include "driver/dac_continuous.h"

#define YELLOW_CABLE_GPIO_PIN GPIO_NUM_16
#define ORANGE_CABLE_GPIO_PIN GPIO_NUM_17

#define LED_GPIO_PIN GPIO_NUM_2

static const char *TAG = "dac_audio";
#define DAC_FREQ_HZ 16000

#include "diesel_running_mono_8bit_16kHz_audio.h"
#include "train_horn_long_mono_8bit_16kHz_audio.h"
#include "diesel_engine_does_not_start_short_mono_8bit_16kHz_audio.h"

static bool IRAM_ATTR dac_on_convert_done_callback(dac_continuous_handle_t handle, const dac_event_data_t *event, void *user_data)
{
    QueueHandle_t que = (QueueHandle_t)user_data;
    BaseType_t need_awoke;
    /* When the queue is full, drop the oldest item */
    if (xQueueIsQueueFullFromISR(que)) {
        dac_event_data_t dummy;
        xQueueReceiveFromISR(que, &dummy, &need_awoke);
    }
    /* Send the event from callback */
    xQueueSendFromISR(que, event, &need_awoke);
    return need_awoke;
}

typedef struct dac_params
{
	dac_continuous_handle_t handle;
	QueueHandle_t que;
    
} dac_params_t;

static void configure_gpio() {
    ESP_LOGI("config_gpio", "starting");

    gpio_config_t io_conf = {};

    // Configure input buttons

    ESP_LOGI("config_gpio", "input buttons");

    io_conf.pin_bit_mask = ((1ULL << YELLOW_CABLE_GPIO_PIN) | (1ULL << ORANGE_CABLE_GPIO_PIN));
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_POSEDGE;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;

    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // Configure LED
    
    ESP_LOGI("config_gpio", "output LED");

    io_conf.pin_bit_mask = (1ULL << LED_GPIO_PIN);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.intr_type = GPIO_INTR_POSEDGE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    
    ESP_ERROR_CHECK(gpio_config(&io_conf));
}

dac_params_t configure_dac() {
    ESP_LOGI("config_dac", "starting");

    dac_continuous_handle_t handle;
    dac_continuous_config_t cont_cfg = {
        .chan_mask = DAC_CHANNEL_MASK_CH0,
        .desc_num = 8,
        .buf_size = 128,
        .freq_hz = DAC_FREQ_HZ,
        .offset = 0,
        .clk_src = DAC_DIGI_CLK_SRC_APLL,
        .chan_mode = DAC_CHANNEL_MODE_SIMUL,
    };

    ESP_LOGI(TAG, "--------------------------------------------------");
    ESP_LOGI(TAG, "DAC continuous output by DMA");
    ESP_LOGI(TAG, "DAC channel 0 io: GPIO_NUM_%d", DAC_CHANNEL_MASK_CH0);
    ESP_LOGI(TAG, "DAC conversion frequency (Hz): %"PRIu32, cont_cfg.freq_hz);
    ESP_LOGI(TAG, "--------------------------------------------------");

    ESP_ERROR_CHECK(dac_continuous_new_channels(&cont_cfg, &handle));
    ESP_ERROR_CHECK(dac_continuous_enable(handle));

    ESP_LOGI("config_dac", "DAC initialized success, DAC DMA is ready");

    /* Create a queue to transport the interrupt event data */
    QueueHandle_t que = xQueueCreate(10, sizeof(dac_event_data_t));
    assert(que);
    dac_event_callbacks_t cbs = {
        .on_convert_done = dac_on_convert_done_callback,
        .on_stop = NULL,
    };
    /* Must register the callback if using asynchronous writing */
    ESP_ERROR_CHECK(dac_continuous_register_event_callback(handle, &cbs, que));

    ESP_ERROR_CHECK(dac_continuous_start_async_writing(handle));

    dac_params_t params;
    params.handle = handle;
    params.que = que;

    return params;
}

static void print_config() {
    printf("configUSE_PREEMPTION %d\n", configUSE_PREEMPTION);
    printf("CONFIG_FREERTOS_HZ %d\n", CONFIG_FREERTOS_HZ);
    printf("portTICK_PERIOD_MS %ld\n", portTICK_PERIOD_MS);
}

static void dac_write_data_asynchronously(dac_continuous_handle_t handle, QueueHandle_t que, uint8_t *data, size_t data_size)
{
    ESP_LOGI("dac_write_data_asynchronously", "starting");
    ESP_LOGI(TAG, "Audio size %d bytes, played at frequency %d Hz asynchronously", data_size, DAC_FREQ_HZ);
    uint32_t cnt = 1;
    while (1) {
        ESP_LOGI("dac_write_data_asynchronously", "play count %"PRIu32"", cnt++);
        dac_event_data_t evt_data;
        size_t byte_written = 0;
        /* Receive the event from callback and load the data into the DMA buffer until the whole audio loaded */
        while (byte_written < data_size) {
            xQueueReceive(que, &evt_data, portMAX_DELAY);
            size_t loaded_bytes = 0;
            ESP_ERROR_CHECK(dac_continuous_write_asynchronously(handle, evt_data.buf, evt_data.buf_size,
                                                                data + byte_written, data_size - byte_written, &loaded_bytes));
            byte_written += loaded_bytes;
            taskYIELD();
        }
    }
}

static void dac_write_data_asynchronously_single(dac_continuous_handle_t handle, QueueHandle_t que, uint8_t *data, size_t data_size)
{
    gpio_set_level(LED_GPIO_PIN, 1);
    ESP_LOGI("dac_write_data_asynchronously_single", "starting");
    ESP_LOGI("dac_write_data_asynchronously_single", "Audio size %d bytes, played at frequency %d Hz asynchronously", data_size, DAC_FREQ_HZ);
    dac_event_data_t evt_data;
    size_t byte_written = 0;
    /* Receive the event from callback and load the data into the DMA buffer until the whole audio loaded */
    while (byte_written < data_size) {
        xQueueReceive(que, &evt_data, portMAX_DELAY);
        size_t loaded_bytes = 0;
        ESP_ERROR_CHECK(dac_continuous_write_asynchronously(handle, evt_data.buf, evt_data.buf_size,
                                                            data + byte_written, data_size - byte_written, &loaded_bytes));
        byte_written += loaded_bytes;
        taskYIELD();
    }
    gpio_set_level(LED_GPIO_PIN, 0);
}

typedef struct task_params
{
	dac_params_t *dac_params;
	SemaphoreHandle_t sem;
    unsigned char *audio;
    size_t audio_len;
} task_params_t;

static void IRAM_ATTR yellow_cable_gpio_isr_handler(void* arg)
{
    SemaphoreHandle_t sem = (SemaphoreHandle_t)arg;
    xSemaphoreGiveFromISR(sem, NULL);
}

static void IRAM_ATTR orange_cable_gpio_isr_handler(void* arg)
{
    SemaphoreHandle_t sem = (SemaphoreHandle_t)arg;
    xSemaphoreGiveFromISR(sem, NULL);
}

static void prvDACPlayTrainHornSoundTask(void* arg) {
    static const char *tag = "task_dac_play_train_horn_sound";
    task_params_t *params = (task_params_t *)arg;
    SemaphoreHandle_t sem = params->sem;
    dac_continuous_handle_t dac_handle = params->dac_params->handle;
    QueueHandle_t que_handle = params->dac_params->que;
    uint8_t *audio = params->audio;
    size_t audio_len = params->audio_len;
    while(1) {
        xSemaphoreTake(sem, portMAX_DELAY);
        ESP_LOGI(tag, "got semaphore");
        ESP_LOGI(tag, "dac handle ptr %p", dac_handle);
        ESP_LOGI(tag, "queue handle ptr %p", que_handle);
        
        dac_write_data_asynchronously_single(dac_handle, que_handle, audio, audio_len);
    }
    xSemaphoreGive(sem);
    vTaskDelete(NULL);
}

static void prvDACPlayDieselDoesNotStartSoundTask(void* arg) {
    static const char *tag = "task_dac_play_train_horn_sound";
    task_params_t *params = (task_params_t *)arg;
    SemaphoreHandle_t sem = params->sem;
    dac_continuous_handle_t dac_handle = params->dac_params->handle;
    QueueHandle_t que_handle = params->dac_params->que;
    uint8_t *audio = params->audio;
    size_t audio_len = params->audio_len;
    while(1) {
        xSemaphoreTake(sem, portMAX_DELAY);
        ESP_LOGI(tag, "got semaphore");
        ESP_LOGI(tag, "dac handle ptr %p", dac_handle);
        ESP_LOGI(tag, "queue handle ptr %p", que_handle);
        
        for (int i = 0; i < 1; ++i) {
            dac_write_data_asynchronously_single(dac_handle, que_handle, audio, audio_len);
        }
    }
    xSemaphoreGive(sem);
    vTaskDelete(NULL);
}

static void prvDACPlayDieselEngineSoundTask(void *pvParameters) {
    task_params_t *params = (task_params_t *)pvParameters;
    while (1) {
        dac_write_data_asynchronously(params->dac_params->handle, params->dac_params->que, (uint8_t *)params->audio, params->audio_len);
    }
}

void app_main(void) {
    configure_gpio();
    dac_params_t dac_params = configure_dac();

    print_config();

    task_params_t t3p = {
        .dac_params = &dac_params, 
        .sem = xSemaphoreCreateBinary(),
        .audio = diesel_engine_does_not_start_short_mono_8bit_16kHz_wav,
        .audio_len = diesel_engine_does_not_start_short_mono_8bit_16kHz_wav_len,
    };

    for (int i = 0; i < 2; ++i) {
        dac_write_data_asynchronously_single(t3p.dac_params->handle, t3p.dac_params->que, t3p.audio, t3p.audio_len);
    }

    xTaskCreate(prvDACPlayDieselDoesNotStartSoundTask, "dac_play_diesel_does_not_start", 2048, &t3p, 10, NULL);

    task_params_t t2p = {
        .dac_params = &dac_params, 
        .sem = xSemaphoreCreateBinary(),
        .audio = train_horn_long_mono_8bit_16kHz_wav,
        .audio_len = train_horn_long_mono_8bit_16kHz_wav_len,
    };

    xTaskCreate(prvDACPlayTrainHornSoundTask, "dac_play_train_horn", 2048, &t2p, 15, NULL);

    task_params_t t1p = {
        .dac_params = &dac_params, 
        .audio = diesel_running_mono_8bit_16kHz_wav,
        .audio_len = diesel_running_mono_8bit_16kHz_wav_len,
    };

    xTaskCreate(prvDACPlayDieselEngineSoundTask, "dac_play_diesel_engine", 4096, &t1p, 1, NULL);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(ORANGE_CABLE_GPIO_PIN, orange_cable_gpio_isr_handler, (void*) t2p.sem);
    gpio_isr_handler_add(YELLOW_CABLE_GPIO_PIN, yellow_cable_gpio_isr_handler, (void*) t3p.sem);
}
